// net: replica - the shared interpolation layer for everything a game shows of another player's game (docs/replication.md).
//
// A remote entity (a player, a car copy, a traffic car, an NPC, a pedestrian) arrives as timestamped snapshots on its
// sender's clock. SnapshotTrack keeps one entity's recent snapshots and answers "where was it at render time t":
// Hermite between the two snapshots around t (positions on a curve through both with their velocities), velocity
// lerped, rotation nlerped, discrete bits from the nearer snapshot; a jump beyond teleportDist is not interpolated
// across; past the newest snapshot a bounded extrapolation, then held. Present() adds the "now" lead: the render time
// stands the playout delay in the past, so the state is moved forward along its velocity by that delay (bounded).
//
// Stream is one sender's set of entities sharing one clock: a PlayoutClock (net/playout.h: adaptive delay measured
// from arrival jitter) plus a track per entity key. Iteration is in key order (std::map), so everything observable
// is deterministic.
//
// Pure logic: no game, transport or OS calls. Not thread-safe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "net/playout.h"
#include "net/remote_motion.h"

namespace mc::net::replica {

using motion::Vec3;

struct Sample {
    double t = 0;                       // sender clock, s (unwrapped)
    Vec3 pos, vel;                      // world m, m/s
    float rot[4] = {0, 0, 0, 1};        // quaternion x y z w (when hasRot)
    bool hasRot = false;
    uint32_t bits = 0;                  // discrete state (flags, mode, gait ...): taken from the nearer sample
};

struct TrackTuning {
    size_t capacity = 32;               // samples kept (older ones trimmed)
    float teleportDist = 20.0f;         // m between two samples: a jump, not interpolated across
    double maxExtrapolate = 0.25;       // s past the newest sample: extrapolated at most this far, then held
    double accelTime = 0.0;             // s: extrapolation follows the acceleration of the last two samples this long
                                        //    (0: velocity only, today's player and traffic behaviour)
    float maxLead = 1.5f;               // m: the most Present()'s lead may move the position
};

struct Out {
    Vec3 pos, vel;
    float rot[4] = {0, 0, 0, 1};
    bool hasRot = false;
    uint32_t bits = 0;
    bool extrapolated = false;          // render time past the newest sample (extrapolating or held)
    bool held = false;                  // ... by more than maxExtrapolate: the position no longer advances
    bool teleport = false;              // the two samples around the render time are a jump: the nearer one is shown
    double starved = 0;                 // s the render time is past the newest sample (0 = interpolating)
    // which samples were used, so a caller can blend its own payload: from (older) / to (newer) sample times and the
    // position between them (u 0..1). Extrapolated / held / before the oldest: from == to.
    double fromT = 0, toT = 0;
    float u = 0;
    bool valid = false;                 // false: the track is empty
};

class SnapshotTrack {
public:
    explicit SnapshotTrack(const TrackTuning& t = {});
    // false (and ignored) when not newer than the newest sample.
    bool Push(const Sample& s);
    void Clear();
    bool Empty() const { return size_ == 0; }
    size_t Size() const { return size_; }
    const Sample& Newest() const;       // only when !Empty()
    const Sample& Oldest() const;
    const Sample& Get(size_t i) const;  // 0 = oldest
    // The state at sender time renderTime.
    Out At(double renderTime) const;
    // At() moved forward along its velocity by `lead` seconds (the playout delay), at most maxLead metres.
    Out Present(double renderTime, double lead) const;
    const TrackTuning& Tuning() const { return t_; }

private:
    TrackTuning t_;
    std::vector<Sample> ring_;
    size_t head_ = 0, size_ = 0;  // head_: index of the oldest
};

struct StreamTuning {
    playout::PlayoutTuning playout;
    TrackTuning track;
};

// One sender's entities on one clock.
class Stream {
public:
    explicit Stream(const StreamTuning& t = {});
    void Reset();  // new stream (reconnect): clock and every track forgotten

    // Once per received message: its sender time and local arrival time (feeds the playout clock).
    void Observe(double senderTime, double localTime);
    // One entity's sample from that message. False when not newer than its newest (ignored).
    bool Push(uint32_t key, const Sample& s);
    // Once per frame before RenderTime / Present.
    void Update(double localNow);
    double RenderTime(double localNow) const { return clock_.RenderTime(localNow); }
    double Delay() const { return clock_.Delay(); }
    const playout::PlayoutClock& Clock() const { return clock_; }

    // Convenience: track(key).At / Present at this stream's render time and lead. valid=false for an unknown key.
    Out At(uint32_t key, double localNow) const;
    Out Present(uint32_t key, double localNow) const;

    const SnapshotTrack* Track(uint32_t key) const;
    SnapshotTrack* Track(uint32_t key);
    bool Remove(uint32_t key);
    // Drop tracks whose last sample arrived (local clock) before olderThanLocal; returns their keys in key order.
    std::vector<uint32_t> Forget(double olderThanLocal);
    // Every key in ascending order.
    std::vector<uint32_t> Keys() const;
    size_t Size() const { return tracks_.size(); }
    // Local time the last sample of key arrived (-1: unknown key).
    double LastArrival(uint32_t key) const;
    uint32_t Dropped() const { return dropped_; }  // samples ignored as not newer

private:
    struct Entry {
        SnapshotTrack track;
        double lastLocal = 0;
    };
    StreamTuning t_;
    playout::PlayoutClock clock_;
    std::map<uint32_t, Entry> tracks_;
    double lastLocal_ = 0;
    uint32_t dropped_ = 0;
};

}  // namespace mc::net::replica
