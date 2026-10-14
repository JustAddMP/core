// MafiaCoop net: Steam P2P transport. Wire format in docs/protocol.md "Steam transport".
#include "net/steam_transport.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/net_internal.h"
#include "net/steam_flat.h"
#include "net/wire.h"

namespace mc::net {
using namespace detail;

namespace {

constexpr uint8_t kMagic = 0x4D;  // 'M'
enum Ctrl : uint8_t {
    kConnect = 1,
    kAccept = 2,
    kDataReliable = 3,
    kDataUnreliable = 4,
    kPing = 5,
    kPong = 6,
    kDisconnect = 7,
    kRefuse = 8,
};
constexpr double kConnectResend = 1.0;
constexpr double kPendingRequestKeep = 15.0;  // re-check denied session requests this long (lobby lag)

enum class PeerState { Connecting, Connected };

struct Peer {
    PeerId id = 0;
    PeerState state = PeerState::Connecting;
    bool outgoing = false;
    double created = 0, lastRecv = 0, lastConnectSend = 0, lastPing = 0;
    double srtt = -1;
    uint32_t pingId = 0;
    std::array<double, 64> pingSent{};
    std::array<bool, 64> pongGot{};
    PeerStats stats;
};

struct PendingEvent {
    EventType type;
    PeerId peer;
    DisconnectReason reason;
    Reliability channel;
    std::vector<uint8_t> data;
};

}  // namespace

struct SteamTransport::Impl {
    SteamOptions opt;
    bool inited = false;
    bool hosting = false;
    bool open = false;
    PeerId self = 0;
    std::function<bool(PeerId)> filter;
    std::unordered_map<PeerId, Peer> peers;
    std::unordered_map<PeerId, double> pendingRequests;  // session requests not (yet) allowed by the filter
    std::unordered_set<PeerId> closedLocally;
    std::vector<PendingEvent> events;
    std::vector<uint8_t> rx;
    int subRequest = 0, subFail = 0;

    steam::Api& api() { return steam::Get(); }

    void Emit(EventType t, PeerId id, DisconnectReason r = DisconnectReason::None, std::vector<uint8_t> d = {},
              Reliability ch = Reliability::Reliable) {
        events.push_back({t, id, r, ch, std::move(d)});
    }

    bool Raw(PeerId to, uint8_t ctrl, std::span<const uint8_t> body, int sendType) {
        std::vector<uint8_t> buf;
        buf.reserve(body.size() + 2);
        buf.push_back(kMagic);
        buf.push_back(ctrl);
        buf.insert(buf.end(), body.begin(), body.end());
        bool ok = api().Net_SendP2PPacket(api().networking, to, buf.data(), static_cast<uint32_t>(buf.size()),
                                          sendType, opt.channel);
        if (ok) {
            auto it = peers.find(to);
            if (it != peers.end()) {
                it->second.stats.bytesSent += buf.size();
                it->second.stats.packetsSent++;
            }
        }
        return ok;
    }
    bool Ctrl(PeerId to, uint8_t ctrl, int sendType = steam::k_EP2PSendUnreliable) {
        return Raw(to, ctrl, {}, sendType);
    }

    Peer* Find(PeerId id) {
        auto it = peers.find(id);
        return it == peers.end() ? nullptr : &it->second;
    }

    bool Allowed(PeerId id) const { return hosting && filter && filter(id); }

    void Drop(PeerId id, DisconnectReason r, bool closeSession = true) {
        Peer* p = Find(id);
        if (!p) return;
        bool report = p->state == PeerState::Connected || p->outgoing;
        Log(kInfo, "steam: peer %llu disconnected: %s", static_cast<unsigned long long>(id), ToString(r));
        peers.erase(id);
        if (closeSession) api().Net_CloseP2PSessionWithUser(api().networking, id);
        if (report) Emit(EventType::Disconnected, id, r);
    }

    void MarkConnected(Peer& p) {
        if (p.state == PeerState::Connected) return;
        p.state = PeerState::Connected;
        p.lastPing = 0;
        Log(kInfo, "steam: peer %llu connected", static_cast<unsigned long long>(p.id));
        Emit(EventType::Connected, p.id);
    }

    void OnSessionRequest(PeerId from) {
        if (!open) return;
        if (Find(from) || Allowed(from)) {
            api().Net_AcceptP2PSessionWithUser(api().networking, from);
            pendingRequests.erase(from);
            Log(kInfo, "steam: accepted P2P session from %llu", static_cast<unsigned long long>(from));
        } else {
            pendingRequests[from] = Now();
            Log(kInfo, "steam: P2P session request from %llu not allowed (yet)", static_cast<unsigned long long>(from));
        }
    }

    void OnConnectFail(PeerId from, uint8_t err) {
        if (!Find(from)) return;
        Log(kWarn, "steam: P2P session with %llu failed (EP2PSessionError %u)", static_cast<unsigned long long>(from),
            err);
        Peer* p = Find(from);
        Drop(from, p->state == PeerState::Connected ? DisconnectReason::Error : DisconnectReason::ConnectFailed);
    }

    void Receive() {
        uint32_t size = 0;
        for (int guard = 0; guard < 4096 && api().Net_IsP2PPacketAvailable(api().networking, &size, opt.channel);
             ++guard) {
            rx.resize(std::max<uint32_t>(size, 1));
            uint64_t from = 0;
            uint32_t got = 0;
            if (!api().Net_ReadP2PPacket(api().networking, rx.data(), static_cast<uint32_t>(rx.size()), &got, &from,
                                         opt.channel))
                break;
            OnPacket(from, rx.data(), got);
        }
    }

    void OnPacket(PeerId from, const uint8_t* data, uint32_t len) {
        if (len < 2 || data[0] != kMagic) return;
        uint8_t ctrl = data[1];
        std::span<const uint8_t> body(data + 2, len - 2);
        Peer* p = Find(from);
        if (p) {
            p->stats.bytesReceived += len;
            p->stats.packetsReceived++;
        }

        if (ctrl == kConnect) {
            if (p) {
                if (!p->outgoing) Ctrl(from, kAccept, steam::k_EP2PSendReliable);  // duplicate CONNECT
                p->lastRecv = Now();
                return;
            }
            if (!Allowed(from)) {
                Ctrl(from, kRefuse, steam::k_EP2PSendReliable);
                return;
            }
            size_t incoming = 0;
            for (auto& kv : peers)
                if (!kv.second.outgoing) ++incoming;
            if (static_cast<int>(incoming) >= opt.maxPeers) {
                Ctrl(from, kRefuse, steam::k_EP2PSendReliable);
                return;
            }
            Peer& np = peers[from];
            np.id = from;
            np.created = np.lastRecv = Now();
            MarkConnected(np);
            Ctrl(from, kAccept, steam::k_EP2PSendReliable);
            return;
        }
        if (!p) return;  // not ours (or stale after a disconnect)
        p->lastRecv = Now();
        switch (ctrl) {
            case kAccept:
                if (p->outgoing) MarkConnected(*p);
                return;
            case kRefuse:
                if (p->outgoing && p->state == PeerState::Connecting) Drop(from, DisconnectReason::Refused);
                return;
            case kDisconnect:
                Drop(from, DisconnectReason::Remote);
                return;
            default:
                break;
        }
        if (p->outgoing && p->state == PeerState::Connecting) MarkConnected(*p);  // ACCEPT overtaken by data
        if (p->state != PeerState::Connected) return;
        switch (ctrl) {
            case kDataReliable:
                // Steam allows up to 1 MB per reliable packet; the Transport contract (and every sender of ours) is
                // kMaxReliableMessage, so anything bigger is not from our code.
                if (body.size() > kMaxReliableMessage) break;
                Emit(EventType::Message, from, DisconnectReason::None, std::vector<uint8_t>(body.begin(), body.end()),
                     Reliability::Reliable);
                break;
            case kDataUnreliable:
                if (body.size() > kMaxUnreliableMessage) break;
                Emit(EventType::Message, from, DisconnectReason::None, std::vector<uint8_t>(body.begin(), body.end()),
                     Reliability::Unreliable);
                break;
            case kPing:
                Raw(from, kPong, body, steam::k_EP2PSendUnreliableNoDelay);
                break;
            case kPong: {
                ByteReader r(body);
                uint32_t id = r.U32();
                if (!r.Ok()) break;
                if (p->pingId - id < 64 && p->pingSent[id % 64] > 0 && !p->pongGot[id % 64]) {
                    p->pongGot[id % 64] = true;
                    double s = Now() - p->pingSent[id % 64];
                    p->srtt = p->srtt < 0 ? s : 0.875 * p->srtt + 0.125 * s;
                }
                break;
            }
            default:
                break;
        }
    }

    void Timers(double now) {
        std::vector<PeerId> ids;
        for (auto& kv : peers) ids.push_back(kv.first);
        for (PeerId id : ids) {
            Peer* p = Find(id);
            if (!p) continue;
            if (p->state == PeerState::Connecting) {
                if (now - p->created > opt.connectTimeoutS) {
                    Drop(id, DisconnectReason::ConnectFailed);
                    continue;
                }
                if (now - p->lastConnectSend >= kConnectResend) {
                    p->lastConnectSend = now;
                    Ctrl(id, kConnect, steam::k_EP2PSendReliable);
                }
                continue;
            }
            if (now - p->lastRecv > opt.timeoutS) {
                Drop(id, DisconnectReason::Timeout);
                continue;
            }
            if (now - p->lastPing >= opt.pingIntervalS) {
                p->lastPing = now;
                uint32_t pid = ++p->pingId;
                p->pingSent[pid % 64] = now;
                p->pongGot[pid % 64] = false;
                ByteWriter w(8);
                w.U32(pid);
                Raw(id, kPing, w.Span(), steam::k_EP2PSendUnreliableNoDelay);
            }
            // stats
            int total = 0, lost = 0;
            for (int k = 0; k < 64; ++k) {
                double t = p->pingSent[k];
                if (t <= 0 || now - t < 2.0 || now - t > 60) continue;
                ++total;
                if (!p->pongGot[k]) ++lost;
            }
            p->stats.loss = total ? static_cast<float>(lost) / static_cast<float>(total) : 0.0f;
            p->stats.rttMs = p->srtt < 0 ? 0.0f : static_cast<float>(p->srtt * 1000.0);
        }
        // Session requests that arrived before the filter allowed them (e.g. lobby membership not yet visible).
        for (auto it = pendingRequests.begin(); it != pendingRequests.end();) {
            if (Allowed(it->first)) {
                api().Net_AcceptP2PSessionWithUser(api().networking, it->first);
                Log(kInfo, "steam: accepted deferred P2P session from %llu", static_cast<unsigned long long>(it->first));
                it = pendingRequests.erase(it);
            } else if (now - it->second > kPendingRequestKeep) {
                it = pendingRequests.erase(it);
            } else {
                ++it;
            }
        }
    }
};

SteamTransport::SteamTransport(const SteamOptions& opt) : impl_(std::make_unique<Impl>()) { impl_->opt = opt; }

SteamTransport::~SteamTransport() {
    Close();
    if (impl_->subRequest) steam::Unsubscribe(impl_->subRequest);
    if (impl_->subFail) steam::Unsubscribe(impl_->subFail);
}

bool SteamTransport::Init() {
    Impl& m = *impl_;
    if (m.inited) return true;
    if (!steam::Load() || !steam::AcquireInterfaces()) return false;
    m.self = steam::Get().User_GetSteamID(steam::Get().user);
    steam::Get().Net_AllowP2PPacketRelay(steam::Get().networking, true);
    Impl* pm = &m;
    m.subRequest = steam::Subscribe(steam::kCb_P2PSessionRequest, sizeof(steam::P2PSessionRequest_t),
                                    [pm](const void* d) {
                                        pm->OnSessionRequest(static_cast<const steam::P2PSessionRequest_t*>(d)->m_steamIDRemote.v);
                                    });
    m.subFail = steam::Subscribe(steam::kCb_P2PSessionConnectFail, sizeof(steam::P2PSessionConnectFail_t),
                                 [pm](const void* d) {
                                     auto* f = static_cast<const steam::P2PSessionConnectFail_t*>(d);
                                     pm->OnConnectFail(f->m_steamIDRemote.v, f->m_eP2PSessionError);
                                 });
    m.inited = true;
    Log(kInfo, "steam: transport ready, local SteamID %llu", static_cast<unsigned long long>(m.self));
    return true;
}

void SteamTransport::SetAcceptFilter(std::function<bool(PeerId)> filter) { impl_->filter = std::move(filter); }

PeerId SteamTransport::LocalId() const { return impl_->self; }
bool SteamTransport::IsHosting() const { return impl_->hosting; }

bool SteamTransport::Host(uint16_t) {
    if (!Init()) return false;
    impl_->hosting = true;
    impl_->open = true;
    if (!impl_->filter) Log(kWarn, "steam: hosting without an accept filter: every session request is refused");
    return true;
}

PeerId SteamTransport::Connect(std::string_view address) {
    if (!Init()) return kInvalidPeer;
    Impl& m = *impl_;
    std::string s(address);
    char* end = nullptr;
    unsigned long long id = strtoull(s.c_str(), &end, 10);
    if (!id || !end || *end || id == m.self) {
        Log(kError, "steam: bad SteamID '%s'", s.c_str());
        return kInvalidPeer;
    }
    m.open = true;
    if (m.peers.count(id)) m.Drop(id, DisconnectReason::Local, true);
    Peer& p = m.peers[id];
    p.id = id;
    p.outgoing = true;
    p.created = p.lastRecv = Now();
    p.lastConnectSend = Now();
    // Sending to a user implicitly accepts their session; the reliable CONNECT opens it.
    m.Ctrl(id, kConnect, steam::k_EP2PSendReliable);
    Log(kInfo, "steam: connecting to %llu", id);
    return id;
}

bool SteamTransport::Send(PeerId id, std::span<const uint8_t> data, Reliability rel) {
    Impl& m = *impl_;
    Peer* p = m.Find(id);
    if (!p || p->state != PeerState::Connected) return false;
    if (rel == Reliability::Unreliable) {
        if (data.size() > kMaxUnreliableMessage) return false;
        return m.Raw(id, kDataUnreliable, data, steam::k_EP2PSendUnreliable);
    }
    if (data.size() > kMaxReliableMessage) return false;
    return m.Raw(id, kDataReliable, data, steam::k_EP2PSendReliable);
}

void SteamTransport::Poll(const EventHandler& handler) {
    Impl& m = *impl_;
    if (!m.inited) return;
    steam::Pump();
    if (!m.open) return;
    m.Receive();
    m.Timers(Now());
    std::vector<PendingEvent> evs;
    evs.swap(m.events);
    for (PendingEvent& e : evs) {
        if (!m.open) break;
        if (m.closedLocally.count(e.peer)) continue;
        Event ev{e.type, e.peer, e.reason, std::span<const uint8_t>(e.data.data(), e.data.size()), e.channel};
        if (handler) handler(ev);
    }
    m.closedLocally.clear();
}

void SteamTransport::Disconnect(PeerId id) {
    Impl& m = *impl_;
    Peer* p = m.Find(id);
    if (!p) return;
    if (p->state == PeerState::Connected) m.Ctrl(id, kDisconnect, steam::k_EP2PSendReliable);
    m.peers.erase(id);
    m.closedLocally.insert(id);
    // The session is closed when the peer answers or times out; closing it now could drop the DISCONNECT.
}

void SteamTransport::Close() {
    Impl& m = *impl_;
    if (!m.inited) return;
    for (auto& kv : m.peers) {
        if (kv.second.state == PeerState::Connected) m.Ctrl(kv.first, kDisconnect, steam::k_EP2PSendReliable);
    }
    m.peers.clear();
    m.events.clear();
    m.pendingRequests.clear();
    m.hosting = false;
    m.open = false;
}

bool SteamTransport::GetStats(PeerId id, PeerStats& out) const {
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end()) return false;
    out = it->second.stats;
    return true;
}

}  // namespace mc::net
