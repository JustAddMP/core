// net: remote motion. For a game layer that shows another player's character or vehicle as a copy the local game
// simulates (an AI human walking with the game's own locomotion, a car with its own physics), these turn the remote
// target (interpolated position, velocity, heading) and the copy's measured state into commands for the game:
//
//   LocomotionController  gait (stand/walk/run/sprint), a speed multiplier and a point to walk toward, so the game's
//                         walk animation runs at the remote player's speed and direction (no sliding feet), and a
//                         position fix that stays small: a gentle nudge only beyond a dead zone, a snap when far off.
//                         The multiplier is calibrated at run time from the copy's measured speed per gait, so it
//                         does not need the game's nominal walk/run speeds. When even the fastest multiplier cannot
//                         reach the wanted speed (a game whose AI sprint is slower than a player's), the shortfall is
//                         carried as a small per-tick nudge, so the copy keeps up instead of falling behind and
//                         snapping.
//   VehicleController     how to keep a car copy on its target: let its own physics carry it (the remote driver's
//                         controls are applied by the caller) with a blended correction plus a wanted velocity for
//                         the copy (a game whose position setter zeroes the body's speed gives it back right after),
//                         or pin it to the target every tick when its physics does not keep up (detected
//                         automatically, retried after a while); a power governor so a copy never outruns its
//                         target.
//
// Plus quantisation helpers for control values on the wire. Pure math: no game, transport or OS calls; the same
// calls in the same order give the same results. One controller per remote player / car; not thread-safe.
#pragma once
#include <cstdint>

namespace mc::net::motion {

// ---- wire quantisation --------------------------------------------------------------------------------------------
// [-1, 1] <-> int8 (x127, rounded, clamped; NaN -> 0) and [0, 1] <-> uint8 (x255).
int8_t PackSnorm8(float v);
float UnpackSnorm8(int8_t v);
uint8_t PackUnorm8(float v);
float UnpackUnorm8(uint8_t v);

struct Vec3 {
    float x = 0, y = 0, z = 0;  // world metres, z up
};

// ---- character locomotion -----------------------------------------------------------------------------------------
enum class Gait : uint8_t { Stand = 0, Walk = 1, Run = 2, Sprint = 3 };

struct LocomotionTuning {
    // nominal ground speed of each gait at multiplier 1 (first guess; calibrated from the copy's measured speed)
    float walkSpeed = 1.5f, runSpeed = 4.0f, sprintSpeed = 6.5f;
    float minMult = 0.6f, maxMult = 1.8f;  // speed multiplier range
    // gait thresholds on the wanted speed (m/s), with hysteresis
    float walkToRun = 2.5f, runToWalk = 2.1f, runToSprint = 5.3f, sprintToRun = 4.7f;
    float sprintHintSpeed = 3.5f;  // the remote player sprints (his sprint flag): sprint from this speed on
    // standing: below this target speed and within startDist the copy stands; it starts again beyond startDist
    float standSpeed = 0.35f, stopDist = 0.25f, startDist = 0.6f;
    float corrGain = 1.2f;         // 1/s: wanted velocity = target velocity + corrGain x position error
    float maxCorrSpeed = 2.5f;     // m/s: at most this much of the wanted velocity comes from the error
    float lookAhead = 0.6f;        // s: the walk point is this far ahead along the wanted velocity
    float lookMin = 2.0f;          // m: and at least this far
    float reissueAngleDeg = 12;    // re-aim the walk when the wanted direction turned more than this
    float reissueInterval = 0.35f; // s: and at least this often while moving
    float nudgeStart = 0.9f;       // m: errors beyond this are nudged away (only the excess)
    float nudgeGain = 2.0f;        // 1/s
    float snapDist = 3.5f;         // m: put the copy at the target
    float snapHeight = 3.0f;       // m: vertical error that snaps (doubled while the remote player is in the air)
    float stuckDist = 0.5f;        // m: the remote player stands but the copy is this far off ...
    float stuckTime = 2.0f;        // s: ... for this long: snapped (the walk is blocked)
    float blockedTime = 1.5f;      // s: moving but the copy did not advance for this long while > nudgeStart off
    float turnRateDeg = 400;       // deg/s: standing, the copy turns to the remote heading at most this fast
    float turnMinDeg = 8;          // standing: smaller heading differences are left alone
    float calibWindow = 0.25f;     // s: speed measurement window
    float calibAlpha = 0.25f;      // EMA weight of one window
    float calibSettle = 0.6f;      // s: gait and multiplier unchanged this long before a window counts
    float carryMin = 0.1f;         // m/s: a smaller shortfall (fastest multiplier still too slow) is not carried
};

struct LocomotionInput {
    Vec3 target;               // where the remote player is now
    Vec3 targetVel;            // his velocity (z ignored)
    float heading[2] = {0, 1}; // his facing, unit vector in the ground plane
    bool sprint = false;       // his sprint flag
    bool inAir = false;        // jumping / falling
    bool haveCopy = false;     // copy position below is known (false: just spawned, hidden, in a car)
    Vec3 copy;                 // the copy's position (as reported after the previous command was applied)
    float copyHeading[2] = {0, 1};
    float dt = 0;              // s since the previous Update
};

struct LocomotionCommand {
    enum class Fix : uint8_t { None, Nudge, Snap };
    Gait gait = Gait::Stand;
    float speedMult = 1;
    bool reissue = false;  // (re)issue the walk now: toward moveTo with `gait`, or stop (gait Stand)
    Vec3 moveTo;
    Fix fix = Fix::None;
    Vec3 nudge;            // Fix::Nudge: add to the copy's current position
    bool turn = false;     // standing: face `face` now
    float face[2] = {0, 1};
    float error = 0;       // horizontal distance copy -> target (m)
    // for traces / the overlay
    float want = 0;        // wanted ground speed (m/s): target speed plus the bounded error correction
    float measured = -1;   // the copy's speed measured over the last window (m/s), -1 none yet
    float carry = 0;       // m/s of the wanted speed carried by nudging (the gait could not reach it)
};

class LocomotionController {
public:
    explicit LocomotionController(const LocomotionTuning& t = {});
    LocomotionCommand Update(const LocomotionInput& in);
    void Reset();  // copy respawned / hidden: forget the measurement window and the walk state (keeps calibration)
    float BaseSpeed(Gait g) const;  // calibrated nominal speed of a gait (m/s)
    Gait CurrentGait() const { return gait_; }
    float CurrentMult() const { return mult_; }

private:
    LocomotionTuning t_;
    float base_[4] = {0, 0, 0, 0};
    Gait gait_ = Gait::Stand;
    float mult_ = 1;
    bool issued_ = false;
    float issueDir_[2] = {0, 0};
    float sinceIssue_ = 1e9f;
    float sinceChange_ = 0;  // gait/multiplier unchanged for
    float sinceGait_ = 0;    // gait unchanged for
    float stuckT_ = 0, blockedT_ = 0;
    // measurement window
    bool winValid_ = false;
    Vec3 winStart_;
    float winT_ = 0;
    Vec3 winNudge_;
    Vec3 lastNudge_;
    float lastMeasured_ = -1;
    float measuredMult_ = 1;  // the multiplier lastMeasured_ was walked with
    bool haveLast_ = false;
    Vec3 last_;
};

// ---- vehicles ------------------------------------------------------------------------------------------------------
struct VehicleTuning {
    float snapDist = 4.0f;      // m: put the copy at the target
    float posGain = 8.0f;       // 1/s: blended position correction
    float rotGain = 8.0f;       // 1/s: blended rotation correction
    float pinSpeed = 3.0f;      // m/s: above this target speed the copy's physics must keep up ...
    float pinRatio = 0.35f;     // ... at least this fraction of it ...
    float pinTime = 1.0f;       // s: ... else (for this long) the copy is pinned to its target from then on
    float runawaySpeed = 8.0f;  // m/s over the target speed: pinned too (its physics ran off)
    float overspeed = 1.5f;     // m/s over the target speed: no power
    float lagDist = 1.5f;       // m: blended but still this far off ...
    float lagTime = 1.5f;       // s: ... for this long: pinned (its physics cannot follow)
    float minMove = 0.002f;     // m: a smaller blended correction (and rotation step) is not applied at all
    float velGain = 1.5f;       // 1/s: wanted copy velocity = target velocity + velGain x position error ...
    float maxVelCorr = 4.0f;    // m/s: ... at most this much of it from the error
    float retryFirst = 5.0f;    // s: a pinned copy gets its physics back after this long (what kept it from moving
    float retryMax = 40.0f;     //    may be gone: engine started, other ground); doubled after each pin, up to this
};

struct VehicleInput {
    Vec3 target, targetVel;
    float targetRot[4] = {0, 0, 0, 1};  // quaternion x y z w
    bool haveCopy = false;              // copy state below is known
    Vec3 copy, copyVel;
    float copyRot[4] = {0, 0, 0, 1};
    float power = 0;                    // remote driver's throttle 0..1
    bool teleported = false;            // the target jumped (do not blend across it)
    float dt = 0;
};

struct VehicleCommand {
    enum class Fix : uint8_t { None, Blend, Pin, Snap };
    enum class Why : uint8_t { None, Slow, Runaway, Lag };  // why the copy was (last) pinned
    Fix fix = Fix::None;
    Vec3 nudge;                   // Fix::Blend: add to the copy's current position
    float rot[4] = {0, 0, 0, 1};  // Fix::Blend/Pin/Snap: rotation to set
    float power = 0;              // throttle to apply on the copy (governed)
    bool pinned = false;          // this copy is pinned to its target every tick
    float error = 0;              // m
    Vec3 vel;                     // wanted copy velocity (give it to the copy after a Blend / Pin / Snap moved it)
    // for traces / the log
    Why why = Why::None;
    bool justPinned = false;      // pinned in this update
    bool retried = false;         // a pinned copy got its physics back in this update
    float tspeed = 0, cspeed = 0; // target / copy speed (m/s)
    float rotErrDeg = 0;          // angle between copy and target rotation
};

class VehicleController {
public:
    explicit VehicleController(const VehicleTuning& t = {});
    VehicleCommand Update(const VehicleInput& in);
    void Reset();  // another car / no longer driven: back to physics-carried, not pinned
    bool Pinned() const { return pinned_; }
    int Pins() const { return pins_; }  // times pinned since construction / Reset
    int Retries() const { return retries_; }

private:
    VehicleTuning t_;
    bool pinned_ = false;
    float slowT_ = 0, lagT_ = 0;
    float pinnedT_ = 0, retryAfter_ = 0;
    VehicleCommand::Why why_ = VehicleCommand::Why::None;
    int pins_ = 0, retries_ = 0;
};

// Normalised lerp of two quaternions (x y z w) with the sign fix; t clamped to [0, 1].
void Nlerp(const float a[4], const float b[4], float t, float out[4]);

}  // namespace mc::net::motion
