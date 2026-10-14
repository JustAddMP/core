// net_fuzz: throws malformed and random input at the net code for a few seconds and checks that nothing crashes,
// hangs or grows without bound. Deterministic: "-seed N" (default 1) picks the random stream, "-v" prints net logs.
//   1. ByteReader: random buffers, random read sequences.
//   2. Session (host and guest) over a fake in-process Transport: random, truncated and bit-flipped session messages,
//      connects and disconnects; plus a few exact regression checks (Welcome validation, name cleaning).
//   3. UdpTransport over real sockets: garbage datagrams, a hand-made client that completes the cookie handshake and
//      then sends random DATA (acks, units, fragments); the per-Poll event cap and the per-address connection cap.
// Prints PASS/FAIL per check, exits non-zero on any failure.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <set>
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
    if (g_verbose && level >= 1) std::printf("    [%7.3f] %s\n", Seconds() - g_t0, msg);
}

// xorshift64*: deterministic for a given seed.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t Next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t U32() { return static_cast<uint32_t>(Next() >> 32); }
    uint32_t Below(uint32_t n) { return n ? U32() % n : 0; }
    uint8_t Byte() { return static_cast<uint8_t>(Next() >> 56); }
    bool Chance(uint32_t percent) { return Below(100) < percent; }
    std::vector<uint8_t> Bytes(size_t n) {
        std::vector<uint8_t> v(n);
        for (auto& b : v) b = Byte();
        return v;
    }
    std::string Text(size_t maxLen) {  // mostly printable, sometimes control bytes / UTF-8 lead bytes
        std::string text(Below(static_cast<uint32_t>(maxLen) + 1), '\0');
        for (char& c : text) {
            uint32_t k = Below(10);
            c = static_cast<char>(k < 7 ? 0x20 + Below(95) : k < 8 ? Below(0x20) : 0x80 + Below(0x80));
        }
        return text;
    }
};

// Corrupts an encoded message: truncation, bit flips, trailing junk.
void Mutate(Rng& rng, std::vector<uint8_t>& m) {
    if (!m.empty() && rng.Chance(15)) m.resize(rng.Below(static_cast<uint32_t>(m.size())));
    if (!m.empty() && rng.Chance(15))
        for (uint32_t i = 0, n = 1 + rng.Below(4); i < n; ++i)
            m[rng.Below(static_cast<uint32_t>(m.size()))] ^= static_cast<uint8_t>(1u << rng.Below(8));
    if (rng.Chance(5)) {
        auto junk = rng.Bytes(rng.Below(64));
        m.insert(m.end(), junk.begin(), junk.end());
    }
}

// ---- 1. ByteReader ------------------------------------------------------------------------------------------------

void FuzzWire(Rng& rng) {
    const double t = Seconds();
    uint64_t reads = 0;
    bool ok = true;
    for (int iter = 0; iter < 200000; ++iter) {
        std::vector<uint8_t> buf = rng.Bytes(rng.Below(48));
        // Sometimes plant a huge varint length so Str/View/Skip see absurd sizes.
        if (!buf.empty() && rng.Chance(20)) {
            ByteWriter w;
            w.VarU64(rng.Chance(50) ? ~0ull : (1ull << rng.Below(64)));
            auto v = w.Take();
            buf.insert(buf.begin(), v.begin(), v.end());
        }
        ByteReader r(buf.data(), buf.size());
        for (int k = 0, n = 1 + static_cast<int>(rng.Below(12)); k < n; ++k) {
            ++reads;
            switch (rng.Below(14)) {
                case 0: r.U8(); break;
                case 1: r.U16(); break;
                case 2: r.U32(); break;
                case 3: r.U64(); break;
                case 4: r.F32(); break;
                case 5: r.F64(); break;
                case 6: r.VarU32(); break;
                case 7: r.VarU64(); break;
                case 8: r.VarI32(); break;
                case 9: {
                    std::string s = r.Str(rng.Below(64));
                    if (s.size() > 64) ok = false;
                    break;
                }
                case 10: {
                    uint8_t out[64];
                    r.Bytes(out, rng.Below(65));
                    break;
                }
                case 11: {
                    size_t len = rng.Chance(10) ? ~size_t{0} - rng.Below(8) : rng.Below(64);
                    auto v = r.View(len);
                    if (v.size() > buf.size()) ok = false;
                    break;
                }
                case 12: r.Skip(rng.Chance(10) ? ~size_t{0} : rng.Below(64)); break;
                default: {
                    auto rest = r.Rest();
                    if (rest.size() > buf.size()) ok = false;
                    break;
                }
            }
            if (r.Position() > buf.size() || (r.Failed() && r.Remaining() != 0)) ok = false;
        }
    }
    char d[96];
    std::snprintf(d, sizeof(d), "%llu reads in %.2f s", static_cast<unsigned long long>(reads), Seconds() - t);
    Check(ok, "wire: random reads stay in bounds", d);
}

// ---- 2. Session over a fake transport -----------------------------------------------------------------------------

// In-process Transport: the test queues events, Session sends are counted. Peer ids 1..8; a guest's host is peer 77.
class FakeTransport final : public Transport {
public:
    struct Ev {
        EventType type;
        PeerId peer;
        DisconnectReason reason;
        Reliability channel;
        std::vector<uint8_t> data;
    };
    std::deque<Ev> queue;
    std::set<PeerId> up;
    bool hosting = false;
    uint64_t sends = 0, sendBytes = 0, generation = 0;
    size_t maxSend = 0;

    bool Host(uint16_t) override {
        hosting = true;
        return true;
    }
    PeerId Connect(std::string_view) override { return 77; }
    bool Send(PeerId peer, std::span<const uint8_t> d, Reliability rel) override {
        if (!up.count(peer)) return false;
        if (d.size() > (rel == Reliability::Reliable ? kMaxReliableMessage : kMaxUnreliableMessage)) return false;
        ++sends;
        sendBytes += d.size();
        maxSend = std::max(maxSend, d.size());
        return true;
    }
    void Poll(const EventHandler& handler) override {
        std::deque<Ev> evs;
        evs.swap(queue);
        const uint64_t gen = generation;
        for (Ev& e : evs) {
            if (generation != gen) break;  // closed from inside the handler: a real transport stops delivering
            if (e.type == EventType::Message && !up.count(e.peer)) continue;
            Event ev{e.type, e.peer, e.reason, std::span<const uint8_t>(e.data.data(), e.data.size()), e.channel};
            handler(ev);
        }
    }
    void Disconnect(PeerId peer) override { up.erase(peer); }
    void Close() override {
        up.clear();
        queue.clear();
        hosting = false;
        ++generation;
    }
    bool GetStats(PeerId peer, PeerStats& out) const override {
        if (!up.count(peer)) return false;
        out = PeerStats{};
        out.rttMs = 30;
        return true;
    }
    bool IsHosting() const override { return hosting; }
    const char* Name() const override { return "fake"; }
    std::string PeerAddress(PeerId peer) const override { return "10.0.0." + std::to_string(peer % 3); }

    void Connected(PeerId p) {
        up.insert(p);
        queue.push_back({EventType::Connected, p, DisconnectReason::None, Reliability::Reliable, {}});
    }
    void Dropped(PeerId p, DisconnectReason why) {
        up.erase(p);
        queue.push_back({EventType::Disconnected, p, why, Reliability::Reliable, {}});
    }
    void Message(PeerId p, std::vector<uint8_t> d, Reliability ch) {
        queue.push_back({EventType::Message, p, DisconnectReason::None, ch, std::move(d)});
    }
};

constexpr uint32_t kBuild = 42;

// One random session message, valid-looking more often than not.
std::vector<uint8_t> RandomSessionMessage(Rng& rng, bool fromHost) {
    static const uint8_t kTypes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x10};
    ByteWriter w;
    const uint8_t type = rng.Chance(90) ? kTypes[rng.Below(sizeof(kTypes))] : rng.Byte();
    w.U8(type);
    switch (type) {
        case 0x01:  // Hello
            w.U32(rng.Chance(80) ? 0x504F434Du : rng.U32());
            w.U32(rng.Chance(80) ? kProtocolVersion : rng.U32());
            w.U32(rng.Chance(80) ? kBuild : rng.U32());
            w.Str(rng.Text(rng.Chance(10) ? 400 : 40));
            w.Str(rng.Chance(50) ? std::string("visuals/hero/x") : rng.Text(120));
            break;
        case 0x02: {  // Welcome
            const uint8_t n = static_cast<uint8_t>(rng.Below(6));
            w.U8(static_cast<uint8_t>(rng.Below(5)));
            w.U8(4);
            w.U8(rng.Chance(90) ? n : rng.Byte());
            for (uint8_t i = 0; i < n; ++i) {
                w.U8(rng.Chance(80) ? i : rng.Byte());
                w.Str(rng.Text(40));
                w.Str(rng.Text(20));
            }
            break;
        }
        case 0x03:  // Reject
            w.U8(rng.Byte());
            w.Str(rng.Text(300));
            break;
        case 0x04:  // PeerJoined
            w.U8(rng.Chance(80) ? static_cast<uint8_t>(rng.Below(4)) : rng.Byte());
            w.Str(rng.Text(150));
            w.Str(rng.Text(20));
            break;
        case 0x05:  // PeerLeft
            w.U8(static_cast<uint8_t>(rng.Below(5)));
            w.Str(rng.Text(300));
            break;
        case 0x06:
        case 0x07:  // Ping / Pong
            w.U32(rng.U32());
            break;
        case 0x08:  // Chat
            w.U8(rng.Byte());
            w.Str(rng.Text(rng.Chance(10) ? 400 : 100));
            break;
        case 0x09: {  // PingTable
            const uint8_t n = rng.Byte();
            w.U8(n);
            for (uint32_t i = 0, k = rng.Below(n + 1u); i < k; ++i) {
                w.U8(static_cast<uint8_t>(rng.Below(5)));
                w.U16(static_cast<uint16_t>(rng.U32()));
            }
            break;
        }
        case 0x0A: {  // Challenge
            auto c = rng.Bytes(rng.Chance(80) ? 32 : rng.Below(40));
            w.Bytes(c);
            break;
        }
        case 0x0B: {  // Auth
            w.U8(rng.Chance(30) ? 0 : 1);
            auto mac = rng.Bytes(rng.Chance(80) ? 32 : rng.Below(40));
            w.Bytes(mac);
            break;
        }
        case 0x10: {  // App
            w.VarU32(rng.Chance(70) ? 100 + rng.Below(4) : rng.U32());
            w.U8(rng.Byte());
            const uint8_t targets[] = {0, 1, 2, 3, 0xFF, 0xFE, 9};
            w.U8(targets[rng.Below(sizeof(targets))]);
            w.U8(rng.Byte());
            auto p = rng.Bytes(rng.Chance(5) ? 3000 : rng.Below(64));
            w.Bytes(p);
            break;
        }
        default: {
            auto p = rng.Bytes(rng.Below(64));
            w.Bytes(p);
            break;
        }
    }
    (void)fromHost;
    std::vector<uint8_t> m = w.Take();
    Mutate(rng, m);
    return m;
}

void InstallReaders(Session& s, uint64_t& appCalls) {
    for (uint16_t id = 100; id < 104; ++id)
        s.On(id, [&appCalls](PlayerId, ByteReader& r) {
            ++appCalls;
            r.U32();
            r.Str(64);
            r.F32();
            r.Rest();
        });
}

void FuzzSessionHost(Rng& rng, int rounds) {
    const double t0 = Seconds();
    uint64_t msgs = 0, appCalls = 0, maxPlayers = 0;
    bool ok = true;
    for (int round = 0; round < rounds; ++round) {
        FakeTransport t;
        SessionConfig cfg;
        cfg.modBuild = kBuild;
        cfg.playerName = "Host";
        cfg.password = rng.Chance(50) ? "pw" : "";
        cfg.authFailDelayS = 0.01;
        cfg.helloTimeoutS = 0.2;
        Session s(t, cfg);
        s.isInvitedPeer = [&rng](PeerId) { return rng.Chance(20); };
        InstallReaders(s, appCalls);
        if (!s.Host(0)) {
            ok = false;
            break;
        }
        for (int step = 0; step < 4000 && s.IsHost(); ++step) {
            const PeerId p = 1 + rng.Below(8);
            const uint32_t k = rng.Below(100);
            if (k < 4 && !t.up.count(p)) {
                t.Connected(p);
            } else if (k < 6 && t.up.count(p)) {
                t.Dropped(p, static_cast<DisconnectReason>(rng.Below(7)));
            } else {
                ++msgs;
                t.Message(p, RandomSessionMessage(rng, false),
                          rng.Chance(50) ? Reliability::Reliable : Reliability::Unreliable);
            }
            if (rng.Chance(10)) s.Poll();
            if (rng.Chance(2)) s.SetPassword(rng.Chance(50) ? "pw" : "");
            maxPlayers = std::max<uint64_t>(maxPlayers, s.Players().size());
            if (s.Players().size() > static_cast<size_t>(kMaxPlayers)) ok = false;
            for (const PlayerInfo& pl : s.Players())
                if (pl.name.empty() || pl.name.size() > 32) ok = false;
        }
        s.Poll();
        if (rng.Chance(50)) {
            ByteWriter w;
            w.U32(1);
            s.Broadcast(100, w.Span());
        }
        s.Leave();
    }
    char d[160];
    std::snprintf(d, sizeof(d), "%d rounds, %llu messages, %llu app calls, up to %llu players, %.2f s", rounds,
                  static_cast<unsigned long long>(msgs), static_cast<unsigned long long>(appCalls),
                  static_cast<unsigned long long>(maxPlayers), Seconds() - t0);
    Check(ok && maxPlayers > 1 && appCalls > 0, "session host: random messages", d);
}

void FuzzSessionGuest(Rng& rng, int iterations) {
    const double t0 = Seconds();
    uint64_t msgs = 0, joins = 0, fails = 0, appCalls = 0;
    bool ok = true;
    FakeTransport t;
    SessionConfig cfg;
    cfg.modBuild = kBuild;
    cfg.playerName = "Guest";
    Session s(t, cfg);
    InstallReaders(s, appCalls);
    s.onJoined = [&] { ++joins; };
    s.onDisconnected = [&](const std::string&) { ++fails; };
    for (int it = 0; it < iterations; ++it) {
        if (s.State() != SessionState::Connecting && s.State() != SessionState::Connected) {
            s.SetPassword(rng.Chance(50) ? "pw" : "");
            s.Join("host");
            t.Connected(77);
        }
        for (int step = 0; step < 200; ++step) {
            const uint32_t k = rng.Below(1000);
            if (k < 3) {
                t.Dropped(77, static_cast<DisconnectReason>(rng.Below(7)));
            } else {
                ++msgs;
                t.Message(77, RandomSessionMessage(rng, true),
                          rng.Chance(50) ? Reliability::Reliable : Reliability::Unreliable);
            }
        }
        s.Poll();
        if (s.State() == SessionState::Connected) {
            if (s.LocalId() == kHostPlayer || s.LocalId() >= kMaxPlayers || !s.FindPlayer(s.LocalId()) ||
                !s.FindPlayer(kHostPlayer))
                ok = false;
            for (const PlayerInfo& pl : s.Players())
                if (pl.name.empty() || pl.name.size() > 32 || pl.id >= kMaxPlayers) ok = false;
            ByteWriter w;
            w.U32(7);
            s.Broadcast(101, w.Span(), Reliability::Unreliable);
            s.SendChat("hi");
            if (rng.Chance(5)) s.Leave();
        }
    }
    s.Leave();
    char d[160];
    std::snprintf(d, sizeof(d), "%llu messages, %llu joins, %llu disconnects, %llu app calls, %.2f s",
                  static_cast<unsigned long long>(msgs), static_cast<unsigned long long>(joins),
                  static_cast<unsigned long long>(fails), static_cast<unsigned long long>(appCalls), Seconds() - t0);
    Check(ok && joins > 0 && fails > 0, "session guest: random messages", d);
}

// Exact guest-side checks: a Welcome must name us (not the host) and the host; names are cleaned like the host does.
void SessionGuestRegressions() {
    auto welcome = [](int me, const std::vector<std::pair<int, std::string>>& players) {
        ByteWriter w;
        w.U8(0x02);
        w.U8(static_cast<uint8_t>(me));
        w.U8(4);
        w.U8(static_cast<uint8_t>(players.size()));
        for (auto& [id, name] : players) {
            w.U8(static_cast<uint8_t>(id));
            w.Str(name);
            w.Str("");
        }
        return w.Take();
    };
    auto run = [&](const std::vector<uint8_t>& msg, std::string* hostName) {
        FakeTransport t;
        SessionConfig cfg;
        cfg.modBuild = kBuild;
        Session s(t, cfg);
        s.Join("host");
        t.Connected(77);
        s.Poll();  // Hello
        t.Message(77, msg, Reliability::Reliable);
        s.Poll();
        if (hostName && s.FindPlayer(kHostPlayer)) *hostName = s.FindPlayer(kHostPlayer)->name;
        return s.State();
    };
    const bool asHost = run(welcome(0, {{0, "H"}, {1, "G"}}), nullptr) == SessionState::Disconnected;
    const bool notListed = run(welcome(2, {{0, "H"}, {1, "G"}}), nullptr) == SessionState::Disconnected;
    const bool dup = run(welcome(1, {{0, "H"}, {1, "G"}, {1, "X"}}), nullptr) == SessionState::Disconnected;
    const bool badId = run(welcome(1, {{0, "H"}, {1, "G"}, {200, "X"}}), nullptr) == SessionState::Disconnected;
    std::string name;
    const bool good = run(welcome(1, {{0, "\x01" "Bo\x7f" "ss\n"}, {1, "G"}}), &name) == SessionState::Connected;
    std::string longName;
    run(welcome(1, {{0, std::string(31, 'a') + "\xC3\xA9xyz"}, {1, "G"}}), &longName);
    Check(asHost && notListed && dup && badId && good && name == "Bo ss" && longName == std::string(31, 'a'),
          "session guest: Welcome validated, names cleaned", "\"" + name + "\", " + std::to_string(longName.size()) +
                                                                  " bytes");
}

// ---- 3. UdpTransport over real sockets ----------------------------------------------------------------------------

struct Raw {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in to{};
    uint32_t token = 0;
    Raw(uint16_t port) {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(port);
    }
    ~Raw() { closesocket(s); }
    Raw(const Raw&) = delete;
    Raw& operator=(const Raw&) = delete;
    void Send(const std::vector<uint8_t>& d) {
        sendto(s, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
    bool Recv(std::vector<uint8_t>& out) {
        char buf[2048];
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.assign(buf, buf + n);
        return true;
    }
};

std::vector<uint8_t> Header(uint8_t type, uint32_t token) {
    ByteWriter w;
    w.U16(0x434D);
    w.U8(1);
    w.U8(type);
    w.U32(token);
    return w.Take();
}

// Cookie handshake by hand. Returns the packet type the host finally answered (3 ACCEPT, 8 REFUSE, 0 nothing).
uint8_t Handshake(Raw& raw, UdpTransport& host, const EventHandler& h, uint32_t token) {
    raw.token = token;
    uint64_t cookie = 0;
    const double end = Seconds() + 2;
    double lastSend = -1;
    while (Seconds() < end) {
        if (Seconds() - lastSend > 0.1) {
            lastSend = Seconds();
            ByteWriter w;
            for (uint8_t b : Header(1, token)) w.U8(b);
            w.U64(cookie);
            while (w.Size() < 64) w.U8(0);
            raw.Send(w.Take());
        }
        host.Poll(h);
        std::vector<uint8_t> in;
        while (raw.Recv(in)) {
            if (in.size() < 8) continue;
            ByteReader r(in.data(), in.size());
            r.U16();
            r.U8();
            const uint8_t type = r.U8();
            r.U32();
            if (type == 2) {
                cookie = r.U64();
                lastSend = -1;
            } else if (type == 3 || type == 8) {
                return type;
            }
        }
        Sleep(1);
    }
    return 0;
}

std::vector<uint8_t> RandomDatagram(Rng& rng, uint32_t token, uint16_t seqBase) {
    std::vector<uint8_t> d;
    const uint32_t mode = rng.Below(10);
    if (mode == 0) return rng.Bytes(rng.Below(1300));  // garbage
    // Mostly DATA; the other types except DISCONNECT (7), which would just end the connection every few packets.
    static const uint8_t kOther[] = {1, 2, 3, 5, 6, 8, 9, 0};
    d = Header(mode == 1 ? rng.Byte() : rng.Chance(85) ? uint8_t{4} : kOther[rng.Below(sizeof(kOther))],
               rng.Chance(90) ? token : rng.U32());
    ByteWriter w;
    w.U16(rng.Chance(50) ? static_cast<uint16_t>(seqBase + rng.Below(8)) : static_cast<uint16_t>(rng.U32()));
    const uint8_t words = rng.Chance(90) ? static_cast<uint8_t>(rng.Below(9)) : rng.Byte();
    w.U8(words);
    for (uint32_t i = 0, n = rng.Below(words + 1u); i < n; ++i) w.U32(rng.U32());
    while (w.Size() + d.size() < 1150 && !rng.Chance(15)) {
        const uint8_t kind = rng.Chance(90) ? static_cast<uint8_t>(rng.Below(5)) : rng.Byte();
        w.U8(kind);
        if (kind != 0) w.U16(rng.Chance(80) ? static_cast<uint16_t>(seqBase + rng.Below(300)) : static_cast<uint16_t>(rng.U32()));
        uint32_t len = rng.Chance(5) ? rng.U32() : rng.Below(80);
        w.VarU32(len);
        std::vector<uint8_t> body = rng.Bytes(std::min<uint32_t>(len, 80));
        if (kind == 2 && body.size() >= 4 && rng.Chance(70)) {  // fragment first: plausible or absurd total
            uint32_t total = rng.Chance(50) ? rng.Below(4000) : rng.U32();
            std::memcpy(body.data(), &total, 4);
        }
        w.Bytes(body);
    }
    auto b = w.Take();
    d.insert(d.end(), b.begin(), b.end());
    if (d.size() > 1200) d.resize(1200);
    return d;
}

void FuzzUdp(Rng& rng, int loops) {
    UdpTransport host;
    if (!host.Host(0)) {
        Check(false, "udp fuzz: host bind");
        return;
    }
    const uint16_t port = host.BoundPort();
    uint64_t events = 0, messages = 0, rehandshakes = 0;
    size_t biggest = 0;
    bool dropped = false;
    EventHandler h = [&](const Event& e) {
        ++events;
        if (e.type == EventType::Message) {
            ++messages;
            biggest = std::max(biggest, e.data.size());
        }
        if (e.type == EventType::Disconnected) dropped = true;
    };
    Raw raw(port);
    const bool hs = Handshake(raw, host, h, 0x12345679) == 3;
    uint64_t datagrams = 0;
    const double t0 = Seconds();
    uint16_t seqBase = 0;
    for (int loop = 0; loop < loops; ++loop) {
        if (dropped) {  // a random DISCONNECT (or a CONNECT with a new token) ended the connection: start a new one
            dropped = false;
            seqBase = 0;
            ++rehandshakes;
            Handshake(raw, host, h, rng.U32() | 1);
        }
        for (int i = 0; i < 32; ++i, ++datagrams) raw.Send(RandomDatagram(rng, raw.token, seqBase));
        if (rng.Chance(5)) seqBase = static_cast<uint16_t>(seqBase + rng.Below(64));
        host.Poll(h);
        std::vector<uint8_t> in;
        while (raw.Recv(in)) {
        }
        if (rng.Chance(3)) {  // keep the fake client alive: answer like a peer would (any DATA refreshes it)
            ByteWriter w;
            for (uint8_t b : Header(4, raw.token)) w.U8(b);
            w.U16(0);
            w.U8(0);
            raw.Send(w.Take());
        }
    }
    host.Poll(h);

    // The host still works for a well-behaved client after all that.
    UdpTransport client;
    PeerId hp = client.Connect("127.0.0.1:" + std::to_string(port));
    bool connected = false;
    size_t got = 0;
    PeerId guestOnHost = 0;
    const std::vector<uint8_t> payload(5000, 0xAB);
    const double end2 = Seconds() + 3;
    bool sent = false;
    while (Seconds() < end2 && got != payload.size()) {
        client.Poll([&](const Event& e) {
            if (e.type == EventType::Connected && e.peer == hp) connected = true;
        });
        host.Poll([&](const Event& e) {
            if (e.type == EventType::Connected) guestOnHost = e.peer;
            if (e.type == EventType::Message && e.peer == guestOnHost && e.data.size() == payload.size()) got = e.data.size();
        });
        if (connected && guestOnHost && !sent) sent = client.Send(hp, payload, Reliability::Reliable);
        Sleep(1);
    }
    char d[200];
    std::snprintf(d, sizeof(d), "%llu datagrams (%.2f s), %llu events, %llu reconnects, biggest %zu; 5000 B msg %s",
                  static_cast<unsigned long long>(datagrams), Seconds() - t0, static_cast<unsigned long long>(events),
                  static_cast<unsigned long long>(rehandshakes), biggest, got == payload.size() ? "arrived" : "LOST");
    Check(hs && biggest <= kMaxReliableMessage && got == payload.size(), "udp: random datagrams from a peer", d);
}

// Regression: one 1200-byte datagram can carry ~590 empty units; a Poll must not queue all of a flood at once.
void UdpEventCap() {
    UdpTransport host;
    host.Host(0);
    uint64_t messages = 0;
    EventHandler h = [&](const Event& e) {
        if (e.type == EventType::Message) ++messages;
    };
    Raw raw(host.BoundPort());
    const bool hs = Handshake(raw, host, h, 0x0BADF00D) == 3;
    const int kDatagrams = 200;
    size_t perDatagram = 0;
    for (int i = 0; i < kDatagrams; ++i) {
        ByteWriter w;
        for (uint8_t b : Header(4, raw.token)) w.U8(b);
        w.U16(0);
        w.U8(0);
        perDatagram = 0;
        while (w.Size() + 2 <= 1200) {
            w.U8(0);   // unreliable
            w.U8(0);   // empty
            ++perDatagram;
        }
        raw.Send(w.Take());
        if (i % 50 == 49) Sleep(5);  // let the loopback drain into the host's socket buffer
    }
    Sleep(50);
    messages = 0;
    host.Poll(h);
    const uint64_t first = messages;
    for (int i = 0; i < 200 && messages < perDatagram * kDatagrams; ++i) {
        host.Poll(h);
        Sleep(1);
    }
    char d[160];
    std::snprintf(d, sizeof(d), "first Poll %llu events, total %llu of %zu", static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(messages), perDatagram * kDatagrams);
    Check(hs && first > 0 && first <= 8192 + perDatagram && messages == perDatagram * kDatagrams,
          "udp: events per Poll are capped, none lost", d);
}

// Regression: one address cannot take every incoming slot.
void UdpPerAddressCap() {
    UdpTransport host;
    host.Host(0);
    EventHandler h = [](const Event&) {};
    std::vector<std::unique_ptr<Raw>> raws;
    std::string results;
    int accepted = 0, refused = 0;
    for (int i = 0; i < 6; ++i) {
        raws.push_back(std::make_unique<Raw>(host.BoundPort()));
        const uint8_t r = Handshake(*raws.back(), host, h, 0x1000u + static_cast<uint32_t>(i));
        results += r == 3 ? "A" : r == 8 ? "R" : "-";
        accepted += r == 3;
        refused += r == 8;
    }
    const int cap = UdpOptions{}.maxPeersPerAddress;
    Check(accepted == cap && refused == 6 - cap, "udp: per-address connection cap", results);
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
        if (!std::strcmp(argv[i], "-seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    }
    g_t0 = Seconds();
    SetLogSink(PrintLog);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::printf("      seed %llu\n", static_cast<unsigned long long>(seed));

    Rng rng(seed);
    FuzzWire(rng);
    FuzzSessionHost(rng, 400);
    FuzzSessionGuest(rng, 3000);
    SessionGuestRegressions();
    FuzzUdp(rng, 1000);
    UdpEventCap();
    UdpPerAddressCap();

    WSACleanup();
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
