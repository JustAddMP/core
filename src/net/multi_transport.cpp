#include "net/multi_transport.h"

#include "net/net_internal.h"

namespace mc::net {
using namespace detail;

MultiTransport::~MultiTransport() { Close(); }

void MultiTransport::Add(std::unique_ptr<Transport> part) {
    if (!part) return;
    if (!_name.empty()) _name += "+";
    _name += part->Name();
    _parts.push_back(std::move(part));
}

bool MultiTransport::Host(uint16_t port) {
    bool any = false;
    for (auto& p : _parts) {
        if (p->Host(port)) {
            any = true;
        } else {
            Log(kWarn, "multi: %s could not start hosting; continuing without it", p->Name());
        }
    }
    return any;
}

PeerId MultiTransport::Connect(std::string_view address) {
    if (_parts.empty()) return kInvalidPeer;
    PeerId id = _parts[0]->Connect(address);
    if (id != kInvalidPeer) _owner[id] = 0;
    return id;
}

Transport* MultiTransport::Owner(PeerId peer) const {
    auto it = _owner.find(peer);
    return it == _owner.end() ? nullptr : _parts[it->second].get();
}

bool MultiTransport::Send(PeerId peer, std::span<const uint8_t> data, Reliability rel) {
    Transport* t = Owner(peer);
    return t && t->Send(peer, data, rel);
}

void MultiTransport::Poll(const EventHandler& handler) {
    for (size_t i = 0; i < _parts.size(); ++i) {
        _parts[i]->Poll([&](const Event& e) {
            if (e.type == EventType::Connected) {
                auto it = _owner.find(e.peer);
                if (it != _owner.end() && it->second != i) {
                    Log(kError, "multi: peer id %llu exists on %s and %s; dropping the new one",
                        static_cast<unsigned long long>(e.peer), _parts[it->second]->Name(), _parts[i]->Name());
                    _parts[i]->Disconnect(e.peer);
                    return;
                }
                _owner[e.peer] = i;
            } else if (e.type == EventType::Message) {
                auto it = _owner.find(e.peer);
                if (it == _owner.end() || it->second != i) return;  // not a peer we routed to this part
            }
            handler(e);
            if (e.type == EventType::Disconnected) {
                auto it = _owner.find(e.peer);
                if (it != _owner.end() && it->second == i) _owner.erase(it);
            }
        });
    }
}

void MultiTransport::Disconnect(PeerId peer) {
    auto it = _owner.find(peer);
    if (it == _owner.end()) return;
    _parts[it->second]->Disconnect(peer);
    _owner.erase(it);
}

void MultiTransport::Close() {
    for (auto& p : _parts) p->Close();
    _owner.clear();
}

bool MultiTransport::GetStats(PeerId peer, PeerStats& out) const {
    Transport* t = Owner(peer);
    return t && t->GetStats(peer, out);
}

std::string MultiTransport::PeerAddress(PeerId peer) const {
    Transport* t = Owner(peer);
    return t ? t->PeerAddress(peer) : std::string();
}

uint64_t MultiTransport::PeerSteamId(PeerId peer) const {
    Transport* t = Owner(peer);
    return t ? t->PeerSteamId(peer) : 0;
}

void MultiTransport::MarkEstablished(PeerId peer) {
    if (Transport* t = Owner(peer)) t->MarkEstablished(peer);
}

bool MultiTransport::IsHosting() const {
    for (auto& p : _parts)
        if (p->IsHosting()) return true;
    return false;
}

}  // namespace mc::net
