// MafiaCoop net: transport interface (see docs/ARCHITECTURE.md "Networking" and docs/protocol.md).
// Single-threaded and non-blocking: every call, and every event, happens on the thread that calls Poll().
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace mc::net {

// SteamID64 for the Steam backend, synthetic (1, 2, 3, ...) for UDP. 0 = invalid.
using PeerId = uint64_t;
constexpr PeerId kInvalidPeer = 0;

enum class Reliability : uint8_t {
    Unreliable,  // may be lost, duplicated never, order not guaranteed; max kMaxUnreliableMessage bytes
    Reliable,    // delivered once, in order (per peer); max kMaxReliableMessage bytes
};

constexpr size_t kMaxReliableMessage = 256 * 1024;
constexpr size_t kMaxUnreliableMessage = 1100;

enum class DisconnectReason : uint8_t {
    None = 0,
    Local,          // we called Disconnect()/Close() (never reported as an event)
    Remote,         // the other side closed cleanly
    Timeout,        // nothing heard for the timeout (10 s)
    ConnectFailed,  // handshake never completed (unreachable, no answer)
    Refused,        // the other side refused the transport connection (not hosting / full / not allowed)
    Error,          // socket or API error
};
const char* ToString(DisconnectReason r);

enum class EventType : uint8_t { Connected, Disconnected, Message };

struct Event {
    EventType type;
    PeerId peer;
    DisconnectReason reason;         // Disconnected only
    std::span<const uint8_t> data;   // Message only; valid only during the handler call
    Reliability channel;             // Message only: channel it arrived on
};

struct PeerStats {
    float rttMs = 0;        // smoothed round-trip time
    float loss = 0;         // 0..1, estimated packet loss (recent window)
    uint64_t bytesSent = 0, bytesReceived = 0;
    uint64_t packetsSent = 0, packetsReceived = 0;
    uint32_t resends = 0;   // reliable retransmissions (backend dependent)
};

using EventHandler = std::function<void(const Event&)>;

class Transport {
public:
    virtual ~Transport() = default;
    // Start accepting incoming connections. UDP binds the port (0 = any); Steam ignores it.
    virtual bool Host(uint16_t port) = 0;
    // Start connecting. UDP: "ip[:port]" (default port kDefaultPort). Steam: decimal SteamID64.
    // Returns the peer id (Connected or Disconnected(ConnectFailed) follows from Poll), or 0 on immediate failure.
    virtual PeerId Connect(std::string_view address) = 0;
    // Queue a message. Returns false if the peer is unknown/not connected or the message is too large.
    virtual bool Send(PeerId peer, std::span<const uint8_t> data, Reliability rel) = 0;
    // Pump: receive, retransmit, keepalive, timeouts; delivers events to handler. Call once per frame.
    virtual void Poll(const EventHandler& handler) = 0;
    // Close one connection (sends a clean disconnect). No Disconnected event is raised for it.
    virtual void Disconnect(PeerId peer) = 0;
    // Close everything (clean disconnect to every peer) and release the socket/session.
    virtual void Close() = 0;
    virtual bool GetStats(PeerId peer, PeerStats& out) const = 0;
    virtual bool IsHosting() const = 0;
    virtual const char* Name() const = 0;
    // Where a peer connects from, for per-address limits (UDP: its IPv4 without the port). "" = no address (the
    // session then keys by peer id, e.g. a SteamID).
    virtual std::string PeerAddress(PeerId) const { return {}; }
};

constexpr uint16_t kDefaultPort = 27960;

// Optional logging hook (default: silent). level: 0 debug, 1 info, 2 warn, 3 error.
using LogSink = void (*)(int level, const char* msg);
void SetLogSink(LogSink sink);

}  // namespace mc::net
