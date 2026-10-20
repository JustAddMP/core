#include "net/remote_motion.h"

#include "net/correction.h"

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
    sinceGait_ = 0;
    stuckT_ = blockedT_ = 0;
    winValid_ = false;
    haveLast_ = false;
    haveVel_ = false;
    copyVel_[0] = copyVel_[1] = 0;
    haveTv_ = false;
    tAcc_[0] = tAcc_[1] = 0;
    lastNudge_ = {};
    lastMeasured_ = -1;
}

void LocomotionController::Applied(const Vec3& nudge) {
    if (haveLast_) lastNudge_ = nudge;
}

LocomotionCommand LocomotionController::Update(const LocomotionInput& in) {
    LocomotionCommand c;
    const float dt = std::clamp(in.dt, 0.0f, 0.5f);
    sinceIssue_ += dt;
    sinceChange_ += dt;
    sinceGait_ += dt;
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
        lastMeasured_ = measured;
        measuredMult_ = std::max(mult_, 0.1f);
    }
    c.measured = lastMeasured_;
    // The copy's walking velocity (per tick, smoothed; nudges are not walking): predicts where it will be when the
    // command takes effect (responseLag).
    if (haveLast_ && dt > 1e-4f) {
        const float vx = (in.copy.x - last_.x - lastNudge_.x) / dt, vy = (in.copy.y - last_.y - lastNudge_.y) / dt;
        const float a = haveVel_ ? std::min(1.0f, dt / 0.1f) : 1.0f;
        copyVel_[0] += (vx - copyVel_[0]) * a;
        copyVel_[1] += (vy - copyVel_[1]) * a;
        haveVel_ = true;
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
        haveVel_ = false;
        copyVel_[0] = copyVel_[1] = 0;
        issued_ = false;  // walk again from the new spot
        sinceIssue_ = 1e9f;
    } else {
        // the blend rule of net/correction.h: the excess beyond the dead zone, at a speed hidden in the walk
        const float step = correction::BlendStep(err, t_.nudgeStart, t_.nudgeGain, t_.nudgeSpeedShare,
                                                 t_.nudgeRestSpeed, tspeed, dt);
        if (step > 0) {
            const float k = step / err;
            c.fix = LocomotionCommand::Fix::Nudge;
            c.nudge = {ex * k, ey * k, 0};
            lastNudge_ = c.nudge;
        }
    }

    // Calibration: a window measured while gait and multiplier were steady tells the gait's speed at multiplier 1.
    if (measured > 0.3f && gait_ != Gait::Stand && sinceChange_ >= t_.calibSettle && c.fix != LocomotionCommand::Fix::Snap) {
        const int g = GaitIndex(gait_);
        const float init = g == 1 ? t_.walkSpeed : g == 2 ? t_.runSpeed : t_.sprintSpeed;
        const float est = std::clamp(measured / std::max(mult_, 0.1f), init * 0.5f, init * 2.0f);
        // far off the first guess (a game whose AI sprint is much slower): learn faster
        const float alpha = std::fabs(est - base_[g]) > base_[g] * 0.3f ? std::max(t_.calibAlpha, 0.6f) : t_.calibAlpha;
        base_[g] += (est - base_[g]) * alpha;
    }

    // Wanted velocity: the target's plus a bounded share of the error - the error expected when this command takes
    // effect (both moved on by their velocities for responseLag), so a turn or a stop is answered before the copy
    // overshoots instead of after.
    // The target's acceleration (from its velocity, smoothed, bounded) says where a start / stop / turn is going:
    // the copy is asked for the velocity the target will have then.
    if (haveTv_ && dt > 1e-4f) {
        const float ax = (in.targetVel.x - lastTv_[0]) / dt, ay = (in.targetVel.y - lastTv_[1]) / dt;
        const float k = std::min(1.0f, dt / 0.1f);
        tAcc_[0] += (ax - tAcc_[0]) * k;
        tAcc_[1] += (ay - tAcc_[1]) * k;
        const float al = Len2(tAcc_[0], tAcc_[1]);
        if (al > t_.maxAccel) {
            tAcc_[0] *= t_.maxAccel / al;
            tAcc_[1] *= t_.maxAccel / al;
        }
    }
    lastTv_[0] = in.targetVel.x;
    lastTv_[1] = in.targetVel.y;
    haveTv_ = true;
    const float lag = haveVel_ ? t_.responseLag : 0.0f;
    float ftx = in.targetVel.x + tAcc_[0] * lag, fty = in.targetVel.y + tAcc_[1] * lag;
    // a decelerating target does not reverse
    if (ftx * in.targetVel.x + fty * in.targetVel.y < 0) ftx = fty = 0;
    const float pex = ex + (in.targetVel.x - copyVel_[0]) * lag + 0.5f * tAcc_[0] * lag * lag;
    const float pey = ey + (in.targetVel.y - copyVel_[1]) * lag + 0.5f * tAcc_[1] * lag * lag;
    float cx = pex * t_.corrGain, cy = pey * t_.corrGain;
    const float cl = Len2(cx, cy);
    const float corrCap = std::min(t_.maxCorrSpeed, std::max(t_.corrRestSpeed, t_.corrSpeedShare * tspeed));
    if (cl > corrCap) {
        cx *= corrCap / cl;
        cy *= corrCap / cl;
    }
    const float wx = ftx + cx, wy = fty + cy;
    const float want = Len2(wx, wy);

    Gait g = gait_;
    if (in.gaitHint >= 0) {
        // The sender's own gait: used as is (the multiplier absorbs the error). A standing sender: the copy stands
        // once it is within the stop distance (and stays standing up to the start distance), else walks there.
        const int h = std::min<int>(in.gaitHint, 3);
        if (h == 0) {
            const float keep = gait_ == Gait::Stand ? t_.startDist : t_.stopDist;
            g = err <= keep ? Gait::Stand : Gait::Walk;
        } else {
            g = static_cast<Gait>(h);
        }
    } else if (g == Gait::Stand) {
        // Unknown gait: chosen from the wanted speed, with hysteresis.
        if (tspeed >= t_.standSpeed || err > t_.startDist) g = Gait::Walk;
    } else if (tspeed < t_.standSpeed && err < t_.stopDist) {
        g = Gait::Stand;
    }
    if (g != Gait::Stand && in.gaitHint < 0) {
        if (g == Gait::Walk && want > t_.walkToRun) g = Gait::Run;
        if (g == Gait::Run && want < t_.runToWalk) g = Gait::Walk;
        const bool sprintWanted = want > t_.runToSprint || (in.sprint && want > t_.sprintHintSpeed);
        if (g == Gait::Run && sprintWanted) g = Gait::Sprint;
        if (g == Gait::Sprint && !sprintWanted && want < t_.sprintToRun) g = Gait::Run;
        if (g == Gait::Walk && want > t_.walkToRun * 2) g = Gait::Run;  // from standing straight into a run
    }

    float mult = 1;
    if (g != Gait::Stand) mult = std::clamp(want / std::max(base_[GaitIndex(g)], 0.1f), t_.minMult, t_.maxMult);
    // His game says he stands but the copy is not there yet: it walks there at a normal pace (not creeping in at the
    // speed of the shrinking error).
    if (in.gaitHint == 0 && g == Gait::Walk) mult = std::max(mult, 1.0f);
    if (g != gait_ || std::fabs(mult - mult_) > 0.05f) sinceChange_ = 0;
    c.want = want;
    // The fastest multiplier still too slow (the game's AI sprint is slower than a player's): carry the shortfall
    // along the wanted direction as a nudge, so the copy does not fall behind until it snaps.
    // Not while the copy is still getting up to speed after a gait change (that is not a shortfall).
    if (g != Gait::Stand && g == gait_ && sinceGait_ >= t_.calibSettle && c.fix != LocomotionCommand::Fix::Snap &&
        want > 1e-3f) {
        // what the copy reaches: the calibrated speed, or what it was last measured doing if that is less (the
        // calibration is still catching up with a slow gait)
        float reach = base_[GaitIndex(g)] * mult;
        if (lastMeasured_ >= 0 && g == gait_) reach = std::min(reach, lastMeasured_ / measuredMult_ * mult);
        const float deficit = want - reach;
        // only while the copy is behind (a copy on or ahead of its target has nothing to make up)
        const float behind = (ex * wx + ey * wy) / want;
        if (deficit > t_.carryMin && behind > 0.1f) {
            const float k = deficit * dt / want;
            c.nudge.x += wx * k;
            c.nudge.y += wy * k;
            c.fix = LocomotionCommand::Fix::Nudge;
            c.carry = deficit;
            lastNudge_ = c.nudge;
        }
    }

    // A target braking to a stop (his game no longer says run / sprint): walk to where he will stand - the game's walk
    // to a point arrives there - instead of a point ahead, so the copy does not overshoot by its response lag.
    bool stopping = false;
    Vec3 stopAt;
    if (tspeed > 0.1f && in.gaitHint <= 1) {
        const float apar = (tAcc_[0] * in.targetVel.x + tAcc_[1] * in.targetVel.y) / tspeed;
        if (apar < -t_.brakeAccel && tspeed / -apar < t_.brakeHorizon) {
            const float d = tspeed * tspeed / (2 * -apar);
            stopAt = {in.target.x + in.targetVel.x / tspeed * d, in.target.y + in.targetVel.y / tspeed * d, in.target.z};
            stopping = true;
        }
    }
    c.stopping = stopping;

    // Walk point and when to re-aim.
    bool reissue = false;
    if (g == Gait::Stand) {
        if (gait_ != Gait::Stand || !issued_) reissue = true;  // stop where we are
        c.moveTo = in.target;
    } else {
        const float dirx = want > 1e-3f ? wx / want : in.heading[0], diry = want > 1e-3f ? wy / want : in.heading[1];
        const float ahead = std::max(t_.lookMin, want * t_.lookAhead);
        // Approaching a standing player: walk to his spot (a point beyond it would overshoot and turn back).
        // (Not when his game says he moves: at a start his speed is still low, and a walk to the copy's own spot
        // would end at once.)
        c.moveTo = stopping                                       ? stopAt
                   : tspeed < t_.standSpeed && in.gaitHint <= 0 ? in.target
                                                                : Vec3{in.copy.x + dirx * ahead, in.copy.y + diry * ahead, in.target.z};
        const float dot = issueDir_[0] * dirx + issueDir_[1] * diry;
        if (!issued_ || g != gait_ || stopping != stopping_ || dot < std::cos(t_.reissueAngleDeg * kPi / 180) ||
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
    if (g != gait_) sinceGait_ = 0;
    stopping_ = stopping;
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
    pinnedT_ = retryAfter_ = 0;
    freeT_ = 0;
    level_ = 0;
    why_ = VehicleCommand::Why::None;
    pins_ = retries_ = 0;
}

VehicleCommand VehicleController::Update(const VehicleInput& in) {
    VehicleCommand c;
    const float dt = std::clamp(in.dt, 0.0f, 0.5f);
    c.power = std::clamp(in.power, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) c.rot[i] = in.targetRot[i];
    c.vel = in.targetVel;
    c.why = why_;
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

    c.tspeed = tspeed;
    c.cspeed = cspeed;
    float dot = 0;
    for (int i = 0; i < 4; ++i) dot += in.copyRot[i] * in.targetRot[i];
    c.rotErrDeg = 2 * std::acos(std::min(1.0f, std::fabs(dot))) * 180 / kPi;

    // A copy that kept up unpinned for retryReset: the next pin starts the doubling again.
    if (!pinned_) {
        freeT_ += dt;
        if (freeT_ >= t_.retryReset) level_ = 0;
    }
    // A pinned copy gets its physics back after a while (what kept it from moving may be gone).
    if (pinned_) {
        pinnedT_ += dt;
        if (pinnedT_ >= retryAfter_) {
            pinned_ = false;
            slowT_ = lagT_ = 0;
            c.retried = true;
            freeT_ = 0;
            ++retries_;
        }
    }
    // Does the copy's own physics carry it? (If the game's teleport zeroes its velocity, it never will.)
    if (!pinned_ && !c.retried) {
        VehicleCommand::Why why = VehicleCommand::Why::None;
        if (cspeed > tspeed + t_.runawaySpeed) {
            why = VehicleCommand::Why::Runaway;
        } else if (tspeed > t_.pinSpeed && cspeed < tspeed * t_.pinRatio) {
            slowT_ += dt;
            if (slowT_ > t_.pinTime) why = VehicleCommand::Why::Slow;
        } else {
            slowT_ = std::max(0.0f, slowT_ - dt);
        }
        lagT_ = (err > t_.lagDist && err <= t_.snapDist && !in.teleported) ? lagT_ + dt : 0;
        if (why == VehicleCommand::Why::None && lagT_ > t_.lagTime) why = VehicleCommand::Why::Lag;
        if (why != VehicleCommand::Why::None) {
            pinned_ = true;
            why_ = why;
            pinnedT_ = 0;
            retryAfter_ = level_ == 0 ? t_.retryFirst : std::min(t_.retryMax, retryAfter_ * 2);
            ++level_;
            ++pins_;
            c.justPinned = true;
        }
    }
    c.pinned = pinned_;
    c.why = why_;
    if (pinned_) {
        c.pinnedFor = pinnedT_;
        c.retryIn = std::max(0.0f, retryAfter_ - pinnedT_);
    }
    // Wanted velocity: the target's plus a bounded share of the error (pinned or snapped: the target's).
    if (!pinned_ && !in.teleported && err <= t_.snapDist) {
        float vx = ex * t_.velGain, vy = ey * t_.velGain, vz = ez * t_.velGain;
        const float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (vl > t_.maxVelCorr) {
            vx *= t_.maxVelCorr / vl;
            vy *= t_.maxVelCorr / vl;
            vz *= t_.maxVelCorr / vl;
        }
        c.vel = {in.targetVel.x + vx, in.targetVel.y + vy, in.targetVel.z + vz};
    }
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
