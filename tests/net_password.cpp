// net_password: the optional session password (docs/protocol.md "Session password"), host + guests over UdpTransport
// on 127.0.0.1 in one process. Prints PASS/FAIL per check, exits non-zero on any failure. "-v" prints net info logs.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/auth.h"
#include "net/session.h"
#include "net/udp_transport.h"

using namespace mc::net;

namespace {

int g_failures = 0;
bool g_verbose = false;
double g_t0 = 0;

// Everything any transport in this test puts on the wire, to prove the password never does.
std::vector<std::vector<uint8_t>> g_sent;

const char kPassword[] = "Sekrit-Pa55word!";

double Seconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void PrintLog(int level, const char* msg) {
    if (level >= 2 || g_verbose) std::printf("    [%7.3f] %s\n", Seconds() - g_t0, msg);
}

struct Node {
    std::string name;
    UdpTransport transport;
    Session session;
    bool joined = false;
    std::string disconnectReason;

    Node(std::string n, SessionConfig cfg) : name(n), session(transport, (cfg.playerName = n, cfg)) {
        UdpOptions o;
        o.testOnSend = [](std::span<const uint8_t> d) { g_sent.emplace_back(d.begin(), d.end()); };
        transport.SetOptions(o);
        session.onJoined = [this] { joined = true; };
        session.onDisconnected = [this](const std::string& r) { disconnectReason = r.empty() ? "?" : r; };
    }
};

bool Pump(const std::vector<Node*>& nodes, double seconds, const std::function<bool()>& done) {
    double end = Seconds() + seconds;
    while (Seconds() < end) {
        for (Node* n : nodes) n->session.Poll();
        if (done && done()) return true;
        Sleep(1);
    }
    return done ? done() : true;
}

SessionConfig Cfg(const std::string& password = {}) {
    SessionConfig c;
    c.modBuild = 42;
    c.password = password;
    return c;
}

std::string Addr(Node& host) { return "127.0.0.1:" + std::to_string(host.transport.BoundPort()); }

// One guest joins and either gets in or is rejected; returns the guest's outcome ("joined" or the reason).
std::string TryJoin(Node& host, const std::string& guestPassword, RejectReason* reject = nullptr, double* took = nullptr) {
    Node g("Guest", Cfg(guestPassword));
    const double t = Seconds();
    g.session.Join(Addr(host));
    Pump({&host, &g}, 8, [&] { return g.joined || !g.disconnectReason.empty(); });
    if (took) *took = Seconds() - t;
    if (reject) *reject = g.session.LastReject();
    std::string out = g.joined ? "joined" : g.disconnectReason;
    g.session.Leave();
    Pump({&host}, 0.3, nullptr);  // host sees the leave / drops the rejected connection
    return out;
}

void TestHmac() {
    // RFC 4231 test case 2.
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    const uint8_t want[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
                              0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
    uint8_t got[32];
    bool ok = auth::HmacSha256({reinterpret_cast<const uint8_t*>(key), 4},
                               {reinterpret_cast<const uint8_t*>(data), std::strlen(data)}, got);
    Check(ok && std::memcmp(got, want, 32) == 0, "auth: HMAC-SHA256 (RFC 4231 case 2)");
    uint8_t a[32] = {}, b[32] = {};
    b[31] = 1;
    uint8_t r1[32] = {}, r2[32] = {};
    bool rnd = auth::SecureRandom(r1, 32) && auth::SecureRandom(r2, 32) && std::memcmp(r1, r2, 32) != 0;
    Check(auth::ConstantTimeEqual(a, a, 32) && !auth::ConstantTimeEqual(a, b, 32) && rnd,
          "auth: constant-time compare + random");
}

bool Contains(const std::vector<uint8_t>& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
    g_t0 = Seconds();
    SetLogSink(PrintLog);

    TestHmac();

    // ---- 1. no password: joins as before (with or without a guest password) ----
    {
        Node host("Host", Cfg());
        if (!host.session.Host(0)) {
            Check(false, "host: bind");
            return 1;
        }
        std::string a = TryJoin(host, "");
        std::string b = TryJoin(host, kPassword);
        Check(a == "joined" && b == "joined", "open session: guests join", "\"" + a + "\", \"" + b + "\"");
    }

    // ---- 2. right / wrong / missing password ----
    {
        Node host("Host", Cfg(kPassword));
        host.session.Host(0);
        RejectReason why = RejectReason::None;
        std::string ok = TryJoin(host, kPassword, &why);
        Check(ok == "joined" && host.session.Players().size() >= 1, "right password: joins", "\"" + ok + "\"");

        double took = 0;
        std::string wrong = TryJoin(host, "not-the-password", &why, &took);
        char d[160];
        std::snprintf(d, sizeof(d), "\"%s\" after %.2f s", wrong.c_str(), took);
        Check(wrong.find("wrong password") != std::string::npos && why == RejectReason::WrongPassword && took >= 0.9,
              "wrong password: Reject after ~1 s", d);

        std::string none = TryJoin(host, "", &why);
        Check(none.find("password required") != std::string::npos && why == RejectReason::PasswordRequired,
              "no password: Reject \"password required\"", "\"" + none + "\"");

        // Retry on the same Session object with the right password (what the UI prompt does).
        Node g("Retry", Cfg(""));
        g.session.Join(Addr(host));
        Pump({&host, &g}, 8, [&] { return !g.disconnectReason.empty(); });
        bool needs = g.session.NeedsPassword();
        g.disconnectReason.clear();
        bool retried = g.session.RetryWithPassword(kPassword);
        Pump({&host, &g}, 8, [&] { return g.joined || !g.disconnectReason.empty(); });
        Check(needs && retried && g.joined && g.session.LastReject() == RejectReason::None,
              "guest retries with a password: joins", g.joined ? "" : "\"" + g.disconnectReason + "\"");
        g.session.Leave();
        Pump({&host}, 0.3, nullptr);

        // Host changes the password at runtime: the old one stops working.
        host.session.SetPassword("new-one");
        std::string old = TryJoin(host, kPassword, &why);
        std::string fresh = TryJoin(host, "new-one");
        host.session.SetPassword("");
        std::string open = TryJoin(host, "");
        Check(why == RejectReason::WrongPassword && fresh == "joined" && open == "joined",
              "host changes / clears the password", "\"" + old + "\", \"" + fresh + "\", \"" + open + "\"");
    }

    // ---- 3. attempt limit: 5 wrong answers per address, the 6th attempt is refused ----
    {
        Node host("Host", Cfg(kPassword));
        host.session.Host(0);
        std::vector<std::string> res;
        for (int i = 0; i < 6; ++i) res.push_back(TryJoin(host, "guess" + std::to_string(i)));
        RejectReason why = RejectReason::None;
        std::string right = TryJoin(host, kPassword, &why);  // locked out even with the right one
        bool firstFive = true;
        for (int i = 0; i < 5; ++i) firstFive &= res[static_cast<size_t>(i)].find("wrong password") != std::string::npos;
        Check(firstFive && res[5].find("too many attempts") != std::string::npos &&
                  why == RejectReason::TooManyAttempts,
              "6 wrong attempts -> \"too many attempts\"", "\"" + res[5] + "\", then right one: \"" + right + "\"");
    }

    // ---- 4. Steam invites (lobby members) skip the password unless required ----
    {
        Node host("Host", Cfg(kPassword));
        host.session.Host(0);
        host.session.isInvitedPeer = [](PeerId) { return true; };  // stand-in for "is a member of our Steam lobby"
        std::string invited = TryJoin(host, "");
        host.session.isInvitedPeer = [](PeerId) { return false; };
        std::string stranger = TryJoin(host, "");

        SessionConfig strict = Cfg(kPassword);
        strict.invitesSkipPassword = false;  // require_password_for_invites = 1
        Node host2("Host2", strict);
        host2.session.Host(0);
        host2.session.isInvitedPeer = [](PeerId) { return true; };
        std::string required = TryJoin(host2, "");
        std::string withPw = TryJoin(host2, kPassword);
        Check(invited == "joined" && stranger.find("password required") != std::string::npos &&
                  required.find("password required") != std::string::npos && withPw == "joined",
              "invites skip the password (unless required)",
              "\"" + invited + "\", \"" + stranger + "\", \"" + required + "\", \"" + withPw + "\"");
    }

    // ---- 5. the password never went over the wire ----
    {
        size_t bytes = 0, hits = 0, names = 0;
        for (auto& d : g_sent) {
            bytes += d.size();
            if (Contains(d, kPassword) || Contains(d, "new-one") || Contains(d, "not-the-password")) ++hits;
            if (Contains(d, "Guest")) ++names;  // control: the scan does see plain session payloads (Hello names)
        }
        Check(hits == 0 && names > 0 && g_sent.size() > 50,
              "no password in any sent datagram",
              std::to_string(g_sent.size()) + " datagrams, " + std::to_string(bytes) + " bytes scanned, " +
                  std::to_string(hits) + " hits (" + std::to_string(names) + " with a player name)");
    }

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
