// net_reconnect: a guest whose connection is lost (not a clean leave, kick or ban) keeps playing and rejoins the same
// host; the host holds his slot meanwhile (docs/protocol.md "Reconnect"). Host + guests over UdpTransport on 127.0.0.1
// in one process; a "lost connection" is made with the transport's test simulation (simLoss = 1: every datagram the
// node sends is dropped, so the other side times out, then this side too). Section 10: four players, two guests lost
// at once, both back into their own slots one after the other, no newcomer meanwhile.
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
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void PrintLog(int level, const char* msg) {
    if (level >= 2 || g_verbose) std::printf("    [%7.3f] %s\n", Seconds() - g_t0, msg);
}

UdpOptions FastUdp(double timeoutS = 1.0) {
    UdpOptions o;
    o.timeoutS = timeoutS;
    o.connectTimeoutS = 1.5;
    o.pingIntervalS = 0.2;
    return o;
}

SessionConfig Cfg(const std::string& name) {
    SessionConfig c;
    c.modBuild = 42;
    c.playerName = name;
    c.reconnectHoldS = 10;
    c.reconnectWindowS = 10;
    c.reconnectFirstDelayS = 0.2;
    c.reconnectMaxDelayS = 1.0;
    c.reconnectAttemptS = 2.0;
    return c;
}

struct Node {
    std::string name;
    UdpTransport* udp = nullptr;
    std::unique_ptr<UdpTransport> transport;
    Session session;
    bool joined = false;
    std::string disconnectReason, lostReason;
    int reconnects = 0;
    bool lastSameSlot = false;
    std::vector<std::pair<PlayerId, std::string>> left, dropped;
    std::vector<PlayerId> resumed, joinedIds;
    std::vector<uint8_t> appFrom;  // senders of app message 0x0001

    Node(const std::string& n, SessionConfig cfg, UdpOptions opt)
        : name(n), transport(std::make_unique<UdpTransport>(opt)), session(*transport, cfg) {
        udp = transport.get();
        session.onJoined = [this] { joined = true; };
        session.onDisconnected = [this](const std::string& r) { disconnectReason = r.empty() ? "?" : r; };
        session.onPlayerLeft = [this](const PlayerInfo& p, const std::string& r) { left.push_back({p.id, r}); };
        session.onPlayerJoined = [this](const PlayerInfo& p) { joinedIds.push_back(p.id); };
        session.onPlayerDropped = [this](const PlayerInfo& p, const std::string& r) { dropped.push_back({p.id, r}); };
        session.onPlayerResumed = [this](const PlayerInfo& p) { resumed.push_back(p.id); };
        session.onConnectionLost = [this](const std::string& r) { lostReason = r; };
        session.onReconnected = [this](bool same) {
            ++reconnects;
            lastSameSlot = same;
        };
        session.On(0x0001, [this](PlayerId from, ByteReader&) { appFrom.push_back(from); });
    }
    void Block(bool on) {  // every datagram this node sends is lost
        UdpOptions o = udp->Options();
        o.simLoss = on ? 1.0f : 0.0f;
        udp->SetOptions(o);
    }
    std::string LeftReason(PlayerId id) const {
        for (auto& l : left)
            if (l.first == id) return l.second;
        return {};
    }
    bool Dropped(PlayerId id) const {
        for (auto& d : dropped)
            if (d.first == id) return true;
        return false;
    }
    bool Resumed(PlayerId id) const {
        for (PlayerId r : resumed)
            if (r == id) return true;
        return false;
    }
};

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

std::unique_ptr<Node> MakeHost(SessionConfig cfg = Cfg("Host"), UdpOptions opt = FastUdp()) {
    auto h = std::make_unique<Node>("Host", cfg, opt);
    if (!h->session.Host(0)) Check(false, "host: bind");
    return h;
}
std::string Addr(Node& host) { return "127.0.0.1:" + std::to_string(host.udp->BoundPort()); }

std::unique_ptr<Node> MakeGuest(const std::string& name, SessionConfig cfg = {}, UdpOptions opt = FastUdp()) {
    if (cfg.modBuild == 0) cfg = Cfg(name);
    cfg.playerName = name;
    return std::make_unique<Node>(name, cfg, opt);
}

bool Join(Node& host, Node& g, const Nodes& others = {}) {
    g.joined = false;
    g.disconnectReason.clear();
    g.session.Join(Addr(host));
    Nodes all = others;
    all.push_back(&host);
    all.push_back(&g);
    Pump(all, 6, [&] { return g.joined || !g.disconnectReason.empty(); });
    return g.joined;
}

const PlayerInfo* Find(const Session& s, PlayerId id) { return s.FindPlayer(id); }

// Raw Hello (as Session::SendHello builds it) with a resume token trailer.
ByteWriter RawHello(const std::string& name, const ResumeToken& token) {
    ByteWriter w(96);
    w.U8(0x01);
    w.U32(0x504F434D);
    w.U32(kProtocolVersion);
    w.U32(42);
    w.Str(name);
    w.Str("");
    w.U8(1);
    w.Bytes(token.data(), token.size());
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
    g_t0 = Seconds();
    SetLogSink(PrintLog);

    // ---- 1. lost connection: the slot is held, the guest rejoins into it, the others see drop + return ----
    {
        auto host = MakeHost();
        auto a = MakeGuest("Alice");
        auto b = MakeGuest("Bob");
        const bool in = Join(*host, *a) && Join(*host, *b, {a.get()});
        Check(in && host->session.Players().size() == 3 && a->session.HasResumeToken(), "setup: host + 2 guests, token");
        const PlayerId aid = a->session.LocalId();
        const uint32_t seqBefore = Find(host->session, aid) ? Find(host->session, aid)->joinSeq : 0;
        Nodes all{host.get(), a.get(), b.get()};

        a->Block(true);
        Pump(all, 6, [&] { return host->Dropped(aid) && b->Dropped(aid) && a->session.IsReconnecting(); });
        const auto held = host->session.DroppedPlayers();
        Check(host->Dropped(aid) && !Find(host->session, aid) && host->session.Players().size() == 2 &&
                  held.size() == 1 && held[0].info.id == aid && held[0].leftS > 5 && host->LeftReason(aid).empty(),
              "host: lost guest out of Players(), slot held", host->dropped.empty() ? "" : host->dropped[0].second);
        Check(b->Dropped(aid) && !Find(b->session, aid) && b->session.DroppedPlayers().size() == 1 &&
                  b->LeftReason(aid).empty(),
              "other guest: PeerDropped, no PeerLeft");
        Check(a->session.IsReconnecting() && !a->session.IsActive() && a->session.Players().size() == 1 &&
                  a->session.LocalId() == aid && a->disconnectReason.empty() &&
                  a->lostReason.find("connection to host lost") == 0 && a->session.ReconnectLeftS() > 5,
              "lost guest: reconnecting, only himself, id kept", "\"" + a->lostReason + "\"");
        const uint8_t one[1] = {1};
        Check(!a->session.SendToHost(0x0001, one) && !a->session.Broadcast(0x0001, one),
              "reconnecting guest cannot send");

        // a newcomer meanwhile does not get the held id
        auto c = MakeGuest("Carol");
        const bool cin = Join(*host, *c, {a.get(), b.get()});
        Check(cin && c->session.LocalId() != aid && c->session.LocalId() != b->session.LocalId(),
              "newcomer does not take the held slot", "id " + std::to_string(c->session.LocalId()));
        all.push_back(c.get());

        a->Block(false);
        Pump(all, 8, [&] { return a->reconnects > 0 && b->Resumed(aid) && c->joinedIds.size() >= 1; });
        const PlayerInfo* back = Find(host->session, aid);
        Check(a->reconnects == 1 && a->lastSameSlot && a->session.State() == SessionState::Connected &&
                  a->session.LocalId() == aid && a->session.Players().size() == 4 && a->session.ReconnectAttempts() >= 1,
              "guest back in the same slot", "attempts " + std::to_string(a->session.ReconnectAttempts()));
        Check(back && host->Resumed(aid) && host->session.Players().size() == 4 && back->joinSeq > seqBefore &&
                  host->session.DroppedPlayers().empty(),
              "host: resumed, joinSeq grew, hold gone");
        Check(b->Resumed(aid) && Find(b->session, aid) && b->session.DroppedPlayers().empty() &&
                  !c->Resumed(aid) && Find(c->session, aid),
              "others: back (Bob resumed, Carol sees a join)");
        a->appFrom.clear();
        host->appFrom.clear();
        a->session.SendToHost(0x0001, one);
        host->session.SendTo(aid, 0x0001, one);
        Pump(all, 2, [&] { return !host->appFrom.empty() && !a->appFrom.empty(); });
        Check(!host->appFrom.empty() && host->appFrom[0] == aid && !a->appFrom.empty(),
              "app messages flow again after the reconnect");
    }

    // ---- 2. a held slot counts as taken: the session is full ----
    {
        SessionConfig hc = Cfg("Host");
        hc.maxPlayers = 3;
        auto host = MakeHost(hc);
        auto a = MakeGuest("Alice");
        auto b = MakeGuest("Bob");
        Join(*host, *a);
        Join(*host, *b, {a.get()});
        const PlayerId aid = a->session.LocalId();
        Nodes all{host.get(), a.get(), b.get()};
        a->Block(true);
        Pump(all, 6, [&] { return host->Dropped(aid) && a->session.IsReconnecting(); });
        auto c = MakeGuest("Carol");
        const bool cin = Join(*host, *c, {a.get(), b.get()});
        Check(!cin && c->session.LastReject() == RejectReason::Full, "newcomer refused while the slot is held",
              "\"" + c->disconnectReason + "\"");
        a->Block(false);
        Pump(all, 8, [&] { return a->reconnects > 0; });
        Check(a->reconnects == 1 && a->lastSameSlot && host->session.Players().size() == 3,
              "held guest still gets back in");
    }

    // ---- 3. the hold expires: PeerLeft to everyone; a later rejoin is a new player ----
    {
        SessionConfig hc = Cfg("Host");
        hc.reconnectHoldS = 1.5;
        auto host = MakeHost(hc);
        SessionConfig gc = Cfg("Alice");
        gc.reconnectWindowS = 15;
        auto a = MakeGuest("Alice", gc);
        auto b = MakeGuest("Bob");
        Join(*host, *a);
        Join(*host, *b, {a.get()});
        const PlayerId aid = a->session.LocalId();
        Nodes all{host.get(), a.get(), b.get()};
        a->Block(true);
        Pump(all, 8, [&] { return !host->LeftReason(aid).empty() && !b->LeftReason(aid).empty(); });
        Check(host->Dropped(aid) && host->LeftReason(aid) == "lost the connection and did not come back" &&
                  b->LeftReason(aid) == host->LeftReason(aid) && host->session.DroppedPlayers().empty() &&
                  b->session.DroppedPlayers().empty() && a->session.IsReconnecting(),
              "hold expired: PeerLeft, guest still trying", "\"" + host->LeftReason(aid) + "\"");
        a->Block(false);
        Pump(all, 8, [&] { return a->reconnects > 0 || !a->disconnectReason.empty(); });
        Check(a->reconnects == 1 && !a->lastSameSlot && a->session.State() == SessionState::Connected &&
                  host->session.Players().size() == 3 && b->session.Players().size() == 3,
              "late rejoin: admitted as a new player", "id " + std::to_string(a->session.LocalId()));
    }

    // ---- 4. the guest's window expires: the session ends with a reason ----
    {
        auto host = MakeHost();
        SessionConfig gc = Cfg("Alice");
        gc.reconnectWindowS = 2.0;
        auto a = MakeGuest("Alice", gc);
        Join(*host, *a);
        Nodes all{host.get(), a.get()};
        a->Block(true);
        Pump(all, 10, [&] { return !a->disconnectReason.empty(); });
        Check(a->session.State() == SessionState::Disconnected &&
                  a->disconnectReason.find("could not reconnect within 2 s") != std::string::npos &&
                  a->session.ReconnectAttempts() >= 1,
              "window expired: disconnected with a reason", "\"" + a->disconnectReason + "\"");
    }

    // ---- 5. no reconnect after a clean leave, a host leave or a kick ----
    {
        auto host = MakeHost();
        auto a = MakeGuest("Alice");
        Join(*host, *a);
        const PlayerId aid = a->session.LocalId();
        a->session.Leave();
        Pump({host.get()}, 3, [&] { return !host->LeftReason(aid).empty(); });
        Check(host->LeftReason(aid) == "closed by remote" && !host->Dropped(aid) &&
                  host->session.DroppedPlayers().empty(),
              "guest leaves: removed at once, no hold", "\"" + host->LeftReason(aid) + "\"");

        auto b = MakeGuest("Bob");
        Join(*host, *b);
        host->session.Leave();
        Pump({b.get()}, 3, [&] { return !b->disconnectReason.empty(); });
        Check(b->disconnectReason == "host closed the session" && b->lostReason.empty() &&
                  b->session.State() == SessionState::Disconnected,
              "host leaves: guest disconnected, no reconnect", "\"" + b->disconnectReason + "\"");

        auto host2 = MakeHost();
        auto c = MakeGuest("Carol");
        Join(*host2, *c);
        host2->session.Kick(c->session.LocalId(), "bye");
        Pump({host2.get(), c.get()}, 3, [&] { return !c->disconnectReason.empty(); });
        Pump({host2.get(), c.get()}, 1.5, nullptr);
        Check(c->session.WasKicked() && c->lostReason.empty() && c->session.State() == SessionState::Disconnected &&
                  host2->session.DroppedPlayers().empty() && host2->dropped.empty(),
              "kicked: no reconnect, no hold");
    }

    // ---- 6. the guest reconnects before the host noticed the old connection was gone: it is replaced ----
    {
        auto host = MakeHost(Cfg("Host"), FastUdp(4.0));  // the host waits 4 s, the guest only 1 s
        auto a = MakeGuest("Alice");
        Join(*host, *a);
        const PlayerId aid = a->session.LocalId();
        Nodes all{host.get(), a.get()};
        host->Block(true);  // the guest hears nothing; the host still hears the guest
        Pump(all, 4, [&] { return a->session.IsReconnecting(); });
        host->Block(false);
        Pump(all, 6, [&] { return a->reconnects > 0; });
        Check(a->reconnects == 1 && a->lastSameSlot && host->Dropped(aid) && host->Resumed(aid) &&
                  host->session.Players().size() == 2,
              "new connection replaces the old one (same slot)");
        Pump(all, 5, nullptr);  // past the host's old timeout: nothing more happens to the resumed player
        Check(host->session.Players().size() == 2 && host->LeftReason(aid).empty() &&
                  a->session.State() == SessionState::Connected && host->dropped.size() == 1,
              "old connection's timeout does not touch him");
    }

    // ---- 7. a new Hello on the existing connection (Steam P2P keeps one per SteamID): drop + resume ----
    {
        auto host = MakeHost();
        auto a = MakeGuest("Alice");
        Join(*host, *a);
        const PlayerId aid = a->session.LocalId();
        const uint32_t seqBefore = Find(host->session, aid)->joinSeq;
        const PlayerInfo* hostOfA = a->session.FindPlayer(kHostPlayer);
        ByteWriter w = RawHello("Alice", a->session.CurrentResumeToken());
        const bool sent = hostOfA && a->udp->Send(hostOfA->peer, w.Span(), Reliability::Reliable);
        Nodes all{host.get(), a.get()};
        Pump(all, 3, [&] { return host->Resumed(aid); });
        const PlayerInfo* back = Find(host->session, aid);
        Check(sent && host->Dropped(aid) && host->Resumed(aid) && back && back->joinSeq > seqBefore &&
                  host->session.Players().size() == 2 && a->session.State() == SessionState::Connected,
              "Hello on the live connection: dropped and resumed");
    }

    // ---- 8. a wrong token does not take a held slot; a password host lets a token through without it ----
    {
        SessionConfig hc = Cfg("Host");
        hc.password = "secret";
        auto host = MakeHost(hc);
        SessionConfig gc = Cfg("Alice");
        gc.password = "secret";
        auto a = MakeGuest("Alice", gc);
        Join(*host, *a);
        const PlayerId aid = a->session.LocalId();
        a->session.SetPassword("");  // the resume must not need it
        Nodes all{host.get(), a.get()};
        a->Block(true);
        Pump(all, 6, [&] { return host->Dropped(aid) && a->session.IsReconnecting(); });

        // a forged Hello with a made-up token from a raw connection: treated as a new join (here: asked for the
        // password), never given the held slot
        UdpTransport raw(FastUdp());
        const PeerId hp = raw.Connect(Addr(*host));
        bool connected = false, challenged = false, welcomed = false;
        const double end = Seconds() + 3;
        while (Seconds() < end && !challenged && !welcomed) {
            raw.Poll([&](const Event& e) {
                if (e.type == EventType::Connected && e.peer == hp) {
                    connected = true;
                    ResumeToken fake{};
                    for (size_t i = 0; i < fake.size(); ++i) fake[i] = static_cast<uint8_t>(i * 7 + 1);
                    ByteWriter h = RawHello("Mallory", fake);
                    raw.Send(hp, h.Span(), Reliability::Reliable);
                } else if (e.type == EventType::Message && !e.data.empty()) {
                    challenged |= e.data[0] == 0x0A;
                    welcomed |= e.data[0] == 0x02;
                }
            });
            host->session.Poll();
            a->session.Poll();
            Sleep(1);
        }
        raw.Close();
        Check(connected && challenged && !welcomed && host->session.DroppedPlayers().size() == 1,
              "forged token: normal join (password asked), slot kept");

        a->Block(false);
        Pump(all, 8, [&] { return a->reconnects > 0 || !a->disconnectReason.empty(); });
        Check(a->reconnects == 1 && a->lastSameSlot, "token resumes without the password",
              "\"" + a->disconnectReason + "\"");
    }

    // ---- 9. kick and ban a held player: slot freed, his reconnect refused ----
    {
        auto host = MakeHost();
        auto a = MakeGuest("Alice");
        auto b = MakeGuest("Bob");
        Join(*host, *a);
        Join(*host, *b, {a.get()});
        const PlayerId aid = a->session.LocalId();
        Nodes all{host.get(), a.get(), b.get()};
        a->Block(true);
        Pump(all, 6, [&] { return host->Dropped(aid) && a->session.IsReconnecting(); });
        const bool kicked = host->session.KickAndBan(aid, "gone");
        Pump(all, 2, [&] { return !b->LeftReason(aid).empty(); });
        Check(kicked && host->session.DroppedPlayers().empty() && host->session.BanList().size() == 1 &&
                  b->LeftReason(aid) == "kicked and banned by the host: gone" && b->session.DroppedPlayers().empty(),
              "held player kicked and banned", "\"" + b->LeftReason(aid) + "\"");
        a->Block(false);
        Pump(all, 8, [&] { return !a->disconnectReason.empty() || a->reconnects > 0; });
        Check(a->reconnects == 0 && a->session.LastReject() == RejectReason::Banned &&
                  a->session.State() == SessionState::Disconnected,
              "his reconnect is refused (banned)", "\"" + a->disconnectReason + "\"");
    }

    // ---- 10. four players: two guests lose the connection at once, both come back into their own slots ----
    {
        auto host = MakeHost();
        auto a = MakeGuest("Alice");
        auto b = MakeGuest("Bob");
        auto c = MakeGuest("Carol");
        const bool in = Join(*host, *a) && Join(*host, *b, {a.get()}) && Join(*host, *c, {a.get(), b.get()});
        Nodes all{host.get(), a.get(), b.get(), c.get()};
        Pump(all, 2, [&] { return a->session.Players().size() == 4; });
        Check(in && host->session.Players().size() == 4, "4 players: setup");
        const PlayerId aid = a->session.LocalId(), bid = b->session.LocalId(), cid = c->session.LocalId();
        a->Block(true);
        b->Block(true);
        Pump(all, 8, [&] {
            return host->Dropped(aid) && host->Dropped(bid) && c->Dropped(aid) && c->Dropped(bid) &&
                   a->session.IsReconnecting() && b->session.IsReconnecting();
        });
        const auto held = host->session.DroppedPlayers();
        Check(held.size() == 2 && host->session.Players().size() == 2 && c->session.DroppedPlayers().size() == 2 &&
                  c->session.Players().size() == 2,
              "4 players: two held at once, the host and the third guest see both", std::to_string(held.size()));
        // a newcomer while two slots are held: the one free slot is the only one he may get
        auto d = MakeGuest("Dave");
        const bool din = Join(*host, *d, {a.get(), b.get(), c.get()});
        Check(!din && d->session.LastReject() == RejectReason::Full, "4 players: no newcomer while the slots are held",
              "\"" + d->disconnectReason + "\"");
        a->Block(false);
        Pump(all, 8, [&] { return a->reconnects > 0 && c->Resumed(aid); });
        Check(a->reconnects == 1 && a->lastSameSlot && a->session.LocalId() == aid && b->session.IsReconnecting() &&
                  host->session.DroppedPlayers().size() == 1,
              "4 players: the first is back in his slot, the second still held");
        b->Block(false);
        Pump(all, 8, [&] { return b->reconnects > 0 && c->Resumed(bid) && a->Resumed(bid); });
        Check(b->reconnects == 1 && b->lastSameSlot && b->session.LocalId() == bid &&
                  host->session.Players().size() == 4 && host->session.DroppedPlayers().empty(),
              "4 players: the second is back in his slot too");
        Pump(all, 2, [&] { return a->session.Players().size() == 4 && b->session.Players().size() == 4; });
        Check(a->session.Players().size() == 4 && b->session.Players().size() == 4 && c->session.Players().size() == 4 &&
                  Find(a->session, bid) && Find(b->session, aid) && Find(a->session, cid),
              "4 players: everyone sees everyone again");
        // guest-to-guest traffic through the host's relay works again for the two who came back
        const uint8_t one[1] = {1};
        b->appFrom.clear();
        c->appFrom.clear();
        a->session.SendTo(bid, 0x0001, one);
        a->session.Broadcast(0x0001, one);
        Pump(all, 2, [&] { return b->appFrom.size() >= 2 && !c->appFrom.empty(); });
        Check(b->appFrom.size() >= 2 && b->appFrom[0] == aid && !c->appFrom.empty() && c->appFrom[0] == aid,
              "4 players: guest -> guest and broadcasts flow again after both reconnects");
    }

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
