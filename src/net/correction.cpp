#include "net/correction.h"

#include <algorithm>
#include <cmath>

namespace mc::net::correction {

// Walker (a puppet walked by the game's AI locomotion): the gait carries it, corrections stay small and slow so the
// feet do not slide; 0.3 m is below what is noticeable next to a moving character.
Tuning Tuning::ForFoot() { return Tuning{}; }

// Car copy carried by its own physics: cars are big and fast, so a 0.5 m error is invisible but a sliding body is
// not; a parked car may creep a little faster than a walker (0.3 m/s); snap at 4 m (the vehicle controller's value).
Tuning Tuning::ForCar() {
    Tuning t;
    t.deadZone = 0.5f;
    t.gain = 3.0f;
    t.speedShare = 0.3f;
    t.restSpeed = 0.3f;
    t.snapDist = 4.0f;
    t.snapHeight = 3.0f;
    t.stuckDist = 1.0f;
    t.stuckTime = 2.0f;
    t.groundOnly = false;
    return t;
}

// NPC (an AI human the host simulates): looked at less closely than a co-op partner, and its AI may take a slightly
// different route around obstacles: a wider dead zone and later snaps, so its own walk is disturbed less.
Tuning Tuning::ForNpc() {
    Tuning t;
    t.deadZone = 0.5f;
    t.snapDist = 5.0f;
    t.stuckDist = 0.8f;
    t.stuckTime = 3.0f;
    return t;
}

// Ambient traffic: many cars, mostly seen from a distance; a snap of a passing car is more visible than a 1 m error,
// so blends start later and snaps only beyond 8 m.
Tuning Tuning::ForTraffic() {
    Tuning t;
    t.deadZone = 1.0f;
    t.gain = 2.0f;
    t.speedShare = 0.3f;
    t.restSpeed = 0.3f;
    t.snapDist = 8.0f;
    t.snapHeight = 4.0f;
    t.stuckDist = 2.0f;
    t.stuckTime = 3.0f;
    t.groundOnly = false;
    return t;
}

const char* WhyName(Result::Why w) {
    switch (w) {
        case Result::Why::Far: return "far";
        case Result::Why::Height: return "height";
        case Result::Why::Stuck: return "stuck";
        case Result::Why::Teleport: return "teleport";
        default: return "none";
    }
}

float BlendStep(float err, float deadZone, float gain, float speedShare, float restSpeed, float targetSpeed, float dt) {
    if (!(err > deadZone) || dt <= 0) return 0;
    const float excess = err - deadZone;
    const float cap = std::max(restSpeed, speedShare * targetSpeed) * dt;
    return std::min(excess * std::min(1.0f, gain * dt), cap);
}

Corrector::Corrector(const Tuning& t) : t_(t) {}

void Corrector::Reset() {
    stuckT_ = 0;
    haveLast_ = false;
}

void Corrector::ResetCounters() {
    snaps_ = blends_ = 0;
    for (auto& n : snapsWhy_) n = 0;
    blendMetres_ = 0;
}

Result Corrector::Update(const Vec3& copy, const Vec3& target, const Vec3& targetVel, float dt, bool inAir) {
    Result r;
    dt = std::clamp(dt, 0.0f, 0.5f);
    const float ex = target.x - copy.x, ey = target.y - copy.y, ez = target.z - copy.z;
    const float ez2 = t_.groundOnly ? 0.0f : ez;
    r.error = std::sqrt(ex * ex + ey * ey + ez2 * ez2);
    r.height = std::fabs(ez);
    const float tspeed = t_.groundOnly ? std::sqrt(targetVel.x * targetVel.x + targetVel.y * targetVel.y)
                                       : std::sqrt(targetVel.x * targetVel.x + targetVel.y * targetVel.y +
                                                   targetVel.z * targetVel.z);
    bool jumped = false;
    if (haveLast_) {
        const float jx = target.x - lastTarget_.x, jy = target.y - lastTarget_.y, jz = target.z - lastTarget_.z;
        jumped = jx * jx + jy * jy + jz * jz > t_.teleportDist * t_.teleportDist;
    }
    haveLast_ = true;
    lastTarget_ = target;

    if (tspeed < t_.standSpeed && r.error > t_.stuckDist)
        stuckT_ += dt;
    else
        stuckT_ = 0;

    const float snapH = inAir ? t_.snapHeight * 2 : t_.snapHeight;
    if (jumped)
        r.why = Result::Why::Teleport;
    else if (r.error > t_.snapDist)
        r.why = Result::Why::Far;
    else if (r.height > snapH)
        r.why = Result::Why::Height;
    else if (stuckT_ > t_.stuckTime)
        r.why = Result::Why::Stuck;
    if (r.why != Result::Why::None) {
        r.kind = Result::Kind::Snap;
        r.delta = {ex, ey, ez};
        stuckT_ = 0;
        ++snaps_;
        ++snapsWhy_[static_cast<int>(r.why)];
        return r;
    }
    r.limit = std::max(t_.restSpeed, t_.speedShare * tspeed);
    const float step = BlendStep(r.error, t_.deadZone, t_.gain, t_.speedShare, t_.restSpeed, tspeed, dt);
    if (step > 0) {
        const float k = step / r.error;
        r.kind = Result::Kind::Blend;
        r.delta = {ex * k, ey * k, ez2 * k};
        ++blends_;
        blendMetres_ += step;
    }
    return r;
}

}  // namespace mc::net::correction
