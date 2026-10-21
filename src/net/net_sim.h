#pragma once
// net: net_sim - the test tool that delays (and can drop / duplicate) what this game sends over UDP, so a one-PC
// test over loopback has a round trip like a real link (docs/replication.md "Simulated lag"). Not a sync setting: it
// changes nothing but when the UDP transport puts a datagram on the socket (UdpOptions::sim*, applied to every
// datagram this game sends, so each side adds its own one-way delay: latency=12,jitter=3 on both games gives a round
// trip of about 24 ms, 18-30 ms). Never applied to Steam P2P.
//
//   --net-sim latency=12,jitter=3,loss=0,dup=0      (ini: net_sim = latency=12,jitter=3; any [section])
//   latency  ms one way (0..1000)       jitter  +- ms, uniform (0..1000; reorders datagrams)
//   loss     % of datagrams dropped (0..100)        dup  % sent twice (0..100)
//   "off", "0", "none" or empty: no simulation. Keys in any order, missing ones 0; an unknown key or a bad number is
//   an error (the game then runs without it and logs why).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "net/udp_transport.h"

namespace mc::net {

struct NetSim {
    float latencyMs = 0, jitterMs = 0, lossPct = 0, dupPct = 0;
    bool On() const { return latencyMs > 0 || jitterMs > 0 || lossPct > 0 || dupPct > 0; }
};

// Parses `text` (see above) into `out`. Returns false (out left as no simulation, *err why) on an error.
inline bool ParseNetSim(std::string_view text, NetSim& out, std::string* err = nullptr) {
    out = NetSim{};
    auto fail = [&](const std::string& why) {
        out = NetSim{};
        if (err) *err = why;
        return false;
    };
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            s.remove_suffix(1);
        return s;
    };
    text = trim(text);
    if (text.empty() || text == "0" || text == "off" || text == "none" || text == "OFF") return true;
    while (!text.empty()) {
        const size_t comma = text.find_first_of(",; ");
        std::string_view item = trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) return fail("'" + std::string(item) + "' is not key=value");
        const std::string key(trim(item.substr(0, eq)));
        const std::string val(trim(item.substr(eq + 1)));
        char* end = nullptr;
        const double v = std::strtod(val.c_str(), &end);
        if (val.empty() || !end || *end != '\0' || !(v >= 0)) return fail("bad number '" + val + "' for " + key);
        if (key == "latency" || key == "lat") {
            if (v > 1000) return fail("latency over 1000 ms");
            out.latencyMs = static_cast<float>(v);
        } else if (key == "jitter") {
            if (v > 1000) return fail("jitter over 1000 ms");
            out.jitterMs = static_cast<float>(v);
        } else if (key == "loss") {
            if (v > 100) return fail("loss over 100 %");
            out.lossPct = static_cast<float>(v);
        } else if (key == "dup" || key == "duplicate") {
            if (v > 100) return fail("dup over 100 %");
            out.dupPct = static_cast<float>(v);
        } else {
            return fail("unknown key '" + key + "' (latency, jitter, loss, dup)");
        }
    }
    if (err) err->clear();
    return true;
}

// "latency=12,jitter=3,loss=0,dup=0" (or "off")
inline std::string NetSimText(const NetSim& s) {
    if (!s.On()) return "off";
    char b[96];
    std::snprintf(b, sizeof b, "latency=%g,jitter=%g,loss=%g,dup=%g", s.latencyMs, s.jitterMs, s.lossPct, s.dupPct);
    return b;
}

inline void ApplyNetSim(const NetSim& s, UdpOptions& o) {
    o.simLatencyMs = s.latencyMs;
    o.simJitterMs = s.jitterMs;
    o.simLoss = s.lossPct / 100.0f;
    o.simDuplicate = s.dupPct / 100.0f;
}

}  // namespace mc::net
