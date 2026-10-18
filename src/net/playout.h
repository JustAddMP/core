// net: playout timing for remote state streams (docs/protocol.md "Latency"). A game that shows another player's
// state from timestamped snapshots renders it a little in the past so it can interpolate between two snapshots. How
// far in the past is a trade: too little and the next snapshot is not there yet (extrapolation, corrections), too much
// and everything lags. PlayoutClock measures it per sender instead of using a fixed delay:
//
//   offset   local clock - sender clock + the smallest one-way delay: the minimum of (arrival - stamp) over the last
//            few seconds (follows clock drift and route changes; needs no clock sync)
//   lateness how much later than that minimum each snapshot arrived (network jitter, both games' frame quantisation)
//   delay    lateness percentile (95 %) + the sender's snapshot interval + a small margin, clamped; reached smoothly
//            (the render clock runs a little slower or faster, never jumps) so motion never stutters
//
// RenderTime(now) is the sender-clock time to show now. With a steady 60 Hz stream on a quiet network the delay is
// ~25-40 ms (it was a fixed 50 ms in Mafia DE 0.14); with jitter it grows to cover it, and shrinks again afterwards.
//
// Plus Hermite interpolation and bounded extrapolation helpers. Pure math, no OS calls; one clock per sender.
#pragma once
#include <array>
#include <cstdint>

#include "net/remote_motion.h"

namespace mc::net::playout {

using motion::Vec3;

struct PlayoutTuning {
    double minDelay = 0.008;     // s: never render closer to the newest snapshot than this
    double maxDelay = 0.300;     // s: never further behind than this (a worse stream extrapolates instead)
    double startDelay = 0.050;   // s: until minSamples arrived
    int minSamples = 12;
    double percentile = 0.95;    // of the lateness samples
    double margin = 0.004;       // s on top
    double intervalShare = 1.0;  // x the snapshot interval on top (time for the snapshot after the render time)
    double raiseRate = 0.25;     // s of delay gained per s when it has to grow (the render clock runs 25 % slower)
    double lowerRate = 0.03;     // s of delay lost per s when it may shrink (3 % faster)
    double lowerSlack = 0.004;   // s: shrink only when the target is this much below
    double snapDist = 0.5;       // s: a larger change (new stream, clock jump) is taken at once
    double offsetWindow = 4.0;   // s: the minimum (arrival - stamp) over this long is the offset
    double maxInterval = 0.25;   // s: longer gaps between snapshots are pauses, not the interval
};

class PlayoutClock {
public:
    explicit PlayoutClock(const PlayoutTuning& t = {});
    void Reset();  // new stream (respawn, reconnect): forget everything

    // A snapshot stamped `senderTime` (sender clock, seconds, unwrapped) arrived at `localTime` (local clock, s).
    void Observe(double senderTime, double localTime);
    // Move the render clock toward its target; once per frame before RenderTime.
    void Update(double localNow);

    bool Ready() const { return samples_ > 0; }
    // Sender-clock time to show at localNow (interpolate the snapshots around it).
    double RenderTime(double localNow) const { return localNow - total_; }
    double Offset() const { return offset_; }            // s: local - sender (incl. the smallest one-way delay)
    double Delay() const { return total_ - offset_; }    // s: current playout delay behind the newest possible
    double TargetDelay() const { return target_; }       // s: where Delay() is heading
    double Lateness() const { return lateness_; }        // s: the lateness percentile (jitter)
    double Interval() const { return interval_; }        // s: smoothed snapshot interval
    uint32_t Samples() const { return samples_; }
    // Snapshots that arrived when the render clock had already passed them (shown by extrapolation), since Reset.
    uint32_t Late() const { return late_; }
    // How far RenderTime(localNow) is past the newest snapshot `newestSenderTime` (s, 0 = interpolating).
    double Starved(double newestSenderTime, double localNow) const;

private:
    void Recompute();

    PlayoutTuning t_;
    static constexpr int kBuckets = 8;
    static constexpr int kRing = 128;
    std::array<double, kBuckets> bucketMin_{};
    std::array<int64_t, kBuckets> bucketId_{};
    std::array<double, kRing> raw_{};  // arrival - stamp of the last kRing snapshots
    int rawCount_ = 0, rawNext_ = 0;
    uint32_t samples_ = 0, late_ = 0;
    double offset_ = 0, lateness_ = 0, interval_ = 0, target_ = 0, total_ = 0;
    double lastSender_ = 0, lastUpdate_ = -1;
    bool haveTotal_ = false;
};

// Cubic Hermite between (p0, v0) at u = 0 and (p1, v1) at u = 1, dt = seconds between them; u clamped to [0, 1].
// Follows a curve through both snapshots with their velocities (no corner at each snapshot like a lerp).
Vec3 Hermite(const Vec3& p0, const Vec3& v0, const Vec3& p1, const Vec3& v1, double dt, double u);
// Past the newest snapshot: p + v t + a t^2 / 2, with t clamped to maxT and the acceleration term only for the first
// accelT seconds (a brake or a turn is followed briefly, never projected far).
Vec3 Extrapolate(const Vec3& p, const Vec3& v, const Vec3& a, double t, double maxT, double accelT = 0.1);

}  // namespace mc::net::playout
