// net: UDP transport. See docs/protocol.md "UDP transport" for the wire format.
#include "net/udp_transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/net_internal.h"
#include "net/wire.h"

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace mc::net {
using namespace detail;

namespace {

constexpr uint16_t kMagic = 0x434D;  // bytes 'M','C' on the wire
constexpr uint8_t kWireVersion = 1;
constexpr size_t kMtu = 1200;
constexpr size_t kHeaderSize = 8;
constexpr size_t kFragPayload = kMaxUnreliableMessage;  // bytes of user data per reliable unit
constexpr uint16_t kWindow = 256;                       // reliable units in flight / receive window
constexpr int kAckWords = kWindow / 32;
constexpr int kNewUnitsPerFlush = 128;
constexpr size_t kMaxQueuedUnits = 8192;
constexpr double kConnectResend = 0.25;
constexpr double kCookieBucketS = 5.0;
constexpr size_t kConnectPacketSize = 64;  // padded so a CHALLENGE is never larger than its CONNECT
// Events (messages) queued per Poll before Receive stops reading the socket until the next Poll. A 1200-byte DATA
// datagram can carry ~590 empty units, so without a cap one connected peer could make a single Poll queue millions of
// events; the rest stays in the socket buffer.
constexpr size_t kMaxEventsPerPoll = 8192;

enum PacketType : uint8_t {
    kConnect = 1,
    kChallenge = 2,
    kAccept = 3,
    kData = 4,
    kPing = 5,
    kPong = 6,
    kDisconnect = 7,
    kRefuse = 8,
    kAlive = 9,  // from the keepalive thread while the sender's Poll() is stalled: only refreshes the timeout
};
// A clean disconnect is unreliable (the connection is gone right after): send it several times so the other side
// learns at once even on a lossy link instead of after the timeout.
constexpr int kDisconnectCopies = 4;

enum UnitKind : uint8_t {
    kUnreliable = 0,
    kReliableWhole = 1,
    kFragFirst = 2,  // payload starts with u32 total length
    kFragMiddle = 3,
    kFragLast = 4,
};
// Laned reliable unit (docs/protocol.md "Lanes"): the kind byte has this bit and the seq is followed by u8 lane,
// u16 laneSeq (its place in its lane), u16 barrier (lane-0 units queued before it). A connection sends the old
// format (no bit: lane 0, delivered in global order) until it first uses a lane > 0, then every reliable unit is
// laned. A laned unit is delivered once its lane is complete up to it; one on a lane > 0 also waits for the lane-0
// units queued before it (session control stays ahead of app traffic), lane 0 waits for nobody else.
constexpr uint8_t kKindLaned = 0x80;
constexpr uint8_t kKindMask = 0x07;

enum RefuseReason : uint8_t { kRefuseNotHosting = 1, kRefuseFull = 2 };

inline bool SeqLess(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) < 0; }

struct WsaGuard {
    bool ok = false;
    bool Start() {
        if (ok) return true;
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
        return ok;
    }
    void Stop() {
        if (ok) WSACleanup();
        ok = false;
    }
};

uint64_t AddrKey(const sockaddr_in& a) {
    return (static_cast<uint64_t>(a.sin_addr.s_addr) << 16) | a.sin_port;
}

std::string AddrStr(const sockaddr_in& a) {
    char ip[32] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

struct OutUnit {
    std::vector<uint8_t> data;
    uint8_t kind = kReliableWhole;
    uint8_t lane = 0;
    bool laned = false;    // sent in the laned format
    uint16_t laneSeq = 0;  // position in its lane
    uint16_t barrier = 0;  // lane > 0: lane-0 units queued before it
    double lastSent = 0;
    uint64_t xmit = 0;     // datagram number it was last sent in (fast retransmit: a later one was acknowledged)
    uint16_t sendCount = 0;
    bool acked = false;
};

struct InSlot {
    bool has = false;        // received (acknowledged); kept until recvExpected passes it
    bool delivered = false;  // handed on (a lane unit can be delivered before earlier units of other lanes arrive)
    uint8_t kind = 0;
    uint8_t lane = 0;
    bool laned = false;      // false: old format (lane 0, delivered only in global order)
    uint16_t laneSeq = 0;
    uint16_t barrier = 0;
    std::vector<uint8_t> data;
};

struct Assembly {  // fragmented reliable message being put together (one per lane)
    std::vector<uint8_t> data;
    uint32_t total = 0;
    bool active = false;
};

enum class PeerState { Connecting, Connected };

struct Peer {
    PeerId id = 0;
    sockaddr_in addr{};
    uint32_t token = 0;  // connection nonce chosen by the connecting side
    PeerState state = PeerState::Connecting;
    bool outgoing = false;  // we initiated it
    bool established = false;  // incoming: the session finished its handshake (Transport::MarkEstablished)
    uint64_t cookie = 0;    // outgoing: challenge cookie received
    double created = 0, lastRecv = 0, lastConnectSend = 0, lastPing = 0;

    // reliable send
    std::deque<OutUnit> rq;
    uint16_t sendBase = 0;  // seq of rq[0]
    // reliable receive
    uint16_t recvExpected = 0;
    std::array<InSlot, kWindow> slots;
    std::array<Assembly, kLanes> assembly;
    bool ackDirty = false;
    // lanes (docs/protocol.md "Lanes")
    std::array<uint16_t, kLanes> laneSendSeq{};  // next laneSeq to send per lane
    std::array<uint16_t, kLanes> laneNext{};     // next laneSeq to deliver per lane
    int lanePending = 0;                          // received laned units not delivered yet
    bool lanesUsed = false;                       // we queued a lane > 0 unit: everything is laned from then on
    // fast retransmit (RACK-like): the newest datagram that carried a unit the peer acknowledged
    uint64_t xmitCounter = 0;  // datagrams with reliable units sent
    uint64_t rackXmit = 0;
    double rackRtt = 0;        // send -> ack time of that unit
    // unreliable send queue
    std::vector<std::vector<uint8_t>> uq;

    // rtt
    double srtt = -1, rttvar = 0;
    // ping loss window
    uint32_t pingId = 0;
    std::array<double, 64> pingSent{};
    std::array<bool, 64> pongGot{};

    PeerStats stats;

    double Rto() const {
        if (srtt < 0) return 0.2;
        // The round trip includes the peer's frame (it acknowledges from its Poll), so this stays above it. Floor
        // 30 ms (was 50 ms before core 0.12): most losses are now repaired by the fast retransmit below anyway.
        return std::clamp(srtt + 4 * rttvar + 0.01, 0.03, 1.0);
    }
    // A unit sent before the newest acknowledged one and still unacknowledged after its round trip plus a small
    // reorder allowance is lost: send it again now instead of waiting for the timeout.
    bool FastLost(const OutUnit& u, double now) const {
        if (u.xmit == 0 || u.xmit >= rackXmit) return false;
        const double reorder = std::clamp(srtt < 0 ? 0.004 : srtt / 4, 0.002, 0.02);
        return now - u.lastSent >= rackRtt + reorder;
    }
    void RttSample(double s) {
        if (s < 0 || s > 10) return;
        if (srtt < 0) {
            srtt = s;
            rttvar = s / 2;
        } else {
            rttvar = 0.75 * rttvar + 0.25 * std::fabs(srtt - s);
            srtt = 0.875 * srtt + 0.125 * s;
        }
    }
};

struct PendingEvent {
    EventType type;
    PeerId peer;
    DisconnectReason reason;
    Reliability channel;
    std::vector<uint8_t> data;
};

struct Delayed {
    double release;
    sockaddr_in to;
    std::vector<uint8_t> bytes;
};

}  // namespace

struct UdpTransport::Impl {
    UdpOptions opt;
    WsaGuard wsa;
    SOCKET sock = INVALID_SOCKET;
    bool hosting = false;
    uint16_t boundPort = 0;
    uint8_t secret[16] = {};
    PeerId nextId = 1;
    std::unordered_map<PeerId, Peer> peers;
    uint64_t evictions = 0;
    std::unordered_map<uint64_t, PeerId> byAddr;
    std::vector<PendingEvent> events;
    std::unordered_set<PeerId> closedLocally;  // peers Disconnect()ed while events for them are pending
    std::vector<Delayed> delayed;
    uint64_t simState = 1;
    uint8_t rxbuf[2048] = {};

    // ---- keepalive thread (UdpOptions::keepaliveThreadS) ----
    // It only reads kaPeers (a copy Poll() refreshes under kaMu) and the socket, and only sends.
    struct KaPeer {
        sockaddr_in addr;
        uint32_t token;
    };
    std::thread kaThread;
    std::mutex kaMu;
    std::condition_variable kaCv;
    bool kaStop = false;
    std::vector<KaPeer> kaPeers;
    std::atomic<double> lastPoll{0};
    std::atomic<uint64_t> kaSent{0};
    std::atomic<double> kaInterval{0.25}, kaMaxStall{60};  // copies of the options (SetOptions may change them)

    void KaStart() {
        kaInterval = opt.keepaliveThreadS;
        kaMaxStall = opt.keepaliveMaxStallS;
        if (kaThread.joinable() || opt.keepaliveThreadS <= 0 || sock == INVALID_SOCKET) return;
        {
            std::lock_guard lock(kaMu);
            kaStop = false;
        }
        lastPoll = Now();
        kaThread = std::thread([this] { KaLoop(); });
    }
    void KaStopThread() {
        if (!kaThread.joinable()) return;
        {
            std::lock_guard lock(kaMu);
            kaStop = true;
        }
        kaCv.notify_all();
        kaThread.join();
    }
    void KaSnapshot() {  // Poll() thread
        lastPoll = Now();
        std::lock_guard lock(kaMu);
        kaPeers.clear();
        for (auto& kv : peers)
            if (kv.second.state == PeerState::Connected) kaPeers.push_back({kv.second.addr, kv.second.token});
    }
    void KaLoop() {
        bool stalled = false;
        std::unique_lock lock(kaMu);
        while (!kaStop) {
            const double interval = std::max(0.05, kaInterval.load()), maxStall = kaMaxStall.load();
            kaCv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(interval * 1000)));
            if (kaStop) break;
            if (kaInterval.load() <= 0) {  // switched off
                stalled = false;
                continue;
            }
            const double stall = Now() - lastPoll.load();
            if (stall < 2 * interval) {
                if (stalled) Log(kInfo, "udp: Poll() is back after a stall");
                stalled = false;
                continue;
            }
            if (stall > maxStall) {
                if (stalled) Log(kWarn, "udp: Poll() stalled for %.0f s: no more keepalives (the peers time out)", stall);
                stalled = false;
                continue;
            }
            if (!stalled) {
                stalled = true;
                Log(kInfo, "udp: Poll() stalled: the keepalive thread keeps %zu peer(s) up", kaPeers.size());
            }
            for (const KaPeer& kp : kaPeers) {
                uint8_t pkt[kHeaderSize];
                ByteWriter w(kHeaderSize);
                Header(w, kAlive, kp.token);
                std::memcpy(pkt, w.Buffer().data(), kHeaderSize);
                sendto(sock, reinterpret_cast<const char*>(pkt), static_cast<int>(kHeaderSize), 0,
                       reinterpret_cast<const sockaddr*>(&kp.addr), sizeof(kp.addr));
                ++kaSent;
            }
        }
    }

    // ---- simulation helpers ----
    float SimRand() {  // xorshift64*, [0,1)
        simState ^= simState >> 12;
        simState ^= simState << 25;
        simState ^= simState >> 27;
        uint64_t r = simState * 0x2545F4914F6CDD1Dull;
        return static_cast<float>((r >> 40) & 0xFFFFFF) / 16777216.0f;
    }
    bool SimActive() const {
        return opt.simLoss > 0 || opt.simLatencyMs > 0 || opt.simJitterMs > 0 || opt.simDuplicate > 0;
    }

    // ---- socket ----
    bool OpenSocket(uint16_t port) {
        if (sock != INVALID_SOCKET) return true;
        if (!wsa.Start()) {
            Log(kError, "udp: WSAStartup failed");
            return false;
        }
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            Log(kError, "udp: socket() failed: %d", WSAGetLastError());
            return false;
        }
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        // Do not report ICMP port-unreachable as WSAECONNRESET on recvfrom.
        BOOL connreset = FALSE;
        DWORD ret = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &connreset, sizeof(connreset), nullptr, 0, &ret, nullptr, nullptr);
        int buf = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            Log(kError, "udp: bind(%u) failed: %d", port, WSAGetLastError());
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }
        int len = sizeof(a);
        getsockname(sock, reinterpret_cast<sockaddr*>(&a), &len);
        boundPort = ntohs(a.sin_port);
        RandomBytes(secret, sizeof(secret));
        simState = opt.simSeed ? opt.simSeed : 1;
        return true;
    }

    void RawSend(const sockaddr_in& to, const uint8_t* p, size_t n) {
        if (opt.testOnSend) opt.testOnSend(std::span<const uint8_t>(p, n));
        sendto(sock, reinterpret_cast<const char*>(p), static_cast<int>(n), 0, reinterpret_cast<const sockaddr*>(&to),
               sizeof(to));
    }

    void SendDatagram(const sockaddr_in& to, const std::vector<uint8_t>& bytes, Peer* p) {
        if (sock == INVALID_SOCKET) return;
        if (p) {
            p->stats.bytesSent += bytes.size();
            p->stats.packetsSent++;
        }
        if (opt.testDrop && opt.testDrop(std::span<const uint8_t>(bytes.data(), bytes.size()))) return;
        if (!SimActive()) {
            RawSend(to, bytes.data(), bytes.size());
            return;
        }
        if (SimRand() < opt.simLoss) return;
        int copies = SimRand() < opt.simDuplicate ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            double d = opt.simLatencyMs + (SimRand() * 2.0f - 1.0f) * opt.simJitterMs;
            if (d < 0) d = 0;
            delayed.push_back({Now() + d / 1000.0, to, bytes});
        }
    }

    void ReleaseDelayed() {
        if (delayed.empty() || sock == INVALID_SOCKET) return;
        double now = Now();
        // Send in release-time order so jitter (not insertion order) decides reordering.
        std::stable_sort(delayed.begin(), delayed.end(),
                         [](const Delayed& a, const Delayed& b) { return a.release < b.release; });
        size_t i = 0;
        for (; i < delayed.size() && delayed[i].release <= now; ++i)
            RawSend(delayed[i].to, delayed[i].bytes.data(), delayed[i].bytes.size());
        delayed.erase(delayed.begin(), delayed.begin() + static_cast<ptrdiff_t>(i));
    }

    static void Header(ByteWriter& w, uint8_t type, uint32_t token) {
        w.U16(kMagic);
        w.U8(kWireVersion);
        w.U8(type);
        w.U32(token);
    }

    template <class F>
    void SendControl(Peer& p, uint8_t type, F&& body) {
        ByteWriter w(64);
        Header(w, type, p.token);
        body(w);
        SendDatagram(p.addr, w.Buffer(), &p);
    }
    void SendControl(Peer& p, uint8_t type) {
        SendControl(p, type, [](ByteWriter&) {});
    }

    uint64_t Cookie(const sockaddr_in& a, uint32_t nonce, int64_t bucket) const {
        uint8_t d[4 + 2 + 4 + 8];
        std::memcpy(d, &a.sin_addr.s_addr, 4);
        std::memcpy(d + 4, &a.sin_port, 2);
        std::memcpy(d + 6, &nonce, 4);
        std::memcpy(d + 10, &bucket, 8);
        uint64_t c = SipHash24(secret, d, sizeof(d));
        return c ? c : 1;
    }

    // ---- peers ----
    Peer* Find(PeerId id) {
        auto it = peers.find(id);
        return it == peers.end() ? nullptr : &it->second;
    }
    Peer* FindByAddr(const sockaddr_in& a) {
        auto it = byAddr.find(AddrKey(a));
        return it == byAddr.end() ? nullptr : Find(it->second);
    }
    Peer& NewPeer(const sockaddr_in& a, uint32_t token, bool outgoing) {
        PeerId id = nextId++;
        Peer& p = peers[id];
        p.id = id;
        p.addr = a;
        p.token = token;
        p.outgoing = outgoing;
        p.created = p.lastRecv = Now();
        byAddr[AddrKey(a)] = id;
        return p;
    }
    void RemovePeer(PeerId id) {
        auto it = peers.find(id);
        if (it == peers.end()) return;
        auto ai = byAddr.find(AddrKey(it->second.addr));
        if (ai != byAddr.end() && ai->second == id) byAddr.erase(ai);
        peers.erase(it);
    }
    void Emit(EventType t, PeerId id, DisconnectReason r = DisconnectReason::None, std::vector<uint8_t> data = {},
              Reliability ch = Reliability::Reliable) {
        events.push_back({t, id, r, ch, std::move(data)});
    }
    void Drop(Peer& p, DisconnectReason r) {
        Log(kInfo, "udp: peer %llu (%s) disconnected: %s", static_cast<unsigned long long>(p.id),
            AddrStr(p.addr).c_str(), ToString(r));
        PeerId id = p.id;
        // Outgoing attempts always get a Disconnected (ConnectFailed/Refused) so the caller learns the outcome.
        bool report = p.state == PeerState::Connected || p.outgoing;
        RemovePeer(id);
        if (report) Emit(EventType::Disconnected, id, r);
    }
    void MarkConnected(Peer& p) {
        if (p.state == PeerState::Connected) return;
        p.state = PeerState::Connected;
        p.lastPing = 0;
        Log(kInfo, "udp: peer %llu (%s) connected", static_cast<unsigned long long>(p.id), AddrStr(p.addr).c_str());
        Emit(EventType::Connected, p.id);
    }

    // ---- receive ----
    void Receive() {
        if (sock == INVALID_SOCKET) return;
        for (int guard = 0; guard < 4096 && events.size() < kMaxEventsPerPoll; ++guard) {
            sockaddr_in from{};
            int fl = sizeof(from);
            int n = recvfrom(sock, reinterpret_cast<char*>(rxbuf), sizeof(rxbuf), 0, reinterpret_cast<sockaddr*>(&from),
                             &fl);
            if (n < 0) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) break;
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;
                Log(kWarn, "udp: recvfrom error %d", e);
                break;
            }
            if (static_cast<size_t>(n) < kHeaderSize || static_cast<size_t>(n) > kMtu) continue;
            OnDatagram(from, rxbuf, static_cast<size_t>(n));
        }
    }

    void OnDatagram(const sockaddr_in& from, const uint8_t* data, size_t len) {
        ByteReader r(data, len);
        if (r.U16() != kMagic || r.U8() != kWireVersion) return;
        uint8_t type = r.U8();
        uint32_t token = r.U32();

        if (type == kConnect) {
            OnConnect(from, token, r);
            return;
        }
        Peer* p = FindByAddr(from);
        if (!p || p->token != token) return;  // unknown or stale/spoofed
        p->stats.bytesReceived += len;
        p->stats.packetsReceived++;

        switch (type) {
            case kChallenge:
                if (p->outgoing && p->state == PeerState::Connecting) {
                    p->cookie = r.U64();
                    if (r.Ok()) SendConnect(*p);
                }
                return;
            case kAccept:
                p->lastRecv = Now();
                if (p->outgoing) MarkConnected(*p);
                return;
            case kRefuse: {
                uint8_t why = r.U8();
                if (p->outgoing && p->state == PeerState::Connecting) {
                    Log(kWarn, "udp: %s refused connection (%s)", AddrStr(from).c_str(),
                        why == kRefuseFull ? "full" : "not hosting");
                    Drop(*p, DisconnectReason::Refused);
                }
                return;
            }
            case kDisconnect:
                if (p->state == PeerState::Connected) Drop(*p, DisconnectReason::Remote);
                return;
            default:
                break;
        }
        // Everything below implies the connection is up.
        p->lastRecv = Now();
        if (p->outgoing && p->state == PeerState::Connecting) MarkConnected(*p);  // ACCEPT was lost
        if (p->state != PeerState::Connected) return;
        switch (type) {
            case kPing: {
                uint32_t id = r.U32(), t = r.U32();
                if (r.Ok())
                    SendControl(*p, kPong, [&](ByteWriter& w) {
                        w.U32(id);
                        w.U32(t);
                    });
                break;
            }
            case kPong: {
                uint32_t id = r.U32();
                r.U32();
                if (!r.Ok()) break;
                if (p->pingId - id < 64 && p->pingSent[id % 64] > 0 && !p->pongGot[id % 64]) {
                    p->pongGot[id % 64] = true;
                    p->RttSample(Now() - p->pingSent[id % 64]);
                }
                break;
            }
            case kData:
                OnData(*p, r);
                break;
            default:
                break;
        }
    }

    void OnConnect(const sockaddr_in& from, uint32_t nonce, ByteReader& r) {
        uint64_t cookie = r.U64();
        if (!r.Ok()) return;
        auto refuse = [&](uint8_t why) {
            ByteWriter w(16);
            Header(w, kRefuse, nonce);
            w.U8(why);
            SendDatagram(from, w.Buffer(), nullptr);
        };
        if (!hosting) {
            refuse(kRefuseNotHosting);
            return;
        }
        int64_t bucket = static_cast<int64_t>(Now() / kCookieBucketS);
        bool valid = cookie != 0 && (cookie == Cookie(from, nonce, bucket) || cookie == Cookie(from, nonce, bucket - 1));
        if (!valid) {
            // Stateless challenge: nothing is allocated until the client proves it receives at its source address.
            ByteWriter w(16);
            Header(w, kChallenge, nonce);
            w.U64(Cookie(from, nonce, bucket));
            SendDatagram(from, w.Buffer(), nullptr);
            return;
        }
        Peer* existing = FindByAddr(from);
        if (existing) {
            if (existing->token == nonce) {
                SendControl(*existing, kAccept);  // our ACCEPT was lost
                return;
            }
            Drop(*existing, DisconnectReason::Remote);  // same address reconnected with a new session
        }
        // Limits: all incoming connections (maxPeers) and those from one IPv4 address (maxPeersPerAddress; any source
        // port completes the cookie handshake). At a limit, the oldest connection that has not completed the
        // session handshake makes room, so connections that never send a valid Hello cannot hold every slot.
        // Established players are never evicted; with only those left, the new one is refused.
        for (;;) {
            size_t incoming = 0, fromAddr = 0;
            for (auto& kv : peers) {
                if (kv.second.outgoing) continue;
                ++incoming;
                if (kv.second.addr.sin_addr.s_addr == from.sin_addr.s_addr) ++fromAddr;
            }
            const bool addrFull = static_cast<int>(fromAddr) >= opt.maxPeersPerAddress;
            if (static_cast<int>(incoming) < opt.maxPeers && !addrFull) break;
            Peer* victim = nullptr;
            for (auto& kv : peers) {
                Peer& c = kv.second;
                if (c.outgoing || c.established) continue;
                if (addrFull && c.addr.sin_addr.s_addr != from.sin_addr.s_addr) continue;
                if (!victim || c.created < victim->created || (c.created == victim->created && c.id < victim->id))
                    victim = &c;
            }
            if (!victim) {
                refuse(kRefuseFull);
                return;
            }
            Log(kInfo, "udp: full; evicting peer %llu (%s), no session handshake after %.1f s",
                static_cast<unsigned long long>(victim->id), AddrStr(victim->addr).c_str(), Now() - victim->created);
            ++evictions;
            SendControl(*victim, kDisconnect);
            SendControl(*victim, kDisconnect);
            Drop(*victim, DisconnectReason::Refused);
        }
        Peer& p = NewPeer(from, nonce, false);
        MarkConnected(p);
        SendControl(p, kAccept);
    }

    void SendConnect(Peer& p) {
        ByteWriter w(kConnectPacketSize);
        Header(w, kConnect, p.token);
        w.U64(p.cookie);
        while (w.Size() < kConnectPacketSize) w.U8(0);
        p.lastConnectSend = Now();
        SendDatagram(p.addr, w.Buffer(), &p);
    }

    void OnData(Peer& p, ByteReader& r) {
        // ack section
        uint16_t cum = r.U16();
        uint8_t nWords = r.U8();
        if (!r.Ok() || nWords > kAckWords) return;
        uint32_t words[kAckWords] = {};
        for (int i = 0; i < nWords; ++i) words[i] = r.U32();
        if (!r.Ok()) return;
        ProcessAck(p, cum, nWords, words);

        // units
        while (r.Ok() && !r.AtEnd()) {
            const uint8_t raw = r.U8();
            const uint8_t kind = raw & kKindMask;
            uint16_t seq = 0, laneSeq = 0, barrier = 0;
            uint8_t lane = 0;
            if (kind != kUnreliable) seq = r.U16();
            if (raw & kKindLaned) {
                lane = r.U8();
                laneSeq = r.U16();
                barrier = r.U16();
            }
            uint32_t len = r.VarU32();
            auto body = r.View(len);
            if (!r.Ok() || kind > kFragLast || (raw & ~(kKindMask | kKindLaned)) ||
                ((raw & kKindLaned) && (kind == kUnreliable || lane >= kLanes)))
                break;
            if (kind == kUnreliable) {
                Emit(EventType::Message, p.id, DisconnectReason::None, std::vector<uint8_t>(body.begin(), body.end()),
                     Reliability::Unreliable);
                continue;
            }
            p.ackDirty = true;
            if (SeqLess(seq, p.recvExpected)) continue;  // duplicate of something delivered
            uint16_t off = static_cast<uint16_t>(seq - p.recvExpected);
            if (off >= kWindow) continue;  // outside receive window
            InSlot& s = p.slots[seq % kWindow];
            if (s.has) continue;
            s.has = true;
            s.delivered = false;
            s.kind = kind;
            s.lane = lane;
            s.laned = (raw & kKindLaned) != 0;
            s.laneSeq = laneSeq;
            s.barrier = barrier;
            s.data.assign(body.begin(), body.end());
            if (s.laned) ++p.lanePending;
        }
        DeliverInOrder(p);
    }

    // Lane 0 units (and every unit once all units before it arrived) in global order; units of lanes > 0 as soon as
    // their lane is complete up to them, even while an earlier unit of another lane is missing.
    void DeliverInOrder(Peer& p) {
        for (;;) {
            InSlot& s = p.slots[p.recvExpected % kWindow];
            if (!s.has) break;
            if (!s.delivered) Deliver(p, s);  // everything before it is here: its lane is complete up to it
            s.has = false;
            s.delivered = false;
            p.recvExpected++;
        }
        bool progress = p.lanePending > 0;
        while (progress) {
            progress = false;
            for (uint16_t k = 1; k < kWindow && p.lanePending > 0; ++k) {
                InSlot& s = p.slots[static_cast<uint16_t>(p.recvExpected + k) % kWindow];
                if (!s.has || s.delivered || !s.laned || s.laneSeq != p.laneNext[s.lane] ||
                    (s.lane && SeqLess(p.laneNext[0], s.barrier)))
                    continue;
                Deliver(p, s);
                progress = true;
            }
        }
    }

    void Deliver(Peer& p, InSlot& s) {
        s.delivered = true;
        if (s.laned) {
            --p.lanePending;
            if (s.laneSeq != p.laneNext[s.lane])  // cannot happen with a sane sender; keep the lane moving
                Log(kWarn, "udp: peer %llu lane %u out of order (%u, expected %u)",
                    static_cast<unsigned long long>(p.id), s.lane, s.laneSeq, p.laneNext[s.lane]);
            p.laneNext[s.lane] = static_cast<uint16_t>(s.laneSeq + 1);
        } else {
            ++p.laneNext[0];  // old format: lane 0
        }
        std::vector<uint8_t> data = std::move(s.data);
        s.data = {};
        Assembly& a = p.assembly[s.lane];
        switch (s.kind) {
            case kReliableWhole:
                Emit(EventType::Message, p.id, DisconnectReason::None, std::move(data), Reliability::Reliable);
                break;
            case kFragFirst: {
                ByteReader fr(data.data(), data.size());
                uint32_t total = fr.U32();
                if (!fr.Ok() || total > kMaxReliableMessage) {
                    Log(kWarn, "udp: peer %llu sent oversized message (%u)", static_cast<unsigned long long>(p.id),
                        total);
                    a.active = false;
                    break;
                }
                a.active = true;
                a.total = total;
                a.data.clear();
                a.data.reserve(total);
                auto rest = fr.Rest();
                a.data.insert(a.data.end(), rest.begin(), rest.end());
                break;
            }
            case kFragMiddle:
            case kFragLast:
                if (!a.active) break;
                if (a.data.size() + data.size() > a.total) {
                    a.active = false;
                    break;
                }
                a.data.insert(a.data.end(), data.begin(), data.end());
                if (s.kind == kFragLast) {
                    a.active = false;
                    if (a.data.size() == a.total)
                        Emit(EventType::Message, p.id, DisconnectReason::None, std::move(a.data),
                             Reliability::Reliable);
                    a.data = {};
                }
                break;
            default:
                break;
        }
    }

    void ProcessAck(Peer& p, uint16_t cum, uint8_t nWords, const uint32_t* words) {
        double now = Now();
        auto ackIdx = [&](size_t i) {
            OutUnit& u = p.rq[i];
            if (u.acked || u.sendCount == 0) return;
            u.acked = true;
            if (u.sendCount == 1) p.RttSample(now - u.lastSent);  // Karn: only unambiguous samples
            if (u.sendCount == 1 && u.xmit > p.rackXmit) {  // a resent unit's ack may be for its first copy
                p.rackXmit = u.xmit;
                p.rackRtt = now - u.lastSent;
            }
        };
        uint16_t diff = static_cast<uint16_t>(cum - p.sendBase);
        if (diff <= p.rq.size())
            for (size_t i = 0; i < diff; ++i) ackIdx(i);
        for (int w = 0; w < nWords; ++w) {
            if (!words[w]) continue;
            for (int b = 0; b < 32; ++b) {
                if (!(words[w] & (1u << b))) continue;
                uint16_t seq = static_cast<uint16_t>(cum + 1 + w * 32 + b);
                uint16_t idx = static_cast<uint16_t>(seq - p.sendBase);
                if (idx < p.rq.size()) ackIdx(idx);
            }
        }
        while (!p.rq.empty() && p.rq.front().acked) {
            p.rq.pop_front();
            p.sendBase++;
        }
    }

    // ---- send ----
    void WriteAck(ByteWriter& w, Peer& p) {
        w.U16(p.recvExpected);
        uint32_t words[kAckWords] = {};
        int n = 0;
        for (int k = 0; k < kWindow - 1; ++k) {
            uint16_t seq = static_cast<uint16_t>(p.recvExpected + 1 + k);
            if (p.slots[seq % kWindow].has) {
                words[k / 32] |= 1u << (k % 32);
                n = k / 32 + 1;
            }
        }
        w.U8(static_cast<uint8_t>(n));
        for (int i = 0; i < n; ++i) w.U32(words[i]);
    }

    void FlushPeer(Peer& p, double now) {
        if (p.state != PeerState::Connected) return;
        struct Ref {
            OutUnit* unit;  // nullptr: unreliable
            uint16_t seq;
            const std::vector<uint8_t>* data;
        };
        std::vector<Ref> refs;
        size_t limit = std::min<size_t>(p.rq.size(), kWindow);
        int fresh = 0;
        for (size_t i = 0; i < limit; ++i) {
            OutUnit& u = p.rq[i];
            if (u.acked) continue;
            if (u.sendCount == 0) {
                if (fresh >= kNewUnitsPerFlush) continue;
                ++fresh;
            } else {
                const double rto = p.Rto() * static_cast<double>(1u << std::min<int>(u.sendCount - 1, 3));
                const bool fast = p.FastLost(u, now);
                if (now - u.lastSent < rto && !fast) continue;
                p.stats.resends++;
                if (fast && now - u.lastSent < rto) p.stats.fastResends++;
            }
            u.sendCount++;
            u.lastSent = now;
            refs.push_back({&u, static_cast<uint16_t>(p.sendBase + i), &u.data});
        }
        for (auto& m : p.uq) refs.push_back({nullptr, 0, &m});

        if (refs.empty() && !p.ackDirty) return;
        ByteWriter w(kMtu);
        auto begin = [&] {
            w.Clear();
            Header(w, kData, p.token);
            WriteAck(w, p);
            ++p.xmitCounter;
        };
        begin();
        size_t baseSize = w.Size();
        for (const Ref& ref : refs) {
            const OutUnit* u = ref.unit;
            size_t need = 1 + (u ? 2 : 0) + (u && u->laned ? 5 : 0) + 3 + ref.data->size();
            if (w.Size() + need > kMtu && w.Size() > baseSize) {
                SendDatagram(p.addr, w.Buffer(), &p);
                begin();
            }
            if (!u) {
                w.U8(kUnreliable);
            } else {
                ref.unit->xmit = p.xmitCounter;
                w.U8(static_cast<uint8_t>(u->kind | (u->laned ? kKindLaned : 0)));
                w.U16(ref.seq);
                if (u->laned) {
                    w.U8(u->lane);
                    w.U16(u->laneSeq);
                    w.U16(u->barrier);
                }
            }
            w.VarU32(static_cast<uint32_t>(ref.data->size()));
            w.Bytes(ref.data->data(), ref.data->size());
        }
        SendDatagram(p.addr, w.Buffer(), &p);
        p.ackDirty = false;
        p.uq.clear();
    }

    void Timers(double now) {
        std::vector<PeerId> ids;
        ids.reserve(peers.size());
        for (auto& kv : peers) ids.push_back(kv.first);
        for (PeerId id : ids) {
            Peer* p = Find(id);
            if (!p) continue;
            if (p->state == PeerState::Connecting) {
                if (now - p->created > opt.connectTimeoutS) {
                    Drop(*p, DisconnectReason::ConnectFailed);
                    continue;
                }
                if (p->outgoing && now - p->lastConnectSend >= kConnectResend) SendConnect(*p);
                continue;
            }
            if (now - p->lastRecv > opt.timeoutS) {
                Drop(*p, DisconnectReason::Timeout);
                continue;
            }
            if (now - p->lastPing >= opt.pingIntervalS) {
                p->lastPing = now;
                uint32_t pid = ++p->pingId;
                p->pingSent[pid % 64] = now;
                p->pongGot[pid % 64] = false;
                SendControl(*p, kPing, [&](ByteWriter& w) {
                    w.U32(pid);
                    w.U32(static_cast<uint32_t>(now * 1000.0));
                });
            }
        }
    }

    void FlushAll() {
        double now = Now();
        for (auto& kv : peers) FlushPeer(kv.second, now);
        ReleaseDelayed();
    }

    void UpdateStats(Peer& p, double now) {
        // Only count pings old enough that their pong should have arrived.
        double settle = std::max(1.0, p.Rto() * 3);
        int total = 0, lost = 0;
        for (int k = 0; k < 64; ++k) {
            double t = p.pingSent[k];
            if (t <= 0 || now - t < settle || now - t > 30) continue;
            ++total;
            if (!p.pongGot[k]) ++lost;
        }
        p.stats.loss = total ? static_cast<float>(lost) / static_cast<float>(total) : 0.0f;
        p.stats.rttMs = p.srtt < 0 ? 0.0f : static_cast<float>(p.srtt * 1000.0);
    }

    void CloseAll() {
        KaStopThread();  // before the socket goes
        if (sock != INVALID_SOCKET) {
            for (auto& kv : peers)
                if (kv.second.state == PeerState::Connected) {
                    for (int i = 0; i < kDisconnectCopies; ++i) SendControl(kv.second, kDisconnect);
                }
            // Push out delayed sim traffic now so the disconnect is not lost with the socket.
            for (auto& d : delayed) RawSend(d.to, d.bytes.data(), d.bytes.size());
            closesocket(sock);
        }
        delayed.clear();
        peers.clear();
        byAddr.clear();
        events.clear();
        sock = INVALID_SOCKET;
        hosting = false;
        boundPort = 0;
        wsa.Stop();
    }
};

UdpTransport::UdpTransport(const UdpOptions& opt) : impl_(std::make_unique<Impl>()) { impl_->opt = opt; }
UdpTransport::~UdpTransport() { Close(); }

void UdpTransport::SetOptions(const UdpOptions& opt) {
    impl_->opt = opt;
    impl_->kaInterval = opt.keepaliveThreadS;
    impl_->kaMaxStall = opt.keepaliveMaxStallS;
}
const UdpOptions& UdpTransport::Options() const { return impl_->opt; }
uint16_t UdpTransport::BoundPort() const { return impl_->boundPort; }
bool UdpTransport::IsHosting() const { return impl_->hosting; }

bool UdpTransport::Host(uint16_t port) {
    Impl& m = *impl_;
    if (m.sock != INVALID_SOCKET) {
        if (m.hosting) return true;
        Log(kError, "udp: Host() on a transport that is already connecting as a client");
        return false;
    }
    if (!m.OpenSocket(port)) return false;
    m.hosting = true;
    Log(kInfo, "udp: hosting on port %u", m.boundPort);
    return true;
}

PeerId UdpTransport::Connect(std::string_view address) {
    Impl& m = *impl_;
    std::string s(address);
    uint16_t port = m.opt.defaultPort;
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        int pv = atoi(s.c_str() + colon + 1);
        if (pv <= 0 || pv > 65535) {
            Log(kError, "udp: bad port in '%s'", s.c_str());
            return kInvalidPeer;
        }
        port = static_cast<uint16_t>(pv);
        s.resize(colon);
    }
    if (s == "localhost") s = "127.0.0.1";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (!m.wsa.Start() || inet_pton(AF_INET, s.c_str(), &a.sin_addr) != 1) {
        Log(kError, "udp: bad address '%s' (expected ip[:port])", std::string(address).c_str());
        return kInvalidPeer;
    }
    if (!m.OpenSocket(0)) return kInvalidPeer;
    if (Peer* old = m.FindByAddr(a)) m.RemovePeer(old->id);
    Peer& p = m.NewPeer(a, Random32() | 1, true);
    Log(kInfo, "udp: connecting to %s (peer %llu)", AddrStr(a).c_str(), static_cast<unsigned long long>(p.id));
    m.SendConnect(p);
    return p.id;
}

bool UdpTransport::Send(PeerId id, std::span<const uint8_t> data, Reliability rel) {
    return SendOnLane(id, data, rel, 0);
}

bool UdpTransport::SendOnLane(PeerId id, std::span<const uint8_t> data, Reliability rel, uint8_t lane) {
    Impl& m = *impl_;
    if (lane >= kLanes) lane = 0;
    Peer* p = m.Find(id);
    if (!p || p->state != PeerState::Connected) return false;
    if (rel == Reliability::Unreliable) {
        if (data.size() > kMaxUnreliableMessage) return false;
        p->uq.emplace_back(data.begin(), data.end());
        return true;
    }
    if (data.size() > kMaxReliableMessage) return false;
    size_t units = data.size() <= kFragPayload ? 1 : (data.size() + 4 + kFragPayload - 1) / kFragPayload;
    if (p->rq.size() + units > kMaxQueuedUnits) {
        Log(kWarn, "udp: peer %llu send queue full", static_cast<unsigned long long>(id));
        return false;
    }
    // Lane bookkeeping per unit: its place in its lane, and how many lane-0 units were queued before it.
    auto push = [p, lane](OutUnit&& u) {
        if (lane) p->lanesUsed = true;
        u.lane = lane;
        u.laned = p->lanesUsed;
        u.barrier = p->laneSendSeq[0];
        u.laneSeq = p->laneSendSeq[lane]++;
        p->rq.push_back(std::move(u));
    };
    if (units == 1) {
        OutUnit u;
        u.kind = kReliableWhole;
        u.data.assign(data.begin(), data.end());
        push(std::move(u));
        return true;
    }
    size_t off = 0;
    bool first = true;
    while (off < data.size()) {
        OutUnit u;
        size_t cap = first ? kFragPayload - 4 : kFragPayload;
        size_t n = std::min(cap, data.size() - off);
        if (first) {
            u.kind = kFragFirst;
            ByteWriter w(n + 4);
            w.U32(static_cast<uint32_t>(data.size()));
            w.Bytes(data.data() + off, n);
            u.data = w.Take();
        } else {
            u.kind = off + n == data.size() ? kFragLast : kFragMiddle;
            u.data.assign(data.begin() + static_cast<ptrdiff_t>(off), data.begin() + static_cast<ptrdiff_t>(off + n));
        }
        off += n;
        first = false;
        push(std::move(u));
    }
    return true;
}

void UdpTransport::Flush() {
    if (impl_->sock != INVALID_SOCKET) impl_->FlushAll();
}

void UdpTransport::Poll(const EventHandler& handler) {
    Impl& m = *impl_;
    if (m.sock == INVALID_SOCKET) return;
    m.ReleaseDelayed();
    m.Receive();
    double now = Now();
    m.Timers(now);
    for (auto& kv : m.peers) m.UpdateStats(kv.second, now);

    std::vector<PendingEvent> evs;
    evs.swap(m.events);
    for (PendingEvent& e : evs) {
        if (m.sock == INVALID_SOCKET) break;                                  // handler closed us
        if (m.closedLocally.count(e.peer)) continue;                         // we disconnected it meanwhile
        Event ev{e.type, e.peer, e.reason, std::span<const uint8_t>(e.data.data(), e.data.size()), e.channel};
        if (handler) handler(ev);
    }
    m.closedLocally.clear();
    if (m.sock != INVALID_SOCKET) {
        m.FlushAll();
        m.KaSnapshot();
        m.KaStart();
    }
}

void UdpTransport::Disconnect(PeerId id) {
    Impl& m = *impl_;
    Peer* p = m.Find(id);
    if (!p) return;
    if (p->state == PeerState::Connected) {
        for (int i = 0; i < kDisconnectCopies; ++i) m.SendControl(*p, kDisconnect);
    }
    m.RemovePeer(id);
    m.closedLocally.insert(id);
    m.ReleaseDelayed();
}

void UdpTransport::Close() { impl_->CloseAll(); }

void UdpTransport::MarkEstablished(PeerId id) {
    if (Peer* p = impl_->Find(id)) p->established = true;
}

uint64_t UdpTransport::Evictions() const { return impl_->evictions; }
uint64_t UdpTransport::KeepalivesSent() const { return impl_->kaSent.load(); }

std::string UdpTransport::PeerAddress(PeerId id) const {
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end()) return {};
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &it->second.addr.sin_addr, ip, sizeof(ip));
    return ip;
}

bool UdpTransport::GetStats(PeerId id, PeerStats& out) const {
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end()) return false;
    out = it->second.stats;
    uint32_t q = 0;
    for (const auto& u : it->second.rq)
        if (!u.acked) ++q;
    out.reliableQueued = q;
    return true;
}

}  // namespace mc::net
