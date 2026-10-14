// net_loopback: host + guests over UdpTransport on 127.0.0.1 in one process.
// Prints PASS/FAIL per check, exits non-zero on any failure. "-v" prints net info logs.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    std::printf("%s  %-40s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void PrintLog(int level, const char* msg) {
    if (level >= 2 || g_verbose) std::printf("    [%7.3f] %s\n", Seconds() - g_t0, msg);
}

uint64_t Fnv(std::span<const uint8_t> d) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : d) h = (h ^ b) * 1099511628211ull;
    return h;
}

struct Node {
    std::string name;
    UdpTransport transport;
    Session session;
    bool joined = false;
    std::string disconnectReason;
    std::vector<std::pair<PlayerId, std::string>> left;  // (id, reason)
    std::vector<PlayerId> joinedPeers;
    std::vector<std::string> chat;
    bool polling = true;

    Node(std::string n, SessionConfig cfg) : name(n), session(transport, (cfg.playerName = n, cfg)) {
        session.onJoined = [this] { joined = true; };
        session.onDisconnected = [this](const std::string& r) { disconnectReason = r.empty() ? "?" : r; };
        session.onPlayerLeft = [this](const PlayerInfo& p, const std::string& r) { left.push_back({p.id, r}); };
        session.onPlayerJoined = [this](const PlayerInfo& p) { joinedPeers.push_back(p.id); };
        session.onChat = [this](PlayerId, const std::string& t) { chat.push_back(t); };
    }
    bool HasLeft(PlayerId id) const {
        for (auto& l : left)
            if (l.first == id) return true;
        return false;
    }
};

using Nodes = std::vector<Node*>;

bool Pump(const Nodes& nodes, double seconds, const std::function<bool()>& done) {
    double end = Seconds() + seconds;
    while (Seconds() < end) {
        for (Node* n : nodes)
            if (n->polling) n->session.Poll();
        if (done && done()) return true;
        Sleep(1);
    }
    return done ? done() : true;
}

SessionConfig BaseConfig() {
    SessionConfig c;
    c.modBuild = 42;
    return c;
}

void SetSim(Node& n, float loss, float lat, float jitter, float dup, uint32_t seed) {
    UdpOptions o = n.transport.Options();
    o.simLoss = loss;
    o.simLatencyMs = lat;
    o.simJitterMs = jitter;
    o.simDuplicate = dup;
    o.simSeed = seed;
    n.transport.SetOptions(o);
}

std::vector<uint8_t> U32Msg(uint32_t v) {
    ByteWriter w(4);
    w.U32(v);
    return w.Take();
}

// ---- wire unit check ----
void TestWire() {
    ByteWriter w;
    w.U8(0xAB);
    w.U16(0xBEEF);
    w.U32(0xDEADBEEF);
    w.U64(0x0123456789ABCDEFull);
    w.F32(1.5f);
    w.F64(-2.25);
    w.VarU32(300);
    w.VarU64(0xFFFFFFFFFFFFFFFFull);
    w.VarI32(-5);
    w.Str("hello");
    ByteReader r(w.Span());
    bool ok = r.U8() == 0xAB && r.U16() == 0xBEEF && r.U32() == 0xDEADBEEF && r.U64() == 0x0123456789ABCDEFull &&
              r.F32() == 1.5f && r.F64() == -2.25 && r.VarU32() == 300 && r.VarU64() == 0xFFFFFFFFFFFFFFFFull &&
              r.VarI32() == -5 && r.Str() == "hello" && r.AtEnd() && r.Ok();
    bool le = w.Data()[1] == 0xEF && w.Data()[2] == 0xBE;
    r.U32();  // overrun
    bool failedFlag = r.Failed() && r.U8() == 0;
    ByteReader bad(w.Data(), 3);
    bad.Str();
    Check(ok && le && failedFlag && bad.Failed(), "wire: roundtrip + bounds");
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
    g_t0 = Seconds();
    SetLogSink(PrintLog);

    TestWire();

    Node host("Host", BaseConfig());
    if (!host.session.Host(0)) {
        Check(false, "host: bind");
        return 1;
    }
    std::string addr = "127.0.0.1:" + std::to_string(host.transport.BoundPort());
    std::printf("      host listening on %s\n", addr.c_str());

    // ---- 1. handshake ----
    auto g1 = std::make_unique<Node>("Alice", BaseConfig());
    auto g2 = std::make_unique<Node>("Bob", BaseConfig());
    g1->session.Join(addr);
    g2->session.Join(addr);
    Nodes all{&host, g1.get(), g2.get()};
    bool hs = Pump(all, 5, [&] {
        return g1->joined && g2->joined && host.session.Players().size() == 3 && g1->session.Players().size() == 3 &&
               g2->session.Players().size() == 3;
    });
    {
        std::string d = "players host/g1/g2 = " + std::to_string(host.session.Players().size()) + "/" +
                        std::to_string(g1->session.Players().size()) + "/" +
                        std::to_string(g2->session.Players().size()) +
                        ", ids " + std::to_string(g1->session.LocalId()) + "," + std::to_string(g2->session.LocalId());
        const PlayerInfo* hp = g1->session.FindPlayer(kHostPlayer);
        bool names = hp && hp->name == "Host" && host.session.FindPlayer(g1->session.LocalId()) &&
                     host.session.FindPlayer(g1->session.LocalId())->name == "Alice";
        Check(hs && names && g1->session.LocalId() != g2->session.LocalId() && g1->session.LocalId() != 0 &&
                  g2->session.LocalId() != 0,
              "handshake: hello/welcome/peer-joined", d);
    }
    if (!hs) return 1;
    PlayerId id1 = g1->session.LocalId(), id2 = g2->session.LocalId();

    // chat
    g1->session.SendChat("hi from alice");
    Pump(all, 2, [&] { return !host.chat.empty() && !g2->chat.empty(); });
    Check(host.chat.size() == 1 && g2->chat.size() == 1 && g2->chat[0] == "hi from alice", "chat: guest -> host -> guest");

    // ---- 2. reliable ordering under 20% loss + reordering ----
    SetSim(host, 0.2f, 20, 15, 0.05f, 11);
    SetSim(*g1, 0.2f, 20, 15, 0.05f, 22);
    SetSim(*g2, 0.2f, 20, 15, 0.05f, 33);
    const uint32_t kN = 1000, kRelayN = 300, kHostN = 500;
    std::vector<uint32_t> atHost, atG2relay, atG2host;
    PlayerId relayFrom = kNoPlayer;
    host.session.On(100, [&](PlayerId, ByteReader& r) { atHost.push_back(r.U32()); });
    host.session.On(101, [&](PlayerId, ByteReader&) {});
    g2->session.On(101, [&](PlayerId from, ByteReader& r) {
        relayFrom = from;
        atG2relay.push_back(r.U32());
    });
    g2->session.On(102, [&](PlayerId, ByteReader& r) { atG2host.push_back(r.U32()); });
    double t = Seconds();
    for (uint32_t i = 0; i < kN; ++i) g1->session.SendToHost(100, U32Msg(i));
    for (uint32_t i = 0; i < kRelayN; ++i) g1->session.Broadcast(101, U32Msg(i));
    for (uint32_t i = 0; i < kHostN; ++i) host.session.SendTo(id2, 102, U32Msg(i));
    bool got = Pump(all, 30, [&] {
        return atHost.size() >= kN && atG2relay.size() >= kRelayN && atG2host.size() >= kHostN;
    });
    auto inOrder = [](const std::vector<uint32_t>& v, uint32_t n) {
        if (v.size() != n) return false;
        for (uint32_t i = 0; i < n; ++i)
            if (v[i] != i) return false;
        return true;
    };
    {
        PeerStats st{};
        g1->transport.GetStats(g1->session.FindPlayer(kHostPlayer)->peer, st);
        char d[200];
        std::snprintf(d, sizeof(d), "%.2fs, host<-g1 %zu, relay g1->g2 %zu (from %u), host->g2 %zu, g1 resends %u",
                      Seconds() - t, atHost.size(), atG2relay.size(), relayFrom, atG2host.size(), st.resends);
        Check(got && inOrder(atHost, kN) && inOrder(atG2relay, kRelayN) && inOrder(atG2host, kHostN) && relayFrom == id1,
              "reliable: in order under 20% loss+jitter", d);
    }

    // ---- 3. 200 KB reliable message (still lossy) ----
    {
        std::vector<uint8_t> big(200 * 1024);
        uint32_t x = 12345;
        for (auto& b : big) {
            x = x * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(x >> 24);
        }
        uint64_t want = Fnv(big);
        size_t gotSize = 0;
        uint64_t gotHash = 0;
        int count = 0;
        g1->session.On(103, [&](PlayerId, ByteReader& r) {
            auto rest = r.Rest();
            gotSize = rest.size();
            gotHash = Fnv(rest);
            ++count;
        });
        t = Seconds();
        bool sent = host.session.SendTo(id1, 103, big);
        bool ok = sent && Pump(all, 30, [&] { return count > 0; });
        char d[160];
        std::snprintf(d, sizeof(d), "%.2fs, %zu bytes, hash %s", Seconds() - t, gotSize,
                      gotHash == want ? "match" : "MISMATCH");
        Check(ok && count == 1 && gotSize == big.size() && gotHash == want, "reliable: 200 KB fragmented message", d);
        bool tooBig = !host.session.SendTo(id1, 103, std::vector<uint8_t>(300 * 1024));
        Check(tooBig, "reliable: >256 KB refused");
    }

    // ---- RTT measured under 20 ms simulated one-way latency ----
    // Ping faster for this check so the loss estimate has enough samples: with ~36% round-trip loss and only a
    // handful of settled pings, "no ping lost" happened in about 1 run in 16.
    for (Node* n : all) {
        UdpOptions o = n->transport.Options();
        o.pingIntervalS = 0.05;
        n->transport.SetOptions(o);
    }
    Pump(all, 2.5, nullptr);
    {
        const PlayerInfo* p = host.session.FindPlayer(id1);
        PeerStats st{};
        host.transport.GetStats(p->peer, st);
        char d[160];
        std::snprintf(d, sizeof(d), "session rtt %.1f ms, transport rtt %.1f ms, loss %.0f%%", p->rttMs, st.rttMs,
                      st.loss * 100);
        Check(p->rttMs > 20 && st.rttMs > 20 && st.loss > 0.05f && st.loss < 0.6f, "stats: rtt + loss reported", d);
    }
    for (Node* n : all) {
        UdpOptions o = n->transport.Options();
        o.pingIntervalS = UdpOptions{}.pingIntervalS;
        n->transport.SetOptions(o);
    }

    // ---- 4. unreliable delivery (sim off) ----
    SetSim(host, 0, 0, 0, 0, 1);
    SetSim(*g1, 0, 0, 0, 0, 1);
    SetSim(*g2, 0, 0, 0, 0, 1);
    Pump(all, 0.5, nullptr);
    {
        int c1 = 0, c2 = 0;
        g1->session.On(104, [&](PlayerId, ByteReader&) { ++c1; });
        g2->session.On(104, [&](PlayerId, ByteReader&) { ++c2; });
        for (uint32_t i = 0; i < 200; ++i) {
            host.session.Broadcast(104, U32Msg(i), Reliability::Unreliable);
            if (i % 10 == 9) Pump(all, 0.005, nullptr);
        }
        Pump(all, 1, [&] { return c1 >= 200 && c2 >= 200; });
        bool tooBig = !host.session.Broadcast(104, std::vector<uint8_t>(1500), Reliability::Unreliable);
        Check(c1 >= 190 && c2 >= 190 && tooBig, "unreliable: delivery",
              std::to_string(c1) + "/200, " + std::to_string(c2) + "/200; oversize refused=" + (tooBig ? "yes" : "no"));
        // unreliable under 20% loss: some lost, none duplicated beyond sim
        SetSim(host, 0.2f, 5, 2, 0, 7);
        c1 = 0;
        for (uint32_t i = 0; i < 200; ++i) {
            host.session.SendTo(id1, 104, U32Msg(i), Reliability::Unreliable);
            if (i % 10 == 9) Pump(all, 0.005, nullptr);
        }
        Pump(all, 1, nullptr);
        Check(c1 > 100 && c1 < 200, "unreliable: lossy delivery is partial", std::to_string(c1) + "/200 arrived");
        SetSim(host, 0, 0, 0, 0, 1);
    }

    // ---- 5. version mismatch ----
    {
        SessionConfig bad = BaseConfig();
        bad.protocolVersion = kProtocolVersion + 1;
        Node g3("Mallory", bad);
        g3.session.Join(addr);
        Nodes n{&host, g1.get(), g2.get(), &g3};
        bool rej = Pump(n, 5, [&] { return !g3.disconnectReason.empty(); });
        Pump(n, 0.2, nullptr);
        Check(rej && g3.disconnectReason.find("version mismatch") != std::string::npos && !g3.joined &&
                  host.session.Players().size() == 3,
              "version mismatch rejected", "\"" + g3.disconnectReason + "\"");
    }

    // ---- 6. max players ----
    {
        Node g3("Carol", BaseConfig());
        Node g4("Dave", BaseConfig());
        g3.session.Join(addr);
        Nodes n{&host, g1.get(), g2.get(), &g3};
        Pump(n, 5, [&] { return g3.joined; });
        g4.session.Join(addr);
        Nodes n2{&host, g1.get(), g2.get(), &g3, &g4};
        Pump(n2, 5, [&] { return !g4.disconnectReason.empty(); });
        Check(g3.joined && host.session.Players().size() == 4 && g4.disconnectReason.find("full") != std::string::npos,
              "max 4 players, 5th rejected", "\"" + g4.disconnectReason + "\"");
        g3.session.Leave();
        Pump(n, 2, [&] { return host.session.Players().size() == 3; });
    }

    // ---- 7. clean disconnect ----
    {
        host.left.clear();
        g1->left.clear();
        t = Seconds();
        g2->session.Leave();
        bool ok = Pump(all, 3, [&] { return host.HasLeft(id2) && g1->HasLeft(id2); });
        std::string reason;
        for (auto& l : host.left)
            if (l.first == id2) reason = l.second;
        char d[160];
        std::snprintf(d, sizeof(d), "%.0f ms, host reason \"%s\"", (Seconds() - t) * 1000, reason.c_str());
        Check(ok && reason == "closed by remote" && host.session.Players().size() == 2 &&
                  g1->session.Players().size() == 2,
              "clean disconnect seen by host + guest", d);
    }

    // ---- 8. timeout detection (guest stops responding) ----
    {
        host.left.clear();
        g1->polling = false;  // simulate a crashed / frozen guest
        t = Seconds();
        bool ok = Pump(all, 15, [&] { return host.HasLeft(id1); });
        double el = Seconds() - t;
        std::string reason = host.left.empty() ? "" : host.left[0].second;
        char d[160];
        std::snprintf(d, sizeof(d), "detected after %.1f s, reason \"%s\"", el, reason.c_str());
        Check(ok && el > 9.5 && el < 12 && reason == "timed out", "timeout detection (10 s)", d);
        g1.reset();
    }

    // ---- 9. host leaves -> guest notified ----
    {
        Node g5("Eve", BaseConfig());
        g5.session.Join(addr);
        Nodes n{&host, &g5};
        Pump(n, 5, [&] { return g5.joined; });
        host.session.Leave();
        bool ok = Pump(n, 3, [&] { return !g5.disconnectReason.empty(); });
        Check(ok && g5.joined && g5.disconnectReason == "host closed the session", "host leave -> guest disconnected",
              "\"" + g5.disconnectReason + "\"");
    }

    // ---- 10. connect to nothing ----
    {
        UdpOptions o;
        o.connectTimeoutS = 2.0;
        Node g6("Frank", BaseConfig());
        g6.transport.SetOptions(o);
        g6.session.Join(addr);  // host is gone
        Nodes n{&g6};
        t = Seconds();
        bool ok = Pump(n, 5, [&] { return !g6.disconnectReason.empty(); });
        Check(ok && g6.disconnectReason.find("could not connect") != std::string::npos, "connect failure reported",
              "\"" + g6.disconnectReason + "\"");
    }

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
