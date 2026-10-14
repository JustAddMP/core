// MafiaCoop net: one Transport made of several (e.g. UDP + Steam), so a single host Session accepts guests from
// both. Peer ids of the parts must not collide: UDP uses small synthetic ids (1, 2, 3, ...), Steam uses SteamID64s
// (always >= 2^52 for individual accounts), so they never overlap in practice; a collision is detected and logged.
// Connect() goes through the first part only (guests use a single transport).
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/transport.h"

namespace mc::net {

class MultiTransport final : public Transport {
public:
    MultiTransport() = default;
    ~MultiTransport() override;
    MultiTransport(const MultiTransport&) = delete;
    MultiTransport& operator=(const MultiTransport&) = delete;

    // Takes ownership. Add every part before Host()/Connect().
    void Add(std::unique_ptr<Transport> part);
    size_t PartCount() const { return _parts.size(); }
    Transport* Part(size_t i) const { return i < _parts.size() ? _parts[i].get() : nullptr; }

    // Hosts on every part; true if at least one part hosts (failures are logged).
    bool Host(uint16_t port) override;
    PeerId Connect(std::string_view address) override;
    bool Send(PeerId peer, std::span<const uint8_t> data, Reliability rel) override;
    void Poll(const EventHandler& handler) override;
    void Disconnect(PeerId peer) override;
    void Close() override;
    bool GetStats(PeerId peer, PeerStats& out) const override;
    bool IsHosting() const override;
    const char* Name() const override { return _name.c_str(); }
    std::string PeerAddress(PeerId peer) const override;

    // Which part a peer belongs to (nullptr if unknown).
    Transport* Owner(PeerId peer) const;

private:
    std::vector<std::unique_ptr<Transport>> _parts;
    std::unordered_map<PeerId, size_t> _owner;  // peer -> index into _parts
    std::string _name;
};

}  // namespace mc::net
