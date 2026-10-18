// net: Steam P2P transport over ISteamNetworking006 (flat API from the game's steam_api64.dll).
// Peer ids are SteamID64s. NAT traversal and relay are Steam's. Our own tiny control layer on top gives
// Connected/Disconnected events, keepalive/timeout and RTT (see docs/protocol.md "Steam transport").
#pragma once
#include <functional>
#include <memory>

#include "net/transport.h"

namespace mc::net {

struct SteamOptions {
    double timeoutS = 10.0;
    double connectTimeoutS = 20.0;  // relay/NAT setup can take a few seconds
    double pingIntervalS = 1.0;
    int channel = 7;                // P2P channel of our traffic; reliable lane k > 0 uses channel + k (kLanes)
    int maxPeers = 8;
};

class SteamTransport final : public Transport {
public:
    explicit SteamTransport(const SteamOptions& opt = {});
    ~SteamTransport() override;
    SteamTransport(const SteamTransport&) = delete;
    SteamTransport& operator=(const SteamTransport&) = delete;

    // Resolves the flat API from the loaded steam_api64.dll and registers the P2P callbacks.
    // Steam must already be initialised (by the game). Safe to call repeatedly.
    bool Init();

    // Who may open a session with us while hosting (e.g. lobby members). Default: nobody.
    void SetAcceptFilter(std::function<bool(PeerId)> filter);

    bool Host(uint16_t port) override;  // port ignored
    PeerId Connect(std::string_view address) override;  // decimal SteamID64
    bool Send(PeerId peer, std::span<const uint8_t> data, Reliability rel) override;
    bool SendOnLane(PeerId peer, std::span<const uint8_t> data, Reliability rel, uint8_t lane) override;
    void Poll(const EventHandler& handler) override;
    void Disconnect(PeerId peer) override;
    void Close() override;
    bool GetStats(PeerId peer, PeerStats& out) const override;
    bool IsHosting() const override;
    const char* Name() const override { return "steam"; }
    uint64_t PeerSteamId(PeerId peer) const override { return peer; }  // peer ids are SteamID64s

    PeerId LocalId() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mc::net
