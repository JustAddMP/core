// net: traffic monitor. Samples a Session (per-peer transport stats, per-message traffic, rate-limit drops) and turns
// the deltas since the last report into log lines and a few short lines for a debug overlay. No game or OS
// dependencies; the caller passes the time. See docs/protocol.md "Traffic statistics".
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "net/session.h"

namespace mc::net {

class NetMonitor {
public:
    // Optional names for app message ids in the report ("PlayerState" instead of "0x0100"). nullptr = hex.
    std::function<const char*(uint16_t)> nameOf;

    // Takes a sample. When at least `interval` seconds have passed since the last report (or since the first
    // sample), builds a new report from the deltas and returns true. A reset session (new Host/Join) starts over.
    bool Sample(Session& session, double now, double interval);
    void Reset();

    // Last report: full lines for a log, short lines for an overlay.
    const std::vector<std::string>& LogLines() const { return log_; }
    const std::vector<std::string>& OverlayLines() const { return overlay_; }

    // Last report's totals over all peers.
    struct Totals {
        double outBytesPerS = 0, inBytesPerS = 0;
        uint64_t resends = 0, relayDropped = 0, chatDropped = 0, sendFailed = 0;
        uint32_t maxQueued = 0;
        double seconds = 0;
    };
    const Totals& Last() const { return totals_; }

private:
    struct PeerSnap {
        PeerStats st;
        bool have = false;
    };
    std::string Name(uint16_t id) const;

    bool started_ = false;
    double lastReport_ = 0;
    std::map<PeerId, PeerSnap> peers_;
    std::map<uint16_t, MsgTraffic> traffic_;
    RateLimitStats rate_;
    std::vector<std::string> log_, overlay_;
    Totals totals_;
};

}  // namespace mc::net
