#include "net/net_monitor.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace mc::net {

namespace {

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string Kb(double bytesPerS) { return Fmt("%.1f KB/s", bytesPerS / 1024.0); }

template <class T>
T Delta(T now, T prev) {
    return now >= prev ? now - prev : 0;
}

}  // namespace

void NetMonitor::Reset() {
    started_ = false;
    peers_.clear();
    traffic_.clear();
    rate_ = {};
    log_.clear();
    overlay_.clear();
    totals_ = {};
}

std::string NetMonitor::Name(uint16_t id) const {
    if (nameOf)
        if (const char* n = nameOf(id); n && *n) return n;
    return Fmt("0x%04x", id);
}

bool NetMonitor::Sample(Session& session, double now, double interval) {
    // a counter that went backwards (or vanished) = the session was reset (Leave + Host/Join): start over
    const auto& traffic = session.Traffic();
    for (const auto& [id, t] : traffic_) {
        auto it = traffic.find(id);
        if (it == traffic.end() || it->second.sentBytes < t.sentBytes || it->second.recvBytes < t.recvBytes) {
            Reset();
            break;
        }
    }
    if (!started_) {
        started_ = true;
        lastReport_ = now;
        traffic_ = traffic;
        rate_ = session.RateLimits();
        peers_.clear();
        for (const auto& p : session.Players()) {
            if (p.peer == kInvalidPeer) continue;
            PeerSnap& s = peers_[p.peer];
            s.have = session.GetTransport().GetStats(p.peer, s.st);
        }
        return false;
    }
    const double dt = now - lastReport_;
    if (dt < interval || dt <= 0) return false;
    lastReport_ = now;
    log_.clear();
    overlay_.clear();
    totals_ = {};
    totals_.seconds = dt;

    // ---- per peer (transport) ----
    std::map<PeerId, PeerSnap> next;
    for (const auto& p : session.Players()) {
        if (p.peer == kInvalidPeer || p.id == session.LocalId()) continue;
        PeerStats st;
        if (!session.GetTransport().GetStats(p.peer, st)) continue;
        PeerSnap& snap = next[p.peer];
        snap.st = st;
        snap.have = true;
        PeerStats prev;
        if (auto it = peers_.find(p.peer); it != peers_.end() && it->second.have) prev = it->second.st;
        const double out = static_cast<double>(Delta(st.bytesSent, prev.bytesSent)) / dt;
        const double in = static_cast<double>(Delta(st.bytesReceived, prev.bytesReceived)) / dt;
        const double pout = static_cast<double>(Delta(st.packetsSent, prev.packetsSent)) / dt;
        const double pin = static_cast<double>(Delta(st.packetsReceived, prev.packetsReceived)) / dt;
        const uint32_t resends = Delta(st.resends, prev.resends);
        const uint32_t fast = Delta(st.fastResends, prev.fastResends);
        totals_.outBytesPerS += out;
        totals_.inBytesPerS += in;
        totals_.resends += resends;
        totals_.maxQueued = std::max(totals_.maxQueued, st.reliableQueued);
        log_.push_back(Fmt("net: %s (%u%s): rtt %.0f ms, loss %.1f %%, out %s (%.0f pkt/s), in %s (%.0f pkt/s), "
                           "resends +%u (fast %u), reliable queued %u",
                           p.name.c_str(), p.id, p.id == kHostPlayer ? ", host" : "", st.rttMs, st.loss * 100.0,
                           Kb(out).c_str(), pout, Kb(in).c_str(), pin, resends, fast, st.reliableQueued));
        overlay_.push_back(Fmt("  net %s: out %s, in %s, loss %.0f%%, queued %u", p.name.c_str(), Kb(out).c_str(),
                               Kb(in).c_str(), st.loss * 100.0, st.reliableQueued));
    }
    peers_ = std::move(next);

    // ---- per message id (session) ----
    struct Row {
        uint16_t id;
        double out, in, relay, outN, inN;
        uint64_t failed;
        double ageMs;  // mean age at arrival over the interval (-1 none stamped)
    };
    std::vector<Row> rows;
    for (const auto& [id, t] : traffic) {
        MsgTraffic prev;
        if (auto it = traffic_.find(id); it != traffic_.end()) prev = it->second;
        Row r{id,
              static_cast<double>(Delta(t.sentBytes, prev.sentBytes)) / dt,
              static_cast<double>(Delta(t.recvBytes, prev.recvBytes)) / dt,
              static_cast<double>(Delta(t.relayedBytes, prev.relayedBytes)) / dt,
              static_cast<double>(Delta(t.sentMsgs, prev.sentMsgs)) / dt,
              static_cast<double>(Delta(t.recvMsgs, prev.recvMsgs)) / dt,
              Delta(t.sendFailed, prev.sendFailed), -1};
        const uint64_t aged = Delta(t.agedMsgs, prev.agedMsgs);
        if (aged > 0) r.ageMs = (t.ageSumMs - prev.ageSumMs) / static_cast<double>(aged);
        totals_.sendFailed += r.failed;
        if (r.out > 0 || r.in > 0 || r.relay > 0 || r.failed) rows.push_back(r);
    }
    traffic_ = traffic;
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        const double x = a.out + a.in + a.relay, y = b.out + b.in + b.relay;
        return x > y || (x == y && a.id < b.id);
    });
    std::string msgs;
    for (const Row& r : rows) {
        msgs += Fmt("%s%s out %.2f KB/s %.0f/s, in %.2f KB/s %.0f/s", msgs.empty() ? "" : "; ", Name(r.id).c_str(),
                    r.out / 1024, r.outN, r.in / 1024, r.inN);
        if (r.relay > 0) msgs += Fmt(", relayed %.2f KB/s", r.relay / 1024);
        if (r.ageMs >= 0) msgs += Fmt(", age %.1f ms", r.ageMs);
        if (r.failed) msgs += Fmt(", FAILED %llu", static_cast<unsigned long long>(r.failed));
    }
    if (!msgs.empty()) log_.push_back("net: messages: " + msgs);
    if (!session.IsHost() && session.ClockSynced())
        log_.push_back(Fmt("net: clock offset to the host %.1f ms (+-%.1f ms)", session.ClockOffsetS() * 1000,
                           session.ClockErrorMs()));
    for (size_t i = 0; i < rows.size() && i < 3; ++i)
        overlay_.push_back(Fmt("  %s: out %.1f, in %.1f KB/s", Name(rows[i].id).c_str(), rows[i].out / 1024,
                               rows[i].in / 1024));

    // ---- rate limits (host) and refused sends ----
    const RateLimitStats& rl = session.RateLimits();
    totals_.relayDropped = Delta(rl.appDropped, rate_.appDropped);
    totals_.chatDropped = Delta(rl.chatDropped, rate_.chatDropped);
    rate_ = rl;
    if (totals_.relayDropped || totals_.chatDropped || totals_.sendFailed) {
        log_.push_back(Fmt("net: DROPPED: relay rate limit +%llu messages, chat +%llu, refused sends +%llu",
                           static_cast<unsigned long long>(totals_.relayDropped),
                           static_cast<unsigned long long>(totals_.chatDropped),
                           static_cast<unsigned long long>(totals_.sendFailed)));
        overlay_.push_back(Fmt("  net DROPS: relay %llu, refused sends %llu",
                               static_cast<unsigned long long>(totals_.relayDropped),
                               static_cast<unsigned long long>(totals_.sendFailed)));
    }
    return true;
}

}  // namespace mc::net
