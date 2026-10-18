// net_lobby: the pre-game lobby (net/lobby.h, docs/protocol.md "Lobby") and clean leaving, host + guests over
// UdpTransport on 127.0.0.1 in one process (with loss on the host's packets).
// Checks: ready flags reach the host and every guest; a start waits for everyone to be ready, then reaches every
// guest once with the host's mode; guests cannot start or fake the state; a late joiner sees the game in progress;
// a newcomer is not ready; a host that leaves is reported to the guests at once ("host closed the session", not a
// timeout) and a guest that leaves is reported to the host at once. A full lobby of four (host + 3 guests): a fifth is
// refused, the start waits for all three, a freed slot goes to a newcomer, the host's leave reaches all three.
// Prints PASS/FAIL per check, exits non-zero on any failure. "-v" prints net info logs.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/lobby.h"
#include "net/session.h"
#include "net/udp_transport.h"
#include "net/wire.h"

using namespace mc::net;

namespace {

int g_failures = 0;
bool g_verbose = false;
double g_t0 = 0;

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
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void PrintLog(int level, const char* msg) {
    if (level >= 2 || g_verbose) std::printf("    [%7.3f] %s\n", Seconds() - g_t0, msg);
}

struct Node {
    std::string name;
    std::unique_ptr<UdpTransport> transport;
    Session session;
    Lobby lobby;
    bool joined = false;
    std::string disconnectReason;
    double disconnectAt = 0;
    std::vector<std::string> left;  // reasons
    double leftAt = 0;
    std::vector<int> starts;

    Node(std::string n, UdpOptions opt = {})
        : name(n), transport(std::make_unique<UdpTransport>()), session(*transport, Cfg(n)) {
        transport->SetOptions(opt);
        lobby.Attach(session);
        lobby.onStart = [this](int mode) { starts.push_back(mode); };
        session.onJoined = [this] { joined = true; };
        session.onDisconnected = [this](const std::string& r) {
            disconnectReason = r.empty() ? "?" : r;
            disconnectAt = Seconds();
        };
        session.onPlayerLeft = [this](const PlayerInfo&, const std::string& r) {
            left.push_back(r);
            leftAt = Seconds();
        };
    }
    static SessionConfig Cfg(const std::string& n) {
        SessionConfig c;
        c.modBuild = 42;
        c.playerName = n;
        return c;
    }
};

using Nodes = std::vector<Node*>;
bool Pump(const Nodes& nodes, double seconds, const std::function<bool()>& done) {
    double end = Seconds() + seconds;
    while (Seconds() < end) {
        for (Node* n : nodes) {
            n->session.Poll();
            n->lobby.Tick();
        }
        if (done && done()) return true;
        Sleep(1);
    }
    return done ? done() : true;
}

std::string Addr(Node& host) { return "127.0.0.1:" + std::to_string(host.transport->BoundPort()); }

bool Join(Node& host, Node& g, const Nodes& others = {}) {
    g.session.Join(Addr(host));
    Nodes all = others;
    all.push_back(&host);
    all.push_back(&g);
    return Pump(all, 8, [&] { return g.joined || !g.disconnectReason.empty(); }) && g.joined;
}

// Everyone has heard the lobby state with these ready flags for the two guests.
bool SeeReady(Node& n, PlayerId a, bool ra, PlayerId b, bool rb) {
    return n.lobby.Ready(a) == ra && n.lobby.Ready(b) == rb;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
    g_t0 = Seconds();
    SetLogSink(PrintLog);

    UdpOptions lossy;
    lossy.simLoss = 0.2f;
    lossy.simLatencyMs = 20;
    lossy.simSeed = 11;
    Node host("Host", lossy);
    Check(host.session.Host(0), "host: bind");
    Node a("Alice"), b("Bob");
    const bool in = Join(host, a) && Join(host, b, {&a});
    Nodes all{&host, &a, &b};
    Check(in && host.session.Players().size() == 3, "setup: host + 2 guests");
    const PlayerId aid = a.session.LocalId(), bid = b.session.LocalId();

    // ---- ready flags ----
    Pump(all, 0.5, nullptr);
    Check(!host.lobby.AllReady() && !a.lobby.Ready(aid) && !b.lobby.Ready(bid), "new players are not ready");
    Check(!host.lobby.Start(0), "start refused while a guest is not ready");
    a.lobby.SetReady(true);
    Pump(all, 5, [&] { return SeeReady(host, aid, true, bid, false) && SeeReady(b, aid, true, bid, false); });
    Check(SeeReady(host, aid, true, bid, false) && SeeReady(b, aid, true, bid, false) &&
              SeeReady(a, aid, true, bid, false),
          "a guest's ready reaches the host and the others");
    Check(!host.lobby.AllReady() && !host.lobby.Start(0), "still refused: one guest not ready");

    // ---- mode ----
    host.lobby.SetMode(2);
    Pump(all, 5, [&] { return a.lobby.Mode() == 2 && b.lobby.Mode() == 2; });
    Check(a.lobby.Mode() == 2 && b.lobby.Mode() == 2, "the host's mode reaches the guests");

    // ---- guests cannot start or fake the state ----
    {
        ByteWriter w(8);
        w.U8(1);
        w.U32(99);
        b.session.Broadcast(Lobby::kMsgStart, w.Span(), Reliability::Reliable);  // relayed by the host to Alice
        ByteWriter s(16);
        s.U8(1), s.U8(3), s.U8(1), s.U8(3), s.U32(98), s.U8(0);
        b.session.Broadcast(Lobby::kMsgState, s.Span(), Reliability::Reliable);
        Pump(all, 1.0, nullptr);
        Check(a.starts.empty() && host.starts.empty() && a.lobby.Mode() == 2 && !a.lobby.InGame(),
              "a guest's start/state is ignored");
    }

    // ---- start ----
    b.lobby.SetReady(true);
    Pump(all, 5, [&] { return host.lobby.AllReady(); });
    Check(host.lobby.AllReady(), "everyone ready");
    const bool started = host.lobby.Start(1);
    Pump(all, 5, [&] { return a.starts.size() == 1 && b.starts.size() == 1; });
    Check(started && host.starts == std::vector<int>{1} && a.starts == std::vector<int>{1} &&
              b.starts == std::vector<int>{1},
          "start reaches the host and every guest once, same mode");
    Pump(all, 5, [&] { return !a.lobby.Ready(aid) && !b.lobby.Ready(bid); });
    Check(!host.lobby.AllReady() && !a.lobby.Ready(aid) && !b.lobby.Ready(bid), "ready is cleared after a start");
    Check(host.lobby.Start(1, true), "a forced start ignores ready");
    Pump(all, 5, [&] { return a.starts.size() == 2 && b.starts.size() == 2; });
    Check(a.starts.size() == 2 && b.starts.size() == 2, "forced start reaches the guests");

    // ---- in game: a late joiner sees it ----
    host.lobby.SetInGame(true, 1);
    Pump(all, 5, [&] { return a.lobby.InGame() && b.lobby.InGame(); });
    Check(a.lobby.InGame() && a.lobby.GameMode() == 1, "guests see the host playing mode 1");
    b.session.Leave();
    Pump({&host, &a}, 3, [&] { return host.session.Players().size() == 2; });
    Node c("Carol");
    const bool cin = Join(host, c, {&a});
    Nodes all2{&host, &a, &c};
    Pump(all2, 5, [&] { return c.lobby.InGame(); });
    Check(cin && c.lobby.InGame() && c.lobby.GameMode() == 1 && c.starts.empty(),
          "a late joiner sees the game in progress, no start replayed");
    Check(!host.lobby.Ready(c.session.LocalId()), "the late joiner is not ready");

    // ---- clean leave: guest -> host at once ----
    {
        const size_t before = host.left.size();
        const double t = Seconds();
        c.session.Leave();
        Pump({&host, &a}, 5, [&] { return host.left.size() > before; });
        const double dt = host.leftAt - t;
        Check(host.left.size() > before && host.left.back() == "closed by remote" && dt < 1.0,
              "a guest that leaves is reported to the host at once",
              host.left.empty() ? "" : "\"" + host.left.back() + "\" after " + std::to_string(dt) + " s");
    }
    // ---- clean leave: host -> guests at once ----
    {
        const double t = Seconds();
        host.session.Leave();
        Pump({&a}, 5, [&] { return !a.disconnectReason.empty(); });
        const double dt = a.disconnectAt - t;
        Check(a.disconnectReason == "host closed the session" && dt < 1.0,
              "a host that leaves is reported to the guests at once",
              "\"" + a.disconnectReason + "\" after " + std::to_string(dt) + " s");
    }

    // ---- a full lobby: host + 3 guests (kMaxPlayers 4): ready, start, a fifth refused ----
    {
        Node h4("Host4");
        Check(h4.session.Host(0), "4 players: host bind");
        Node g1("Gina"), g2("Hugo"), g3("Ivy");
        const bool in4 = Join(h4, g1) && Join(h4, g2, {&g1}) && Join(h4, g3, {&g1, &g2});
        Nodes four{&h4, &g1, &g2, &g3};
        Pump(four, 3, [&] { return g1.session.Players().size() == 4 && g3.session.Players().size() == 4; });
        Check(in4 && h4.session.Players().size() == 4 && g1.session.Players().size() == 4 &&
                  g2.session.Players().size() == 4 && g3.session.Players().size() == 4,
              "4 players: everyone sees all four");
        Node g5("Jack");
        g5.session.Join(Addr(h4));
        Nodes five{&h4, &g1, &g2, &g3, &g5};
        Pump(five, 6, [&] { return g5.joined || !g5.disconnectReason.empty(); });
        Check(!g5.joined && g5.disconnectReason.find("full") != std::string::npos && h4.session.Players().size() == 4,
              "4 players: a fifth is refused (full)", "\"" + g5.disconnectReason + "\"");
        g1.lobby.SetReady(true);
        g2.lobby.SetReady(true);
        Pump(four, 5, [&] { return h4.lobby.Ready(g1.session.LocalId()) && h4.lobby.Ready(g2.session.LocalId()); });
        Check(!h4.lobby.AllReady() && !h4.lobby.Start(3), "4 players: two of three ready: start refused");
        g3.lobby.SetReady(true);
        auto allSeen = [&] {
            bool seen = true;
            for (Node* n : {&g1, &g2, &g3})
                for (Node* m : {&g1, &g2, &g3}) seen &= n->lobby.Ready(m->session.LocalId());
            return seen;
        };
        Pump(four, 5, [&] { return h4.lobby.AllReady() && allSeen(); });
        Check(h4.lobby.AllReady() && allSeen(), "4 players: all three ready, every guest sees every flag");
        const bool st = h4.lobby.Start(3);
        Pump(four, 5, [&] { return g1.starts.size() == 1 && g2.starts.size() == 1 && g3.starts.size() == 1; });
        Check(st && h4.starts == std::vector<int>{3} && g1.starts == std::vector<int>{3} &&
                  g2.starts == std::vector<int>{3} && g3.starts == std::vector<int>{3},
              "4 players: the start reaches all three guests once");
        // one guest leaves: the others stay, a newcomer gets his slot and is not ready
        g2.session.Leave();
        Pump({&h4, &g1, &g3}, 3, [&] { return h4.session.Players().size() == 3 && g1.session.Players().size() == 3; });
        Node g6("Kim");
        const bool in6 = Join(h4, g6, {&g1, &g3});
        Pump({&h4, &g1, &g3, &g6}, 3, [&] { return g1.session.Players().size() == 4; });
        Check(in6 && h4.session.Players().size() == 4 && !h4.lobby.Ready(g6.session.LocalId()) &&
                  g1.session.Players().size() == 4,
              "4 players: a guest leaves, a newcomer takes the free slot (not ready)");
        h4.session.Leave();
        Pump({&g1, &g3, &g6}, 5, [&] {
            return !g1.disconnectReason.empty() && !g3.disconnectReason.empty() && !g6.disconnectReason.empty();
        });
        Check(g1.disconnectReason == "host closed the session" && g3.disconnectReason == "host closed the session" &&
                  g6.disconnectReason == "host closed the session",
              "4 players: the host leaves, all three guests are told");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
