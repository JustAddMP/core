// net: direct UDP transport (Winsock), own reliability layer. Wire format in docs/protocol.md.
// IPv4 only. Single-threaded: call everything from the thread that calls Poll(). The one exception is internal: a
// keepalive thread (UdpOptions::keepaliveThreadS) that only sends ALIVE packets while Poll() is not being called.
#pragma once
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "net/transport.h"

namespace mc::net {

struct UdpOptions {
    double timeoutS = 10.0;         // connected peer: drop after this long without any packet
    double connectTimeoutS = 10.0;  // give up connecting after this long
    double pingIntervalS = 0.5;     // keepalive / RTT probe
    // Keepalive thread: when Poll() has not run for twice this long (the calling thread is stalled, e.g. a game
    // loading a world), a thread of the transport sends an ALIVE packet to every connected peer at this interval, so
    // a short timeoutS does not drop a player whose game is only busy. 0 = no thread. It gives up after
    // keepaliveMaxStallS of stall (a hung process is dropped like a killed one, after that plus the peer's timeout).
    double keepaliveThreadS = 0.25;
    double keepaliveMaxStallS = 60.0;
    int maxPeers = 8;               // incoming connection limit when hosting
    int maxPeersPerAddress = 4;     // incoming connections from one IPv4 address (any port)
    // When a limit above is reached, a new connection evicts the oldest incoming one the session has not marked
    // established (MarkEstablished: handshake done); established players are never evicted.
    uint16_t defaultPort = kDefaultPort;  // Connect("ip") without a port
    // Test-only network simulation, applied to datagrams this transport SENDS.
    float simLoss = 0.0f;           // 0..1 drop probability
    float simLatencyMs = 0.0f;      // base one-way delay
    float simJitterMs = 0.0f;       // +- uniform jitter (reorders packets)
    float simDuplicate = 0.0f;      // 0..1 probability a datagram is sent twice
    uint32_t simSeed = 1;
    // Test-only: sees every datagram as it goes to the socket (after simulation), e.g. to scan for secrets.
    std::function<void(std::span<const uint8_t>)> testOnSend;
};

class UdpTransport final : public Transport {
public:
    explicit UdpTransport(const UdpOptions& opt = {});
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    bool Host(uint16_t port) override;
    PeerId Connect(std::string_view address) override;
    bool Send(PeerId peer, std::span<const uint8_t> data, Reliability rel) override;
    void Poll(const EventHandler& handler) override;
    void Disconnect(PeerId peer) override;
    void Close() override;
    bool GetStats(PeerId peer, PeerStats& out) const override;
    bool IsHosting() const override;
    const char* Name() const override { return "udp"; }
    std::string PeerAddress(PeerId peer) const override;  // "a.b.c.d"
    void MarkEstablished(PeerId peer) override;

    // Options may be changed at any time (e.g. switch simulation on/off mid-test).
    void SetOptions(const UdpOptions& opt);
    const UdpOptions& Options() const;
    uint16_t BoundPort() const;
    // Send everything queued now (Poll() does this too).
    void Flush();
    // Incoming connections evicted to make room (see UdpOptions::maxPeers).
    uint64_t Evictions() const;
    // ALIVE packets the keepalive thread sent (while Poll() was stalled).
    uint64_t KeepalivesSent() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mc::net
