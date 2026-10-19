// net: correction - one policy for keeping a locally simulated copy (a puppet, a car copy, an NPC, a traffic car) on
// its network target (docs/replication.md "Correction").
//
// The copy moves by its own simulation (walk animation, car physics); the network target is where it should be. The
// error between them is fixed in three tiers:
//   inside deadZone            nothing (the copy's own motion is trusted; small errors are invisible)
//   beyond it                  blend: move gain x dt of the excess, at most max(restSpeed, speedShare x target speed)
//                              x dt - a correction hidden inside the motion: a moving copy slides at most ~30 % of
//                              its speed, a standing one only very slowly
//   far / teleport / stuck     snap: beyond snapDist (or snapHeight vertically, doubled in the air), when the target
//                              itself jumped more than teleportDist since the last update, or when more than
//                              stuckDist off for stuckTime while the target stands (the copy cannot get there)
//
// Pure math, deterministic, one Corrector per copy. Not thread-safe.
#pragma once
#include <cstdint>

#include "net/remote_motion.h"

namespace mc::net::correction {

using motion::Vec3;

struct Tuning {
    float deadZone = 0.3f;    // m: errors up to this are left alone
    float gain = 2.0f;        // 1/s: share of the excess removed per second
    float speedShare = 0.35f; // max correction speed as a share of the target's speed
    float restSpeed = 0.15f;  // m/s: max correction speed when the target stands
    float snapDist = 3.5f;    // m: put the copy at the target
    float snapHeight = 3.0f;  // m: vertical error that snaps (doubled while in the air)
    float stuckDist = 0.5f;   // m: the target stands but the copy is this far off ...
    float stuckTime = 2.0f;   // s: ... for this long: snapped (it cannot get there)
    float teleportDist = 20.0f; // m: the target moved this far between two updates: snapped (do not blend across)
    float standSpeed = 0.35f; // m/s: a target slower than this stands
    bool groundOnly = true;   // errors and blends in the ground plane only (a walker: the game keeps it on the
                              // ground); false: in 3D (a car body)

    // Presets. Values and reasons in docs/replication.md "Correction presets".
    static Tuning ForFoot();
    static Tuning ForCar();
    static Tuning ForNpc();
    static Tuning ForTraffic();
};

struct Result {
    enum class Kind : uint8_t { None, Blend, Snap };
    enum class Why : uint8_t { None, Far, Height, Stuck, Teleport };
    Kind kind = Kind::None;
    Vec3 delta;      // Blend: add to the copy's position; Snap: target - copy
    float error = 0; // m (ground plane when groundOnly)
    float height = 0;// m: vertical error (absolute)
    float limit = 0; // m/s: the blend speed cap that applied
    Why why = Why::None;
};

const char* WhyName(Result::Why w);

class Corrector {
public:
    explicit Corrector(const Tuning& t = {});
    Result Update(const Vec3& copy, const Vec3& target, const Vec3& targetVel, float dt, bool inAir = false);
    void Reset();  // copy respawned: no stuck time, no previous target (keeps the counters)
    const Tuning& GetTuning() const { return t_; }
    void SetTuning(const Tuning& t) { t_ = t; }

    // counters for traces (since construction / ResetCounters)
    uint32_t Snaps() const { return snaps_; }
    uint32_t Snaps(Result::Why w) const { return snapsWhy_[static_cast<int>(w)]; }
    uint32_t Blends() const { return blends_; }
    double BlendMetres() const { return blendMetres_; }
    void ResetCounters();

private:
    Tuning t_;
    float stuckT_ = 0;
    bool haveLast_ = false;
    Vec3 lastTarget_;
    uint32_t snaps_ = 0, blends_ = 0;
    uint32_t snapsWhy_[5] = {};
    double blendMetres_ = 0;
};

// The blend step alone (shared with motion::LocomotionController's nudge): the length to move toward the target for
// an error `err` (m), or 0 inside the dead zone.
float BlendStep(float err, float deadZone, float gain, float speedShare, float restSpeed, float targetSpeed, float dt);

}  // namespace mc::net::correction
