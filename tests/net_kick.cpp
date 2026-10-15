// net_kick: Session::Kick / KickAndBan (docs/protocol.md "Kick and ban"), host + guests over UdpTransport on
// 127.0.0.1 in one process. The host's transport is wrapped so each incoming connection can be given a made-up
// address and SteamID (bans key by those; on one PC every guest is 127.0.0.1 and nobody has a SteamID).
// Prints PASS/FAIL per check, exits non-zero on any failure. "-v" prints net info logs.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
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
    std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void PrintLog(int level, const char* msg) {
    if (level >= 2 || g_verbose) std::printf("    [%7.3f] %s\n", Seconds() - g_t0, msg);
}

// Test seam: a UdpTransport whose incoming peers report the identity that was "next" when they connected, as if
// they came from that address (UDP) or over Steam with that SteamID.
struct Identity {
    std::string address;
    uint64_t steamId = 0;
};

class IdentityTransport final : public Transport {
public:
    UdpTransport udp;
    Identity next;
    std::map<PeerId, Identity> ids;

    bool Host(uint16_t port) override { return udp.Host(port); }
    PeerId Connect(std::string_view address) override { return udp.Connect(address); }
    bool Send(PeerId peer, std::span<const uint8_t> data, Reliability rel) override { return udp.Send(peer, data, rel); }
    void Poll(const EventHandler& handler) override {
        udp.Poll([&](const Event& e) {
            if (e.type == EventType::Connected) ids[e.peer] = next;
            handler(e);
        });
    }
    void Disconnect(PeerId peer) override { udp.Disconnect(peer); }
    void Close() override { udp.Close(); }
    bool GetStats(PeerId peer, PeerStats& out) const override { return udp.GetStats(peer, out); }
    bool IsHosting() const override { return udp.IsHosting(); }
    const char* Name() const override { return "udp+identity"; }
    std::string PeerAddress(PeerId peer) const override {
        auto it = ids.find(peer);
        return it == ids.end() ? std::string() : it->second.address;
    }
    uint64_t PeerSteamId(PeerId peer) const override {
        auto it = ids.find(peer);
        return it == ids.end() ? 0 : it->second.steamId;
    }
    void MarkEstablished(PeerId peer) override { udp.MarkEstablished(peer); }
};

struct Node {
    std::string name;
    std::unique_ptr<Transport> transport;
    Session session;
    bool joined = false;
    std::string disconnectReason;
    std::vector<std::pair<PlayerId, std::string>> left;  // (id, reason)

    Node(std::string n, SessionConfig cfg, std::unique_ptr<Transport> t)
        : name(n), transport(std::move(t)), session(*transport, (cfg.playerName = n, cfg)) {
        session.onJoined = [this] { joined = true; };
        session.onDisconnected = [this](const std::string& r) { disconnectReason = r.empty() ? "?" : r; };
        session.onPlayerLeft = [this](const PlayerInfo& p, const std::string& r) { left.push_back({p.id, r}); };
    }
    std::string LeftReason(PlayerId id) const {
        for (auto& l : left)
            if (l.first == id) return l.second;
        return {};
    }
};

SessionConfig Cfg() {
    SessionConfig c;
    c.modBuild = 42;
    return c;
}

std::unique_ptr<Node> MakeHost(UdpOptions opt = {}) {
    auto t = std::make_unique<IdentityTransport>();
    t->udp.SetOptions(opt);
    auto h = std::make_unique<Node>("Host", Cfg(), std::move(t));
    if (!h->session.Host(0)) Check(false, "host: bind");
    return h;
}
IdentityTransport& Ids(Node& host) { return static_cast<IdentityTransport&>(*host.transport); }
std::string Addr(Node& host) { return "127.0.0.1:" + std::to_string(Ids(host).udp.BoundPort()); }

std::unique_ptr<Node> MakeGuest(const std::string& name) {
    return std::make_unique<Node>(name, Cfg(), std::make_unique<UdpTransport>());
}

using Nodes = std::vector<Node*>;
bool Pump(const Nodes& nodes, double seconds, const std::function<bool()>& done) {
    double end = Seconds() + seconds;
    while (Seconds() < end) {
        for (Node* n : nodes) n->session.Poll();
        if (done && done()) return true;
        Sleep(1);
    }
    return done ? done() : true;
}

// The guest (as the host sees it) and its transport peer on the host.
const PlayerInfo* HostView(Node& host, const std::string& name) {
    for (const auto& p : host.session.Players())
        if (p.name == name) return &p;
    return nullptr;
}

// A guest with this identity joins `host`; true if it got in. The others keep being polled.
bool Join(Node& host, Node& g, const Identity& id, const Nodes& others = {}) {
    Ids(host).next = id;
    g.joined = false;
    g.disconnectReason.clear();
    g.session.Join(Addr(host));
    Nodes all = others;
    all.push_back(&host);
    all.push_back(&g);
    Pump(all, 8, [&] { return g.joined || !g.disconnectReason.empty(); });
    return g.joined;
}

// A fresh guest with this identity tries to join; returns "joined" or its disconnect reason (and the reject).
std::string TryJoin(Node& host, const Identity& id, RejectReason* why = nullptr) {
    auto g = MakeGuest("Again");
    const bool ok = Join(host, *g, id);
    if (why) *why = g->session.LastReject();
    std::string out = ok ? "joined" : g->disconnectReason;
    g->session.Leave();
    Pump({&host}, 0.3, nullptr);
    return out;
}

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
    g_t0 = Seconds();
    SetLogSink(PrintLog);

    // ---- 1. kick: the player gets the reason and is disconnected, the others get PeerLeft ----
    {
        UdpOptions lossy;  // the host's packets: the notice must arrive through loss before the host drops the peer
        lossy.simLoss = 0.2f;
        lossy.simLatencyMs = 20;
        lossy.simSeed = 7;
        auto host = MakeHost(lossy);
        auto a = MakeGuest("Alice");
        auto b = MakeGuest("Bob");
        const bool in = Join(*host, *a, {"10.0.0.1", 0}) && Join(*host, *b, {"10.0.0.2", 0}, {a.get()});
        Check(in && host->session.Players().size() == 3, "setup: host + 2 guests");
        const PlayerInfo* ha = HostView(*host, "Alice");
        const PlayerId aid = ha ? ha->id : kNoPlayer;
        const PeerId apeer = ha ? ha->peer : kInvalidPeer;

        const bool kicked = host->session.Kick(aid, "too loud");
        Nodes all{host.get(), a.get(), b.get()};
        Pump(all, 5, [&] { return !a->disconnectReason.empty() && !b->LeftReason(aid).empty(); });
        Check(kicked && a->disconnectReason == "You were kicked by the host: too loud" && a->session.WasKicked() &&
                  !a->session.WasBanned() && a->session.State() == SessionState::Disconnected,
              "kicked guest gets the reason, is disconnected", "\"" + a->disconnectReason + "\"");
        Check(Has(b->LeftReason(aid), "kicked by the host") && b->session.Players().size() == 2 &&
                  Has(host->LeftReason(aid), "kicked by the host") && host->session.Players().size() == 2,
              "other guest gets PeerLeft (kicked)", "\"" + b->LeftReason(aid) + "\"");
        Pump(all, 1.5, nullptr);  // past the linger: the host has dropped the connection
        PeerStats st;
        Check(!Ids(*host).udp.GetStats(apeer, st) && b->session.State() == SessionState::Connected &&
                  host->session.BanList().empty(),
              "host dropped the kicked connection, Bob stays");

        // a kicked (not banned) player can come back
        Check(Join(*host, *a, {"10.0.0.1", 0}, {b.get()}) && host->session.Players().size() == 3,
              "kicked (not banned) player can rejoin");

        // ---- 2. guests cannot kick ----
        const PlayerInfo* hb = HostView(*host, "Bob");
        const PlayerId bid = hb ? hb->id : kNoPlayer;
        const bool apiRefused = !a->session.Kick(bid, "x") && !a->session.KickAndBan(bid, "x") &&
                                !a->session.Kick(kHostPlayer) && !host->session.Kick(kHostPlayer) &&
                                !host->session.Kick(3, "nobody");
        // A guest forging the host-only Kicked message (0x0C) to the host: ignored.
        const PlayerInfo* hostOfA = a->session.FindPlayer(kHostPlayer);
        ByteWriter w(16);
        w.U8(0x0C);
        w.U8(2);
        w.Str("forged");
        const bool sent = hostOfA && a->transport->Send(hostOfA->peer, w.Span(), Reliability::Reliable);
        // ... and as an app message to Bob through the relay (it arrives as App, never as Kicked)
        const uint8_t payload[1] = {0x0C};
        a->session.SendTo(bid, 0x0001, payload);
        Pump(all, 1.0, nullptr);
        Check(apiRefused && sent && a->disconnectReason.empty() && b->disconnectReason.empty() &&
                  !b->session.WasKicked() && host->session.Players().size() == 3 &&
                  a->session.State() == SessionState::Connected && b->session.State() == SessionState::Connected,
              "guests cannot kick (API refuses, forged msg ignored)");
    }

    // ---- 3. kick and ban by address (UDP) ----
    {
        auto host = MakeHost();
        auto a = MakeGuest("Alice");
        Join(*host, *a, {"10.0.0.1", 0});
        const PlayerInfo* ha = HostView(*host, "Alice");
        const bool kicked = ha && host->session.KickAndBan(ha->id);
        Pump({host.get(), a.get()}, 3, [&] { return !a->disconnectReason.empty(); });
        const auto& bans = host->session.BanList();
        Check(kicked && a->session.WasBanned() && a->disconnectReason == "You were kicked and banned by the host" &&
                  bans.size() == 1 && bans[0].address == "10.0.0.1" && bans[0].steamId == 0 && bans[0].name == "Alice",
              "kick and ban: guest told, ban list entry", "\"" + a->disconnectReason + "\"");
        RejectReason why = RejectReason::None;
        const std::string again = TryJoin(*host, {"10.0.0.1", 0}, &why);
        const std::string other = TryJoin(*host, {"10.0.0.2", 0});
        Check(Has(again, "You are banned from this session") && why == RejectReason::Banned && other == "joined",
              "banned address refused, other address joins", "\"" + again + "\", \"" + other + "\"");

        // Unban (for a later UI)
        const bool unbanned = host->session.Unban(0) && !host->session.Unban(0);
        Check(unbanned && TryJoin(*host, {"10.0.0.1", 0}) == "joined", "unban: the address can join again");
    }

    // ---- 4. kick and ban by SteamID ----
    {
        auto host = MakeHost();
        const uint64_t sid = 76561197960287930ull;
        auto a = MakeGuest("SteamGuy");
        Join(*host, *a, {"", sid});
        const PlayerInfo* ha = HostView(*host, "SteamGuy");
        const bool kicked = ha && host->session.KickAndBan(ha->id, "cheating");
        Pump({host.get(), a.get()}, 3, [&] { return !a->disconnectReason.empty(); });
        const auto& bans = host->session.BanList();
        Check(kicked && bans.size() == 1 && bans[0].steamId == sid && bans[0].address.empty() &&
                  a->disconnectReason == "You were kicked and banned by the host: cheating",
              "kick and ban a Steam peer", "\"" + a->disconnectReason + "\"");
        RejectReason why = RejectReason::None;
        const std::string viaSteam = TryJoin(*host, {"", sid}, &why);
        const std::string viaOtherAddr = TryJoin(*host, {"10.0.0.9", sid});
        const std::string otherId = TryJoin(*host, {"", sid + 1});
        Check(Has(viaSteam, "You are banned from this session") && why == RejectReason::Banned &&
                  Has(viaOtherAddr, "You are banned") && otherId == "joined",
              "banned SteamID refused, other SteamID joins",
              "\"" + viaSteam + "\", \"" + viaOtherAddr + "\", \"" + otherId + "\"");

        // ---- 5. the ban list clears when the host stops hosting / starts a new session ----
        host->session.Leave();
        const bool clearedOnLeave = host->session.BanList().empty();
        host->session.Host(0);
        const std::string afterNew = TryJoin(*host, {"", sid});
        // and Host() while still hosting (a new session) clears it too
        auto c = MakeGuest("Carol");
        Join(*host, *c, {"10.0.0.3", 0});
        const PlayerInfo* hc = HostView(*host, "Carol");
        if (hc) host->session.KickAndBan(hc->id);
        Pump({host.get(), c.get()}, 3, [&] { return !c->disconnectReason.empty(); });
        const size_t before = host->session.BanList().size();
        host->session.Host(0);
        Check(clearedOnLeave && afterNew == "joined" && before == 1 && host->session.BanList().empty() &&
                  TryJoin(*host, {"10.0.0.3", 0}) == "joined",
              "ban list clears on a new session", "\"" + afterNew + "\"");
    }

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
