#include "net/remote_motion.h"

#include <algorithm>
#include <cmath>

namespace mc::net::motion {
namespace {

constexpr float kPi = 3.14159265358979f;

float Len2(float x, float y) { return std::sqrt(x * x + y * y); }

int GaitIndex(Gait g) { return static_cast<int>(g); }

}  // namespace

// ---- quantisation ----------------------------------------------------------------------------------------------
int8_t PackSnorm8(float v) {
    if (!(v == v)) return 0;  // NaN
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lround(v * 127.0f));
}
float UnpackSnorm8(int8_t v) { return std::max(-1.0f, static_cast<float>(v) / 127.0f); }
uint8_t PackUnorm8(float v) {
    if (!(v == v)) return 0;
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}
float UnpackUnorm8(uint8_t v) { return static_cast<float>(v) / 255.0f; }

void Nlerp(const float a[4], const float b[4], float t, float out[4]) {
    t = std::clamp(t, 0.0f, 1.0f);
    float dot = 0;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    const float s = dot < 0 ? -1.0f : 1.0f;
    float len = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = a[i] + (b[i] * s - a[i]) * t;
        len += out[i] * out[i];
    }
    len = std::sqrt(len);
    if (len > 1e-6f) {
        for (int i = 0; i < 4; ++i) out[i] /= len;
    } else {
        for (int i = 0; i < 4; ++i) out[i] = b[i];
    }
}

// ---- locomotion ------------------------------------------------------------------------------------------------
LocomotionController::LocomotionController(const LocomotionTuning& t) : t_(t) {
    base_[0] = 0;
    base_[1] = t.walkSpeed;
    base_[2] = t.runSpeed;
    base_[3] = t.sprintSpeed;
}

float LocomotionController::BaseSpeed(Gait g) const { return base_[GaitIndex(g)]; }

void LocomotionController::Reset() {
    gait_ = Gait::Stand;
    mult_ = 1;
    issued_ = false;
    sinceIssue_ = 1e9f;
    sinceChange_ = 0;
    stuckT_ = blockedT_ = 0;
    winValid_ = false;
    haveLast_ = false;
    lastNudge_ = {};
}

LocomotionCommand LocomotionController::Update(const LocomotionInput& in) {
    LocomotionCommand c;
    const float dt = std::clamp(in.dt, 0.0f, 0.5f);
    sinceIssue_ += dt;
    sinceChange_ += dt;
    if (!in.haveCopy) {
        // Nothing to measure: walk toward the target at the remote speed, no fixes.
        Reset();
        c.moveTo = in.target;
        return c;
    }

    // Measured copy speed over a short window (the nudges we applied are not walking).
    if (haveLast_) {
        if (!winValid_) {
            winValid_ = true;
            winStart_ = last_;
            winT_ = 0;
            winNudge_ = {};
        }
        winT_ += dt;
        winNudge_.x += lastNudge_.x;
        winNudge_.y += lastNudge_.y;
    }
    float measured = -1;
    if (winValid_ && winT_ >= t_.calibWindow) {
        const float dx = in.copy.x - winStart_.x - winNudge_.x, dy = in.copy.y - winStart_.y - winNudge_.y;
        measured = Len2(dx, dy) / winT_;
        winValid_ = false;
    }
    last_ = in.copy;
    haveLast_ = true;
    lastNudge_ = {};

    const float ex = in.target.x - in.copy.x, ey = in.target.y - in.copy.y, ez = in.target.z - in.copy.z;
    const float err = Len2(ex, ey);
    c.error = err;
    const float tspeed = Len2(in.targetVel.x, in.targetVel.y);

    // Big errors: put it there.
    const float snapH = in.inAir ? t_.snapHeight * 2 : t_.snapHeight;
    bool snap = err > t_.snapDist || std::fabs(ez) > snapH;
    // Remote player stands, copy cannot get there (blocked): snap after a while.
    if (tspeed < t_.standSpeed && err > t_.stuckDist) {
        stuckT_ += dt;
    } else {
        stuckT_ = 0;
    }
    if (stuckT_ > t_.stuckTime) snap = true;
    // Walking but not advancing (an obstacle, a failed walk) while clearly off.
    if (measured >= 0) {
        if (gait_ != Gait::Stand && measured < 0.2f && err > t_.nudgeStart)
            blockedT_ += t_.calibWindow;
        else if (measured >= 0.2f)
            blockedT_ = 0;
    }
    if (blockedT_ > t_.blockedTime) snap = true;
    if (snap) {
        c.fix = LocomotionCommand::Fix::Snap;
        stuckT_ = blockedT_ = 0;
        winValid_ = false;
        haveLast_ = false;
        issued_ = false;  // walk again from the new spot
        sinceIssue_ = 1e9f;
    } else if (err > t_.nudgeStart) {
        const float k = (err - t_.nudgeStart) / err * std::min(1.0f, t_.nudgeGain * dt);
        c.fix = LocomotionCommand::Fix::Nudge;
        c.nudge = {ex * k, ey * k, 0};
        lastNudge_ = c.nudge;
    }

    // Calibration: a window measured while gait and multiplier were steady tells the gait's speed at multiplier 1.
    if (measured > 0.3f && gait_ != Gait::Stand && sinceChange_ >= t_.calibSettle && c.fix != LocomotionCommand::Fix::Snap) {
        const int g = GaitIndex(gait_);
        const float init = g == 1 ? t_.walkSpeed : g == 2 ? t_.runSpeed : t_.sprintSpeed;
        const float est = std::clamp(measured / std::max(mult_, 0.1f), init * 0.5f, init * 2.0f);
        base_[g] += (est - base_[g]) * t_.calibAlpha;
    }

    // Wanted velocity: the target's plus a bounded share of the error.
    float cx = ex * t_.corrGain, cy = ey * t_.corrGain;
    const float cl = Len2(cx, cy);
    if (cl > t_.maxCorrSpeed) {
        cx *= t_.maxCorrSpeed / cl;
        cy *= t_.maxCorrSpeed / cl;
    }
    const float wx = in.targetVel.x + cx, wy = in.targetVel.y + cy;
    const float want = Len2(wx, wy);

    // Gait with hysteresis.
    Gait g = gait_;
    if (g == Gait::Stand) {
        if (tspeed >= t_.standSpeed || err > t_.startDist) g = Gait::Walk;
    } else if (tspeed < t_.standSpeed && err < t_.stopDist) {
        g = Gait::Stand;
    }
    if (g != Gait::Stand) {
        if (g == Gait::Walk && want > t_.walkToRun) g = Gait::Run;
        if (g == Gait::Run && want < t_.runToWalk) g = Gait::Walk;
        const bool sprintWanted = want > t_.runToSprint || (in.sprint && want > t_.sprintHintSpeed);
        if (g == Gait::Run && sprintWanted) g = Gait::Sprint;
        if (g == Gait::Sprint && !sprintWanted && want < t_.sprintToRun) g = Gait::Run;
        if (g == Gait::Walk && want > t_.walkToRun * 2) g = Gait::Run;  // from standing straight into a run
    }

    float mult = 1;
    if (g != Gait::Stand) mult = std::clamp(want / std::max(base_[GaitIndex(g)], 0.1f), t_.minMult, t_.maxMult);
    if (g != gait_ || std::fabs(mult - mult_) > 0.05f) sinceChange_ = 0;

    // Walk point and when to re-aim.
    bool reissue = false;
    if (g == Gait::Stand) {
        if (gait_ != Gait::Stand || !issued_) reissue = true;  // stop where we are
        c.moveTo = in.target;
    } else {
        const float dirx = want > 1e-3f ? wx / want : in.heading[0], diry = want > 1e-3f ? wy / want : in.heading[1];
        const float ahead = std::max(t_.lookMin, want * t_.lookAhead);
        // Approaching a standing player: walk to his spot (a point beyond it would overshoot and turn back).
        c.moveTo = tspeed < t_.standSpeed ? in.target
                                          : Vec3{in.copy.x + dirx * ahead, in.copy.y + diry * ahead, in.target.z};
        const float dot = issueDir_[0] * dirx + issueDir_[1] * diry;
        if (!issued_ || g != gait_ || dot < std::cos(t_.reissueAngleDeg * kPi / 180) ||
            sinceIssue_ >= t_.reissueInterval || c.fix == LocomotionCommand::Fix::Snap) {
            reissue = true;
            issueDir_[0] = dirx;
            issueDir_[1] = diry;
        }
    }
    if (reissue) {
        issued_ = true;
        sinceIssue_ = 0;
    }
    gait_ = g;
    mult_ = mult;
    c.gait = g;
    c.speedMult = mult;
    c.reissue = reissue;

    // Standing: turn toward the remote heading, rate-limited.
    if (g == Gait::Stand) {
        const float hx = in.heading[0], hy = in.heading[1];
        const float cxh = in.copyHeading[0], cyh = in.copyHeading[1];
        const float a = std::atan2(cxh * hy - cyh * hx, cxh * hx + cyh * hy);  // signed angle copy -> target
        if (std::fabs(a) > t_.turnMinDeg * kPi / 180 || c.fix == LocomotionCommand::Fix::Snap) {
            const float step = t_.turnRateDeg * kPi / 180 * dt;
            const float r = c.fix == LocomotionCommand::Fix::Snap ? a : std::clamp(a, -step, step);
            const float base = std::atan2(cyh, cxh);
            c.turn = true;
            c.face[0] = std::cos(base + r);
            c.face[1] = std::sin(base + r);
        }
    }
    return c;
}

// ---- vehicles --------------------------------------------------------------------------------------------------
VehicleController::VehicleController(const VehicleTuning& t) : t_(t) {}

void VehicleController::Reset() {
    pinned_ = false;
    slowT_ = lagT_ = 0;
}

VehicleCommand VehicleController::Update(const VehicleInput& in) {
    VehicleCommand c;
    const float dt = std::clamp(in.dt, 0.0f, 0.5f);
    c.power = std::clamp(in.power, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) c.rot[i] = in.targetRot[i];
    if (!in.haveCopy) {
        c.fix = VehicleCommand::Fix::Pin;
        c.pinned = pinned_;
        return c;
    }
    const float ex = in.target.x - in.copy.x, ey = in.target.y - in.copy.y, ez = in.target.z - in.copy.z;
    const float err = std::sqrt(ex * ex + ey * ey + ez * ez);
    c.error = err;
    const float tspeed = std::sqrt(in.targetVel.x * in.targetVel.x + in.targetVel.y * in.targetVel.y +
                                   in.targetVel.z * in.targetVel.z);
    const float cspeed =
        std::sqrt(in.copyVel.x * in.copyVel.x + in.copyVel.y * in.copyVel.y + in.copyVel.z * in.copyVel.z);

    // Does the copy's own physics carry it? (If the game's teleport zeroes its velocity, it never will.)
    if (!pinned_) {
        if (cspeed > tspeed + t_.runawaySpeed) {
            pinned_ = true;
        } else if (tspeed > t_.pinSpeed && cspeed < tspeed * t_.pinRatio) {
            slowT_ += dt;
            if (slowT_ > t_.pinTime) pinned_ = true;
        } else {
            slowT_ = std::max(0.0f, slowT_ - dt);
        }
        lagT_ = (err > t_.lagDist && err <= t_.snapDist && !in.teleported) ? lagT_ + dt : 0;
        if (lagT_ > t_.lagTime) pinned_ = true;
    }
    c.pinned = pinned_;
    if (cspeed > tspeed + t_.overspeed) c.power = 0;

    if (in.teleported || err > t_.snapDist) {
        c.fix = VehicleCommand::Fix::Snap;
        return c;
    }
    if (pinned_) {
        c.fix = VehicleCommand::Fix::Pin;
        return c;
    }
    const float k = std::min(1.0f, t_.posGain * dt);
    c.nudge = {ex * k, ey * k, ez * k};
    Nlerp(in.copyRot, in.targetRot, std::min(1.0f, t_.rotGain * dt), c.rot);
    float rd = 0;
    for (int i = 0; i < 4; ++i) rd = std::max(rd, std::fabs(c.rot[i] - in.copyRot[i]));
    const float nl = std::sqrt(c.nudge.x * c.nudge.x + c.nudge.y * c.nudge.y + c.nudge.z * c.nudge.z);
    c.fix = (nl < t_.minMove && rd < 1e-4f) ? VehicleCommand::Fix::None : VehicleCommand::Fix::Blend;
    if (c.fix == VehicleCommand::Fix::None) c.nudge = {};
    return c;
}

}  // namespace mc::net::motion
