// update_check: the update check (src/core/update_check.h) without a game and without the internet: version parsing
// and comparison (v prefixes, missing parts, pre-releases, malformed input), the endpoint's answer, the notice text,
// fetch failures (bad URL, nothing listening) and one whole check against a stand-in endpoint on 127.0.0.1.
// Exits non-zero on any failure.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/update_check.h"

namespace {

namespace up = mc::core::update;

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

int Cmp(const char* a, const char* b) {
    up::Version va, vb;
    if (!up::ParseVersion(a, &va) || !up::ParseVersion(b, &vb)) return 99;
    return up::Compare(va, vb);
}

void Parsing() {
    up::Version v;
    Check(up::ParseVersion("v0.15.0", &v) && v.major == 0 && v.minor == 15 && v.patch == 0 && v.pre.empty(),
          "parse v0.15.0");
    Check(up::ParseVersion("0.16.0", &v) && v.minor == 16, "parse 0.16.0 (no v)");
    Check(up::ParseVersion("V1.2.3", &v) && v.major == 1 && v.minor == 2 && v.patch == 3, "parse V1.2.3");
    Check(up::ParseVersion("0.15", &v) && v.minor == 15 && v.patch == 0, "parse 0.15 (patch 0)");
    Check(up::ParseVersion("2", &v) && v.major == 2 && v.minor == 0, "parse 2");
    Check(up::ParseVersion(" v0.15.0 \r\n", &v) && v.minor == 15, "parse with spaces around");
    Check(up::ParseVersion("1.2.3-beta.2", &v) && v.patch == 3 && v.pre == "beta.2", "parse pre-release");
    Check(up::ParseVersion("1.2.3+build.7", &v) && v.patch == 3 && v.pre.empty(), "parse build suffix ignored");
    Check(up::ParseVersion("1.2.3-rc.1+b5", &v) && v.pre == "rc.1", "parse pre-release and build");

    const char* bad[] = {"",       "v",        "vv1.0",   "1..2",     "1.2.3.4", "a.b.c",   "1.2.x",     "-1.0.0",
                         "1.2.3-", "1.2.3-a..b", "1.2.3-a b", "1234567890.0.0", "1.2 3", ".1.2", "1.2.", "latest",
                         "null",   "v0.15.0\"", "0x10"};
    for (const char* b : bad) Check(!up::ParseVersion(b, &v), "malformed rejected", std::string("\"") + b + "\"");
}

void Comparing() {
    Check(Cmp("v0.16.0", "v0.15.0") > 0, "v0.16.0 > v0.15.0");
    Check(Cmp("v0.15.0", "v0.16.0") < 0, "v0.15.0 < v0.16.0");
    Check(Cmp("v0.15.0", "0.15.0") == 0, "v0.15.0 == 0.15.0");
    Check(Cmp("0.15", "0.15.0") == 0, "0.15 == 0.15.0");
    Check(Cmp("0.10.0", "0.9.0") > 0, "0.10.0 > 0.9.0 (numbers, not text)");
    Check(Cmp("1.0.0", "0.99.99") > 0, "1.0.0 > 0.99.99");
    Check(Cmp("0.15.1", "0.15.0") > 0, "patch counts");
    Check(Cmp("0.16.0-beta", "0.16.0") < 0, "pre-release < release");
    Check(Cmp("0.16.0-beta", "0.15.0") > 0, "pre-release of a newer version > older release");
    Check(Cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0, "alpha < alpha.1");
    Check(Cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0, "numeric id < alphanumeric id");
    Check(Cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0, "beta.2 < beta.11");
    Check(Cmp("1.0.0-beta.11", "1.0.0-rc.1") < 0, "beta.11 < rc.1");
    Check(Cmp("1.0.0-rc.1", "1.0.0-rc.1+x") == 0, "build suffix does not count");

    Check(up::IsNewer("v0.16.0", "0.15.0"), "IsNewer v0.16.0 over 0.15.0");
    Check(!up::IsNewer("v0.15.0", "0.15.0"), "IsNewer same version: no");
    Check(!up::IsNewer("v0.14.0", "0.15.0"), "IsNewer older release: no");
    Check(!up::IsNewer("garbage", "0.15.0"), "IsNewer malformed latest: no");
    Check(!up::IsNewer("v0.16.0", "dev"), "IsNewer malformed current: no");
    Check(!up::IsNewer("", ""), "IsNewer empty: no");
}

void Answers() {
    up::Answer a;
    const char* real =
        "{\"mod\":\"mafia-de\",\"latest\":\"v0.15.0\",\"prerelease\":true,\"published\":\"2026-10-18T19:00:39Z\","
        "\"url\":\"https:\\/\\/github.com/JustAddMP/MafiaDE/releases/tag/v0.15.0\",\"download\":null,"
        "\"notes\":\"MafiaCoop 0.15.0 (pre-release, \\\"latest\\\": soon)\",\"source\":\"github\"}";
    Check(up::ParseAnswer(real, &a) && a.latest == "v0.15.0" &&
              a.url == "https://github.com/JustAddMP/MafiaDE/releases/tag/v0.15.0",
          "answer: the endpoint's form", a.latest + " " + a.url);
    Check(up::ParseAnswer("{\"mod\":\"mafia2-de\",\"latest\":null,\"url\":null}", &a) && a.latest.empty(),
          "answer: no release yet (latest null)");
    Check(up::ParseAnswer(" \r\n{ \"extra\" : {\"latest\":\"9.9.9\",\"a\":[1,2,{\"b\":null}]}, \"latest\" : \"0.16.0\" }\n",
                          &a) &&
              a.latest == "0.16.0",
          "answer: nested values skipped, spaces", a.latest);
    const char* bad[] = {"",
                         "<html>502 Bad Gateway</html>",
                         "{\"error\":\"unknown mod\",\"mods\":[\"mafia-de\"]}",
                         "{\"latest\":\"not a version\"}",
                         "{\"latest\":42}",
                         "{\"latest\":\"v0.16.0\"",
                         "{\"latest\":\"v0.16.0\"} trailing",
                         "{\"latest\":\"v0.16.0\",}",
                         "[\"latest\",\"v0.16.0\"]",
                         "{\"latest\":\"v0.16\\q\"}",
                         "{\"latest\":\"v0.16.0\" \"url\":\"x\"}"};
    for (const char* b : bad) Check(!up::ParseAnswer(b, &a), "answer: rejected", std::string("'") + b + "'");
    std::string deep(200, '[');
    Check(!up::ParseAnswer("{\"x\":" + deep + "}", &a), "answer: absurd nesting rejected");
}

void Texts() {
    Check(up::Display("v0.16.0") == "v0.16.0" && up::Display("0.16.0") == "v0.16.0" && up::Display("V1.0") == "v1.0",
          "display adds v once");
    const std::string t = up::NoticeText("MafiaCoop", "v0.16.0", "0.15.0");
    Check(t == "MafiaCoop v0.16.0 is out (you have v0.15.0). Get it at justaddmp.com", "notice text", t);
}

// A one-shot HTTP server on 127.0.0.1 answering every request with `reply`.
struct StandIn {
    SOCKET listener = INVALID_SOCKET;
    unsigned short port = 0;
    std::thread thread;
    explicit StandIn(std::string reply) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        getsockname(listener, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        listen(listener, 4);
        thread = std::thread([this, reply] {
            for (int k = 0; k < 4; ++k) {
                SOCKET c = accept(listener, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                char buf[2048];
                std::string req;
                while (req.find("\r\n\r\n") == std::string::npos) {
                    const int n = recv(c, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    req.append(buf, n);
                }
                send(c, reply.data(), static_cast<int>(reply.size()), 0);
                shutdown(c, SD_SEND);
                closesocket(c);
            }
        });
    }
    ~StandIn() {
        closesocket(listener);
        thread.join();
    }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port) + "/api/version/test"; }
};

std::string HttpReply(const char* status, const std::string& body) {
    return std::string("HTTP/1.1 ") + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

void Fetching() {
    std::string body, error;
    Check(!up::Fetch("not a url", 2000, &body, &error), "fetch: bad URL fails", error);
    Check(!up::Fetch("ftp://127.0.0.1/x", 2000, &body, &error), "fetch: not http fails", error);

    // a port nothing listens on: refused at once, not after the timeout
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    int len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    closesocket(s);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = up::Fetch("http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/", 3000, &body, &error);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Check(!ok && secs < 4.0, "fetch: nothing listening fails within the timeout", error + ", " + std::to_string(secs));

    {
        StandIn server(HttpReply("404 Not Found", "{\"error\":\"unknown mod\"}"));
        Check(!up::Fetch(server.Url(), 3000, &body, &error) && error == "HTTP 404", "fetch: HTTP 404 fails", error);
    }
    {
        StandIn server(HttpReply("200 OK", std::string(20000, ' ')));
        Check(!up::Fetch(server.Url(), 3000, &body, &error), "fetch: over-long answer fails", error);
    }
}

void WholeCheck() {
    Check(up::Poll() == up::State::Idle, "check: idle before Start");
    StandIn server(HttpReply("200 OK", "{\"mod\":\"test\",\"latest\":\"v0.16.0\",\"url\":null}"));
    const auto t0 = std::chrono::steady_clock::now();
    up::Start(server.Url(), "0.15.0");
    up::Start(server.Url(), "0.15.0");  // a second call does nothing
    up::State st = up::Poll();
    while ((st == up::State::Running || st == up::State::Idle) &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        st = up::Poll();
    }
    std::string latest;
    st = up::Poll(&latest);
    Check(st == up::State::NewerAvailable && latest == "v0.16.0", "check: newer release found", latest);
}

}  // namespace

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    Parsing();
    Comparing();
    Answers();
    Texts();
    Fetching();
    WholeCheck();
    WSACleanup();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
