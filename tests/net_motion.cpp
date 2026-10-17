// net_motion: unit tests for the remote motion helpers (src/net/remote_motion.h): wire quantisation, quaternion
// nlerp, the locomotion controller against a simulated AI walker (its real walk/run/sprint speeds differ from the
// controller's first guess) and the vehicle controller against a car whose physics keeps its velocity and one whose
// teleports zero it. Prints PASS/FAIL per check; exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "net/remote_motion.h"

using namespace mc::net::motion;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-56s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Fmt(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, a, b, c, d);
    return buf;
}

void TestQuant() {
    bool ok = true;
    for (int i = -127; i <= 127; ++i) ok &= PackSnorm8(UnpackSnorm8(static_cast<int8_t>(i))) == i;
    Check(ok, "snorm8 round trip of every code");
    Check(PackSnorm8(1.0f) == 127 && PackSnorm8(-1.0f) == -127 && PackSnorm8(0) == 0, "snorm8 ends and zero");
    Check(PackSnorm8(5.0f) == 127 && PackSnorm8(-9.0f) == -127, "snorm8 clamps");
    Check(PackSnorm8(std::nanf("")) == 0 && PackUnorm8(std::nanf("")) == 0, "NaN packs as 0");
    Check(UnpackSnorm8(-128) == -1.0f, "snorm8 -128 reads as -1");
    float worst = 0;
    for (int i = 0; i <= 1000; ++i) {
        const float v = -1.0f + 2.0f * static_cast<float>(i) / 1000.0f;
        worst = std::max(worst, std::fabs(UnpackSnorm8(PackSnorm8(v)) - v));
    }
    Check(worst <= 0.5f / 127 + 1e-6f, "snorm8 error at most half a step", Fmt("%.5f", worst));
    ok = true;
    for (int i = 0; i <= 255; ++i) ok &= PackUnorm8(UnpackUnorm8(static_cast<uint8_t>(i))) == i;
    Check(ok, "unorm8 round trip of every code");
    Check(PackUnorm8(-0.5f) == 0 && PackUnorm8(2.0f) == 255, "unorm8 clamps");
}

void TestNlerp() {
    const float a[4] = {0, 0, 0, 1};
    const float b[4] = {0, 0, std::sin(0.5f), std::cos(0.5f)};  // 60 deg about z
    float o[4];
    Nlerp(a, b, 0, o);
    Check(std::fabs(o[3] - 1) < 1e-6f, "nlerp t=0 is a");
    Nlerp(a, b, 1, o);
    Check(std::fabs(o[2] - b[2]) < 1e-6f && std::fabs(o[3] - b[3]) < 1e-6f, "nlerp t=1 is b");
    const float nb[4] = {-b[0], -b[1], -b[2], -b[3]};  // same rotation, other sign
    Nlerp(a, nb, 0.5f, o);
    const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
    Check(std::fabs(len - 1) < 1e-5f && o[3] > 0.9f, "nlerp takes the short way (sign fix), unit length");
}

// A stand-in for the game's AI walk: walks toward its walk point at gaitSpeed x multiplier, accelerating over
// ~0.15 s; told to stand it walks to the given spot and stops there. Nudges and snaps move it directly.
struct Walker {
    float realSpeed[4] = {0, 1.2f, 3.6f, 6.1f};
    float x = 0, y = 0, z = 0, hx = 1, hy = 0;
    float vx = 0, vy = 0;
    Gait gait = Gait::Stand;
    float mult = 1;
    float tx = 0, ty = 0;  // walk point
    bool walking = false;

    void Apply(const LocomotionCommand& c, const Vec3& target) {
        if (c.fix == LocomotionCommand::Fix::Snap) {
            x = target.x;
            y = target.y;
            z = target.z;
            vx = vy = 0;
        } else if (c.fix == LocomotionCommand::Fix::Nudge) {
            x += c.nudge.x;
            y += c.nudge.y;
        }
        if (c.reissue) {  // a stop is a walk to the spot (the glue issues MoveVec(target, "WALK")), then it stands
            gait = c.gait == Gait::Stand ? Gait::Walk : c.gait;
            walking = true;
            tx = c.moveTo.x;
            ty = c.moveTo.y;
        }
        mult = c.gait == Gait::Stand ? 1.0f : c.speedMult;
        if (c.turn) {
            hx = c.face[0];
            hy = c.face[1];
        }
    }
    void Step(float dt) {
        float wx = 0, wy = 0;
        if (walking) {
            const float dx = tx - x, dy = ty - y, d = std::sqrt(dx * dx + dy * dy);
            if (d > 0.05f) {
                const float s = realSpeed[static_cast<int>(gait)] * mult;
                wx = dx / d * s;
                wy = dy / d * s;
            }
        }
        const float k = std::min(1.0f, dt / 0.15f);
        vx += (wx - vx) * k;
        vy += (wy - vy) * k;
        x += vx * dt;
        y += vy * dt;
        const float sp = std::sqrt(vx * vx + vy * vy);
        if (sp > 0.2f) {
            hx = vx / sp;
            hy = vy / sp;
        }
    }
};

struct Phase {
    float seconds, speed, dirDeg;
    bool sprint;
    Gait expect;
};

void TestLocomotion() {
    LocomotionController ctl;
    Walker w;
    Vec3 t;
    const float dt = 1.0f / 60;
    const std::vector<Phase> phases = {
        {1.0f, 0, 0, false, Gait::Stand},   {4.0f, 1.4f, 0, false, Gait::Walk},  {4.0f, 4.0f, 0, false, Gait::Run},
        {5.0f, 6.2f, 90, true, Gait::Sprint}, {3.0f, 3.8f, 180, false, Gait::Run}, {3.0f, 0, 180, false, Gait::Stand},
    };
    double errSum = 0, nudged = 0, walked = 0;
    int n = 0, snaps = 0;
    float errMax = 0, clock = 0, snapAtT = -1, snapErr = 0;
    bool gaitsOk = true;
    std::string gaitDetail;
    LocomotionCommand last;
    for (const Phase& ph : phases) {
        const float a = ph.dirDeg * 3.14159265f / 180;
        const float vx = std::cos(a) * ph.speed, vy = std::sin(a) * ph.speed;
        for (float s = 0; s < ph.seconds; s += dt) {
            t.x += vx * dt;
            t.y += vy * dt;
            LocomotionInput in;
            in.target = t;
            in.targetVel = {vx, vy, 0};
            in.heading[0] = std::cos(a);
            in.heading[1] = std::sin(a);
            in.sprint = ph.sprint;
            in.haveCopy = true;
            in.copy = {w.x, w.y, w.z};
            in.copyHeading[0] = w.hx;
            in.copyHeading[1] = w.hy;
            in.dt = dt;
            last = ctl.Update(in);
            clock += dt;
            if (last.fix == LocomotionCommand::Fix::Snap) {
                ++snaps;
                snapAtT = clock;
                snapErr = last.error;
            }
            if (last.fix == LocomotionCommand::Fix::Nudge)
                nudged += std::sqrt(last.nudge.x * last.nudge.x + last.nudge.y * last.nudge.y);
            const float px = w.x, py = w.y;
            w.Apply(last, t);
            w.Step(dt);
            walked += std::sqrt((w.x - px) * (w.x - px) + (w.y - py) * (w.y - py));
            if (s > 1.0f) {  // settled part of the phase
                errSum += last.error;
                errMax = std::max(errMax, last.error);
                ++n;
            }
        }
        if (last.gait != ph.expect) {
            gaitsOk = false;
            gaitDetail += Fmt("phase %.1f m/s: gait %.0f, want %.0f; ", ph.speed, static_cast<double>(last.gait),
                              static_cast<double>(ph.expect));
        }
    }
    Check(gaitsOk, "gait follows the remote speed (stand/walk/run/sprint)", gaitDetail);
    Check(snaps == 0, "no snaps on a normal walk/run/sprint/turn/stop", Fmt("%.0f snaps, last at %.2f s (%.2f m off)", snaps, snapAtT, snapErr));
    Check(errSum / n < 0.6, "average error below 0.6 m", Fmt("%.2f m, max %.2f m", errSum / n, errMax));
    Check(errMax < 2.0f, "error never above 2 m", Fmt("%.2f m", errMax));
    Check(nudged < walked * 0.08, "position nudges below 8% of the distance walked (feet do not slide)",
          Fmt("nudged %.2f m of %.1f m", nudged, walked));
    Check(std::fabs(ctl.BaseSpeed(Gait::Walk) - 1.2f) < 0.25f, "walk speed calibrated toward the real 1.2 m/s",
          Fmt("%.2f", ctl.BaseSpeed(Gait::Walk)));
    Check(std::fabs(ctl.BaseSpeed(Gait::Run) - 3.6f) < 0.5f, "run speed calibrated toward the real 3.6 m/s",
          Fmt("%.2f", ctl.BaseSpeed(Gait::Run)));
    Check(std::fabs(ctl.BaseSpeed(Gait::Sprint) - 6.1f) < 0.8f, "sprint speed calibrated toward the real 6.1 m/s",
          Fmt("%.2f", ctl.BaseSpeed(Gait::Sprint)));
    Check(last.gait == Gait::Stand && last.error < 0.3f, "stops close to the target", Fmt("%.2f m", last.error));

    // Far off: a snap. Then a standing remote with the copy stuck 0.8 m away: snapped after the stuck time.
    LocomotionInput in;
    in.target = {10, 0, 0};
    in.haveCopy = true;
    in.copy = {0, 0, 0};
    in.dt = dt;
    Check(ctl.Update(in).fix == LocomotionCommand::Fix::Snap, "10 m off: snap");
    in.copy = {9.2f, 0, 0};
    int snapAt = -1;
    for (int i = 0; i < 300 && snapAt < 0; ++i)
        if (ctl.Update(in).fix == LocomotionCommand::Fix::Snap) snapAt = i;
    Check(snapAt > 60 && snapAt < 200, "a blocked copy 0.8 m off a standing player is snapped after ~2 s",
          Fmt("after %.2f s", snapAt * dt));
    in.copy = {0, 0, 0};
    in.target = {0, 0, 4};
    Check(ctl.Update(in).fix == LocomotionCommand::Fix::Snap, "4 m height difference: snap");
    in.inAir = true;
    Check(ctl.Update(in).fix != LocomotionCommand::Fix::Snap, "the same while the remote player is in the air: no snap");

    // Standing turn is rate-limited: 180 deg needs ~0.45 s at 400 deg/s.
    LocomotionController tc;
    LocomotionInput st;
    st.haveCopy = true;
    st.dt = dt;
    st.heading[0] = -1;
    st.heading[1] = 0;
    st.copyHeading[0] = 1;
    st.copyHeading[1] = 0;
    int frames = 0;
    for (; frames < 200; ++frames) {
        const LocomotionCommand c = tc.Update(st);
        if (!c.turn) break;
        st.copyHeading[0] = c.face[0];
        st.copyHeading[1] = c.face[1];
    }
    Check(frames > 20 && frames < 40, "standing turn of 180 deg takes ~0.45 s", Fmt("%.2f s", frames * dt));

    LocomotionInput none;
    none.target = {5, 5, 0};
    none.dt = dt;
    const LocomotionCommand c = tc.Update(none);
    Check(c.fix == LocomotionCommand::Fix::None && !c.reissue, "no copy known: no fix, no walk");
}

// A car copy: its physics either keeps the velocity it had (and the governed throttle tops it up toward the target
// speed), or every teleport zeroes it.
void TestVehicle(bool physicsCarries) {
    VehicleController vc;
    const float dt = 1.0f / 60;
    Vec3 t, p, v;
    float speed = 0;
    float errMax = 0;
    int pins = 0, snaps = 0, firstPin = -1;
    for (int i = 0; i < 60 * 8; ++i) {
        speed = std::min(20.0f, speed + 6.0f * dt);  // accelerating to 20 m/s along x
        t.x += speed * dt;
        VehicleInput in;
        in.target = t;
        in.targetVel = {speed, 0, 0};
        in.haveCopy = true;
        in.copy = p;
        in.copyVel = v;
        in.power = 0.8f;
        in.dt = dt;
        const VehicleCommand c = vc.Update(in);
        if (c.fix == VehicleCommand::Fix::Snap) ++snaps;
        if (c.fix == VehicleCommand::Fix::Pin) {
            if (firstPin < 0) firstPin = i;
            ++pins;
        }
        if (i > 60) errMax = std::max(errMax, c.error);
        // apply
        if (c.fix == VehicleCommand::Fix::Pin || c.fix == VehicleCommand::Fix::Snap) {
            p = t;
            if (!physicsCarries) v = {};
        } else if (c.fix == VehicleCommand::Fix::Blend) {
            p.x += c.nudge.x;
            p.y += c.nudge.y;
            p.z += c.nudge.z;
            if (!physicsCarries) v = {};
        }
        // physics step: throttle accelerates up to the target speed
        if (physicsCarries) v.x = std::min(v.x + c.power * 10.0f * dt, v.x < speed ? speed : v.x);
        p.x += v.x * dt;
    }
    if (physicsCarries) {
        Check(pins == 0 && snaps == 0, "car carried by its physics: blended, never pinned or snapped",
              Fmt("%.0f pins, %.0f snaps", pins, snaps));
        Check(errMax < 1.0f, "car carried by its physics: error below 1 m", Fmt("%.2f m", errMax));
    } else {
        Check(firstPin > 0 && firstPin * dt < 2.5f, "car whose teleport zeroes its speed: pinned within 2.5 s",
              Fmt("after %.2f s", firstPin * dt));
        Check(vc.Pinned(), "pinned stays pinned");
    }
}

void TestVehicleRules() {
    VehicleController vc;
    VehicleInput in;
    in.haveCopy = true;
    in.target = {10, 0, 0};
    in.dt = 1.0f / 60;
    Check(vc.Update(in).fix == VehicleCommand::Fix::Snap, "car 10 m off: snap");
    in.target = {0.5f, 0, 0};
    in.teleported = true;
    Check(vc.Update(in).fix == VehicleCommand::Fix::Snap, "teleported target: snap");
    in.teleported = false;
    in.target = {0, 0, 0};
    Check(vc.Update(in).fix == VehicleCommand::Fix::None, "on target and still: nothing applied (no SetPos)");
    in.targetVel = {5, 0, 0};
    in.copyVel = {8, 0, 0};
    in.power = 1;
    Check(vc.Update(in).power == 0, "copy faster than the target: no power");
    in.copyVel = {5, 0, 0};
    Check(vc.Update(in).power == 1, "copy as fast as the target: the driver's power");
    in.copyVel = {30, 0, 0};
    vc.Update(in);
    Check(vc.Pinned(), "copy running off (25 m/s over): pinned");
    vc.Reset();
    Check(!vc.Pinned(), "Reset: not pinned");
    in.copyVel = {5, 0, 0};
    in.target = {2, 0, 0};
    int frames = 0;
    for (; frames < 200 && !vc.Pinned(); ++frames) vc.Update(in);
    Check(frames > 80 && frames < 100, "copy 2 m off for 1.5 s although blended: pinned", Fmt("after %.2f s", frames / 60.0));
}

}  // namespace

int main() {
    TestQuant();
    TestNlerp();
    TestLocomotion();
    TestVehicle(true);
    TestVehicle(false);
    TestVehicleRules();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
