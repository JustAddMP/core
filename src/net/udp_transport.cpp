// MafiaCoop net: UDP transport. See docs/protocol.md "UDP transport" for the wire format.
#include "net/udp_transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
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
};

enum UnitKind : uint8_t {
    kUnreliable = 0,
    kReliableWhole = 1,
    kFragFirst = 2,  // payload starts with u32 total length
    kFragMiddle = 3,
    kFragLast = 4,
};

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
    double lastSent = 0;
    uint16_t sendCount = 0;
    bool acked = false;
};

struct InSlot {
    bool has = false;
    uint8_t kind = 0;
    std::vector<uint8_t> data;
};

enum class PeerState { Connecting, Connected };

struct Peer {
    PeerId id = 0;
    sockaddr_in addr{};
    uint32_t token = 0;  // connection nonce chosen by the connecting side
    PeerState state = PeerState::Connecting;
    bool outgoing = false;  // we initiated it
    uint64_t cookie = 0;    // outgoing: challenge cookie received
    double created = 0, lastRecv = 0, lastConnectSend = 0, lastPing = 0;

    // reliable send
    std::deque<OutUnit> rq;
    uint16_t sendBase = 0;  // seq of rq[0]
    // reliable receive
    uint16_t recvExpected = 0;
    std::array<InSlot, kWindow> slots;
    std::vector<uint8_t> assembly;
    uint32_t assemblyTotal = 0;
    bool assembling = false;
    bool ackDirty = false;
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
        return std::clamp(srtt + 4 * rttvar + 0.01, 0.05, 1.0);
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
    std::unordered_map<uint64_t, PeerId> byAddr;
    std::vector<PendingEvent> events;
    std::unordered_set<PeerId> closedLocally;  // peers Disconnect()ed while events for them are pending
    std::vector<Delayed> delayed;
    uint64_t simState = 1;
    uint8_t rxbuf[2048] = {};

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
        size_t incoming = 0, fromAddr = 0;
        for (auto& kv : peers) {
            if (kv.second.outgoing) continue;
            ++incoming;
            if (kv.second.addr.sin_addr.s_addr == from.sin_addr.s_addr) ++fromAddr;
        }
        // Per-address cap: otherwise one machine (any source port completes the cookie handshake) could hold every
        // slot with connections that never get past the session handshake, and nobody else could join.
        if (static_cast<int>(incoming) >= opt.maxPeers || static_cast<int>(fromAddr) >= opt.maxPeersPerAddress) {
            refuse(kRefuseFull);
            return;
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
            uint8_t kind = r.U8();
            uint16_t seq = 0;
            if (kind != kUnreliable) seq = r.U16();
            uint32_t len = r.VarU32();
            auto body = r.View(len);
            if (!r.Ok() || kind > kFragLast) break;
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
            s.kind = kind;
            s.data.assign(body.begin(), body.end());
        }
        DeliverInOrder(p);
    }

    void DeliverInOrder(Peer& p) {
        for (;;) {
            InSlot& s = p.slots[p.recvExpected % kWindow];
            if (!s.has) break;
            s.has = false;
            p.recvExpected++;
            std::vector<uint8_t> data = std::move(s.data);
            s.data = {};
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
                        p.assembling = false;
                        break;
                    }
                    p.assembling = true;
                    p.assemblyTotal = total;
                    p.assembly.clear();
                    p.assembly.reserve(total);
                    auto rest = fr.Rest();
                    p.assembly.insert(p.assembly.end(), rest.begin(), rest.end());
                    break;
                }
                case kFragMiddle:
                case kFragLast:
                    if (!p.assembling) break;
                    if (p.assembly.size() + data.size() > p.assemblyTotal) {
                        p.assembling = false;
                        break;
                    }
                    p.assembly.insert(p.assembly.end(), data.begin(), data.end());
                    if (s.kind == kFragLast) {
                        p.assembling = false;
                        if (p.assembly.size() == p.assemblyTotal)
                            Emit(EventType::Message, p.id, DisconnectReason::None, std::move(p.assembly),
                                 Reliability::Reliable);
                        p.assembly = {};
                    }
                    break;
                default:
                    break;
            }
        }
    }

    void ProcessAck(Peer& p, uint16_t cum, uint8_t nWords, const uint32_t* words) {
        double now = Now();
        auto ackIdx = [&](size_t i) {
            OutUnit& u = p.rq[i];
            if (u.acked || u.sendCount == 0) return;
            u.acked = true;
            if (u.sendCount == 1) p.RttSample(now - u.lastSent);  // Karn: only unambiguous samples
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
            uint8_t kind;
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
                double rto = p.Rto() * static_cast<double>(1u << std::min<int>(u.sendCount - 1, 3));
                if (now - u.lastSent < rto) continue;
                p.stats.resends++;
            }
            u.sendCount++;
            u.lastSent = now;
            refs.push_back({u.kind, static_cast<uint16_t>(p.sendBase + i), &u.data});
        }
        for (auto& m : p.uq) refs.push_back({kUnreliable, 0, &m});

        if (refs.empty() && !p.ackDirty) return;
        ByteWriter w(kMtu);
        auto begin = [&] {
            w.Clear();
            Header(w, kData, p.token);
            WriteAck(w, p);
        };
        begin();
        size_t baseSize = w.Size();
        for (const Ref& ref : refs) {
            size_t need = 1 + (ref.kind ? 2 : 0) + 3 + ref.data->size();
            if (w.Size() + need > kMtu && w.Size() > baseSize) {
                SendDatagram(p.addr, w.Buffer(), &p);
                begin();
            }
            w.U8(ref.kind);
            if (ref.kind) w.U16(ref.seq);
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
        if (sock != INVALID_SOCKET) {
            for (auto& kv : peers)
                if (kv.second.state == PeerState::Connected) {
                    SendControl(kv.second, kDisconnect);
                    SendControl(kv.second, kDisconnect);
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

void UdpTransport::SetOptions(const UdpOptions& opt) { impl_->opt = opt; }
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
    uint16_t port = kDefaultPort;
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
    Impl& m = *impl_;
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
    if (units == 1) {
        OutUnit u;
        u.kind = kReliableWhole;
        u.data.assign(data.begin(), data.end());
        p->rq.push_back(std::move(u));
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
        p->rq.push_back(std::move(u));
    }
    return true;
}

void UdpTransport::Flush() { impl_->FlushAll(); }

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
    if (m.sock != INVALID_SOCKET) m.FlushAll();
}

void UdpTransport::Disconnect(PeerId id) {
    Impl& m = *impl_;
    Peer* p = m.Find(id);
    if (!p) return;
    if (p->state == PeerState::Connected) {
        m.SendControl(*p, kDisconnect);
        m.SendControl(*p, kDisconnect);
    }
    m.RemovePeer(id);
    m.closedLocally.insert(id);
    m.ReleaseDelayed();
}

void UdpTransport::Close() { impl_->CloseAll(); }

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
    return true;
}

}  // namespace mc::net
