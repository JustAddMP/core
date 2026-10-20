#include "core/bundle.h"

#include "core/log.h"
#include "core/trace.h"
#include "core/zip_writer.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace mc::core::bundle {
namespace {

struct Worker {
    std::mutex mu;
    std::condition_variable cv, idle;
    std::deque<Job> jobs;
    bool busy = false, started = false;
    std::wstring last;
};

Worker& W() {
    static Worker* w = new Worker;  // never destroyed: the thread may outlive static destruction
    return *w;
}

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string ReadText(const std::wstring& path, size_t max) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s;
    s.resize(max);
    f.read(s.data(), static_cast<std::streamsize>(max));
    s.resize(static_cast<size_t>(f.gcount()));
    return s;
}

void Write(Job& job) {
    const auto t0 = GetTickCount64();
    const std::wstring tmp = job.zip + L".tmp";
    zip::ZipWriter z;
    if (!z.Open(tmp)) {
        MC_LOG_WARN("bundle: cannot create %s (%lu)", Narrow(tmp).c_str(), GetLastError());
        return;
    }
    for (const File& f : job.files) {
        if (f.redactIni) {
            const std::string text = ReadText(f.source, 1 << 20);
            if (text.empty() && GetFileAttributesW(f.source.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const std::string red = RedactIni(text);
            z.AddData(f.name, red.data(), red.size());
        } else {
            z.AddFile(f.source, f.name);
        }
    }
    std::string m = job.manifest;
    m += "files:\r\n";
    uint64_t in = 0, out = 0;
    int written = 0;
    for (const auto& e : z.Info()) {
        char line[512];
        std::snprintf(line, sizeof line, "  %-56s %12llu bytes (%llu compressed)%s%s\r\n", e.name.c_str(),
                      static_cast<unsigned long long>(e.in), static_cast<unsigned long long>(e.out),
                      e.note.empty() ? "" : "  ", e.note.c_str());
        m += line;
        in += e.in;
        out += e.out;
        written += e.written ? 1 : 0;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char tail[160];
    std::snprintf(tail, sizeof tail, "written: %04u-%02u-%02u %02u:%02u:%02u by process %lu\r\n", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    m += tail;
    z.AddData("manifest.txt", m.data(), m.size());
    const bool ok = z.Close() && !z.Failed();
    const double secs = static_cast<double>(GetTickCount64() - t0) / 1000.0;
    if (!ok) {
        DeleteFileW(tmp.c_str());
        MC_LOG_WARN("bundle: writing %s failed (disk full?)", Narrow(job.zip).c_str());
        return;
    }
    if (!MoveFileExW(tmp.c_str(), job.zip.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        MC_LOG_WARN("bundle: could not name it %s (%lu)", Narrow(job.zip).c_str(), GetLastError());
        return;
    }
    MC_LOG_INFO("bundle: wrote %s (%d files, %.1f MB -> %.1f MB, %.1f s)", Narrow(job.zip).c_str(), written,
                static_cast<double>(in) / 1e6, static_cast<double>(z.Size()) / 1e6, secs);
    {
        std::lock_guard lock(W().mu);
        W().last = job.zip;
    }
    if (!job.keepDir.empty()) {
        const int gone = trace::PruneFolder(job.keepDir, L"*.zip", kKeepBundles, 0);
        if (gone > 0) MC_LOG_INFO("bundle: %d old bundle(s) removed (the newest %d stay)", gone, kKeepBundles);
    }
}

void Run() {
    Worker& w = W();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(w.mu);
            w.cv.wait(lock, [&] { return !w.jobs.empty(); });
            job = std::move(w.jobs.front());
            w.jobs.pop_front();
            w.busy = true;
        }
        Write(job);
        {
            std::lock_guard lock(w.mu);
            w.busy = false;
        }
        w.idle.notify_all();
    }
}

}  // namespace

void Submit(Job job) {
    Worker& w = W();
    {
        std::lock_guard lock(w.mu);
        w.jobs.push_back(std::move(job));
        if (!w.started) {
            w.started = true;
            std::thread(Run).detach();
        }
    }
    w.cv.notify_one();
}

bool WaitIdle(unsigned ms) {
    Worker& w = W();
    std::unique_lock lock(w.mu);
    return w.idle.wait_for(lock, std::chrono::milliseconds(ms), [&] { return w.jobs.empty() && !w.busy; });
}

bool Busy() {
    Worker& w = W();
    std::lock_guard lock(w.mu);
    return w.busy || !w.jobs.empty();
}

std::wstring LastWritten() {
    std::lock_guard lock(W().mu);
    return W().last;
}

bool IsSecretKey(std::string_view key) {
    const std::string k = Lower(key);
    if (k.rfind("require_", 0) == 0) return false;  // a switch (require_password_for_invites), not a secret
    for (const char* s : {"password", "passwd", "pwd", "secret", "token"})
        if (k.find(s) != std::string::npos) return true;
    return false;
}

std::string RedactIni(const std::string& text) {
    std::string out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        const size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        const size_t eq = line.find('=');
        if (b != std::string::npos && line[b] != '#' && line[b] != ';' && line[b] != '[' && eq != std::string::npos &&
            eq > b) {
            std::string key = line.substr(b, eq - b);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            const size_t v = line.find_first_not_of(" \t", eq + 1);
            if (IsSecretKey(key) && v != std::string::npos) line = line.substr(0, v) + "<redacted>";
        }
        out += line;
        if (cr) out += '\r';
        if (next <= text.size() && end < text.size()) out += '\n';
        at = next;
    }
    return out;
}

std::string TraceStamp(const std::wstring& fileName, const std::wstring& base) {
    // base + "-" + 8 digits + "-" + 6 digits + ".trace"
    const std::wstring pre = base + L"-";
    if (fileName.size() != pre.size() + 15 + 6 || fileName.compare(0, pre.size(), pre) != 0) return {};
    if (fileName.compare(fileName.size() - 6, 6, L".trace") != 0) return {};
    std::string stamp;
    for (size_t i = pre.size(); i < pre.size() + 15; ++i) {
        const wchar_t c = fileName[i];
        const bool dash = i == pre.size() + 8;
        if (dash ? c != L'-' : (c < L'0' || c > L'9')) return {};
        stamp += static_cast<char>(c);
    }
    return stamp;
}

std::wstring BundleName(const std::string& mod, const std::string& stamp, const std::string& role, bool recovered) {
    const std::string n = mod + "-" + stamp + "-" + (role.empty() ? "session" : role) + (recovered ? "-recovered" : "") +
                          ".zip";
    return std::wstring(n.begin(), n.end());
}

bool HasBundle(const std::vector<std::wstring>& bundles, const std::string& mod, const std::string& stamp,
               const std::string& role) {
    const std::string p = mod + "-" + stamp + "-" + (role.empty() ? "session" : role);
    const std::wstring pre(p.begin(), p.end());
    for (const auto& b : bundles)
        if (b.size() >= pre.size() + 4 && b.compare(0, pre.size(), pre) == 0 &&
            (b[pre.size()] == L'.' || b[pre.size()] == L'-') && b.compare(b.size() - 4, 4, L".zip") == 0)
            return true;
    return false;
}

std::wstring NewestTrace(const std::vector<std::wstring>& traces, const std::wstring& base) {
    std::wstring best;
    std::string bestStamp;
    for (const auto& t : traces) {
        const std::string s = TraceStamp(t, base);
        if (!s.empty() && s > bestStamp) {
            bestStamp = s;
            best = t;
        }
    }
    return best;
}

std::string RoleOfHeader(const std::string& firstLine) {
    const size_t at = firstLine.find(" role=");
    if (at == std::string::npos || firstLine.find(" H ") == std::string::npos) return {};
    std::string role;
    for (size_t i = at + 6; i < firstLine.size() && std::isalnum(static_cast<unsigned char>(firstLine[i])); ++i)
        role += firstLine[i];
    return role;
}

}  // namespace mc::core::bundle
