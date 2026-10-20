#include "core/update_check.h"

#include "core/log.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")  // game repos link core without listing it

namespace mc::core::update {
namespace {

bool ParseNumber(std::string_view s, unsigned* out) {
    if (s.empty() || s.size() > 9) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    *out = v;
    return true;
}

bool IsNumeric(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        parts.push_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) return parts;
        start = at + 1;
    }
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// ---- a minimal JSON reader: one flat object of the endpoint's answer, nested values skipped ----
struct Json {
    std::string_view s;
    size_t i = 0;

    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool Eat(char c) {
        Ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool String(std::string* out) {
        Ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        std::string v;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') {
                if (out) *out = std::move(v);
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                v += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
            case '"': v += '"'; break;
            case '\\': v += '\\'; break;
            case '/': v += '/'; break;
            case 'b': v += '\b'; break;
            case 'f': v += '\f'; break;
            case 'n': v += '\n'; break;
            case 'r': v += '\r'; break;
            case 't': v += '\t'; break;
            case 'u': {
                if (i + 4 > s.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                v += cp < 0x80 ? static_cast<char>(cp) : '?';  // versions and URLs are ASCII
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Word(std::string_view w) {
        Ws();
        if (s.substr(i, w.size()) != w) return false;
        i += w.size();
        return true;
    }
    // Skips any value; *isNull / *str report what it was.
    bool Value(std::string* str, bool* isNull, int depth = 0) {
        if (depth > 16) return false;
        Ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '"') return String(str);
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i;
            if (Eat(close)) return true;
            for (;;) {
                if (c == '{' && (!String(nullptr) || !Eat(':'))) return false;
                if (!Value(nullptr, nullptr, depth + 1)) return false;
                if (Eat(close)) return true;
                if (!Eat(',')) return false;
            }
        }
        if (Word("null")) {
            if (isNull) *isNull = true;
            return true;
        }
        if (Word("true") || Word("false")) return true;
        const size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            ++i;
        return i > start;
    }
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct CheckState {
    std::mutex lock;
    bool started = false;
    State state = State::Idle;
    std::string latest;
};
CheckState& S() {
    static CheckState* s = new CheckState;  // never destroyed: the thread may outlive static destruction
    return *s;
}

void Finish(State state, std::string latest = {}) {
    CheckState& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    s.state = state;
    s.latest = std::move(latest);
}

void Run(std::string url, std::string current) {
    try {
        std::string body, error;
        if (!Fetch(url, 5000, &body, &error)) {
            MC_LOG_INFO("update check: no answer from %s (%s); not checked this time", url.c_str(), error.c_str());
            Finish(State::Failed);
            return;
        }
        Answer a;
        if (!ParseAnswer(body, &a)) {
            MC_LOG_INFO("update check: %s did not answer with a version; not checked this time", url.c_str());
            Finish(State::Failed);
            return;
        }
        if (a.latest.empty()) {
            MC_LOG_INFO("update check: no release published yet (this is %s)", Display(current).c_str());
            Finish(State::UpToDate);
            return;
        }
        Version lv, cv;
        if (!ParseVersion(a.latest, &lv) || !ParseVersion(current, &cv)) {
            MC_LOG_INFO("update check: cannot compare \"%s\" with \"%s\"", a.latest.c_str(), current.c_str());
            Finish(State::Failed);
            return;
        }
        if (Compare(lv, cv) > 0) {
            MC_LOG_INFO("update check: %s is available (this is %s)", Display(a.latest).c_str(),
                        Display(current).c_str());
            Finish(State::NewerAvailable, a.latest);
        } else {
            MC_LOG_INFO("update check: up to date (this is %s, newest release %s)", Display(current).c_str(),
                        Display(a.latest).c_str());
            Finish(State::UpToDate);
        }
    } catch (const std::exception& e) {
        MC_LOG_INFO("update check: failed (%s)", e.what());
        Finish(State::Failed);
    } catch (...) {
        Finish(State::Failed);
    }
}

}  // namespace

bool ParseVersion(std::string_view text, Version* out) {
    std::string_view s = Trim(text);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    if (const size_t plus = s.find('+'); plus != std::string_view::npos) s = s.substr(0, plus);
    std::string_view pre;
    if (const size_t dash = s.find('-'); dash != std::string_view::npos) {
        pre = s.substr(dash + 1);
        s = s.substr(0, dash);
        if (pre.empty()) return false;
        for (std::string_view id : Split(pre, '.')) {
            if (id.empty()) return false;
            for (char c : id)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
        }
    }
    const auto parts = Split(s, '.');
    if (parts.empty() || parts.size() > 3) return false;
    Version v;
    unsigned* fields[] = {&v.major, &v.minor, &v.patch};
    for (size_t k = 0; k < parts.size(); ++k)
        if (!ParseNumber(parts[k], fields[k])) return false;
    v.pre = std::string(pre);
    if (out) *out = std::move(v);
    return true;
}

int Compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : (a.pre.empty() ? 1 : -1);
    const auto pa = Split(a.pre, '.'), pb = Split(b.pre, '.');
    for (size_t k = 0; k < pa.size() && k < pb.size(); ++k) {
        const bool na = IsNumeric(pa[k]), nb = IsNumeric(pb[k]);
        if (na && nb) {
            unsigned x = 0, y = 0;
            const bool okx = ParseNumber(pa[k], &x), oky = ParseNumber(pb[k], &y);
            if (okx && oky && x != y) return x < y ? -1 : 1;
            if (!okx || !oky) {  // too long for unsigned: longer is larger, then text order
                if (pa[k].size() != pb[k].size()) return pa[k].size() < pb[k].size() ? -1 : 1;
                if (const int c = pa[k].compare(pb[k]); c) return c < 0 ? -1 : 1;
            }
        } else if (na != nb) {
            return na ? -1 : 1;
        } else if (const int c = pa[k].compare(pb[k]); c) {
            return c < 0 ? -1 : 1;
        }
    }
    if (pa.size() != pb.size()) return pa.size() < pb.size() ? -1 : 1;
    return 0;
}

bool IsNewer(std::string_view latest, std::string_view current) {
    Version l, c;
    return ParseVersion(latest, &l) && ParseVersion(current, &c) && Compare(l, c) > 0;
}

bool ParseAnswer(std::string_view json, Answer* out) {
    Json j{json};
    if (!j.Eat('{')) return false;
    Answer a;
    bool haveLatest = false;
    if (!j.Eat('}')) {
        for (;;) {
            std::string key;
            if (!j.String(&key) || !j.Eat(':')) return false;
            std::string str;
            bool isNull = false;
            if (!j.Value(&str, &isNull)) return false;
            if (key == "latest") {
                if (isNull) {
                    a.latest.clear();
                    haveLatest = true;
                } else if (ParseVersion(str, nullptr)) {
                    a.latest = str;
                    haveLatest = true;
                } else {
                    return false;  // a "latest" that is not a version
                }
            } else if (key == "url") {
                a.url = str;
            }
            if (j.Eat('}')) break;
            if (!j.Eat(',')) return false;
        }
    }
    j.Ws();
    if (j.i != json.size() || !haveLatest) return false;
    if (out) *out = std::move(a);
    return true;
}

std::string Display(std::string_view version) {
    const std::string_view s = Trim(version);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) return "v" + std::string(s.substr(1));
    return "v" + std::string(s);
}

std::string NoticeText(std::string_view modName, std::string_view latest, std::string_view current) {
    return std::string(modName) + " " + Display(latest) + " is out (you have " + Display(current) +
           "). Get it at justaddmp.com";
}

void Start(const std::string& url, const std::string& current) {
    {
        CheckState& s = S();
        std::lock_guard<std::mutex> g(s.lock);
        if (s.started) return;
        s.started = true;
        s.state = State::Running;
    }
    try {
        std::thread(Run, url, current).detach();
    } catch (...) {
        MC_LOG_INFO("update check: could not start its thread");
        Finish(State::Failed);
    }
}

State Poll(std::string* latest) {
    CheckState& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    if (latest && s.state == State::NewerAvailable) *latest = s.latest;
    return s.state;
}

bool Fetch(const std::string& url, unsigned timeoutMs, std::string* body, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s, error %lu", what, GetLastError());
            *error = buf;
        }
        return false;
    };
    const std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wurl.empty() || !WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
        return fail("bad URL");
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return fail("not http(s)");
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() {
            if (h) WinHttpCloseHandle(h);
        }
    };
    Handle session, connect, request;
    session.h = WinHttpOpen(L"JustAddMP-update-check/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return fail("WinHttpOpen");
    const int t = static_cast<int>(timeoutMs);
    WinHttpSetTimeouts(session.h, t, t, t, t);
    connect.h = WinHttpConnect(session.h, host.c_str(), uc.nPort, 0);
    if (!connect.h) return fail("connect");
    request.h = WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) return fail("open request");
    if (!WinHttpSendRequest(request.h, L"Accept: application/json\r\n", static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return fail("send");
    if (!WinHttpReceiveResponse(request.h, nullptr)) return fail("no response");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return fail("no status");
    if (status != 200) {
        if (error) *error = "HTTP " + std::to_string(status);
        return false;
    }
    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) return fail("read");
        if (!avail) break;
        if (out.size() + avail > 16 * 1024) {
            if (error) *error = "answer too long";
            return false;
        }
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, out.data() + at, avail, &got)) return fail("read");
        out.resize(at + got);
    }
    if (body) *body = std::move(out);
    return true;
}

}  // namespace mc::core::update
