// net_motion: unit tests for the remote motion helpers (src/net/remote_motion.h): wire quantisation, quaternion
// nlerp, the locomotion controller against a simulated AI walker (its real walk/run/sprint speeds differ from the
// controller's first guess) and the vehicle controller against a car whose physics keeps its velocity and one whose
// teleports zero it (pinned, retried), one whose teleports zero it but gets the wanted velocity back, and an AI
// sprint slower than the player's (carried shortfall); the gait hint (the sender's own gait) against an honest AI
// walker (speed error, response lag, turn rate limit) on a recorded-like path; the vehicle pin / retry policy.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
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
// The game's AI sprint is much slower than a player's (Mafia DE: ~3.3 m/s against ~6 m/s): the copy keeps up by
// carrying the shortfall instead of falling behind until it snaps.
void TestSlowSprint() {
    LocomotionController ctl;
    Walker w;
    w.realSpeed[2] = 3.1f;
    w.realSpeed[3] = 3.3f;
    Vec3 t;
    const float dt = 1.0f / 60;
    int snaps = 0;
    float errMax = 0, carryMax = 0;
    for (int i = 0; i < 60 * 10; ++i) {
        const float speed = i < 60 ? 0.0f : 6.2f;
        t.x += speed * dt;
        LocomotionInput in;
        in.target = t;
        in.targetVel = {speed, 0, 0};
        in.heading[0] = 1;
        in.heading[1] = 0;
        in.sprint = speed > 0;
        in.haveCopy = true;
        in.copy = {w.x, w.y, w.z};
        in.copyHeading[0] = w.hx;
        in.copyHeading[1] = w.hy;
        in.dt = dt;
        const LocomotionCommand c = ctl.Update(in);
        if (c.fix == LocomotionCommand::Fix::Snap) ++snaps;
        if (i > 60 * 3) errMax = std::max(errMax, c.error);
        carryMax = std::max(carryMax, c.carry);
        w.Apply(c, t);
        w.Step(dt);
    }
    Check(snaps <= 1, "slow AI sprint (3.3 m/s) behind a 6.2 m/s player: no repeated snaps", Fmt("%.0f snaps", snaps));
    Check(errMax < 1.2f, "slow AI sprint: the copy keeps up (error below 1.2 m once settled)", Fmt("%.2f m", errMax));
    Check(carryMax > 0.5f, "slow AI sprint: the shortfall is carried", Fmt("%.2f m/s", carryMax));
}

enum class CarPhysics { Carries, ZeroedByTeleport, ZeroedButVelocityGiven };

void TestVehicle(CarPhysics mode) {
    const bool physicsCarries = mode != CarPhysics::ZeroedByTeleport;
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
        if (mode == CarPhysics::ZeroedButVelocityGiven && c.fix != VehicleCommand::Fix::None) {
            v = {};          // the game's position setter stops the body ...
            v.x = c.vel.x;   // ... and the glue gives it the wanted velocity right after
        }
        // physics step: throttle accelerates up to the target speed
        if (physicsCarries) v.x = std::min(v.x + c.power * 10.0f * dt, v.x < speed ? speed : v.x);
        p.x += v.x * dt;
    }
    const char* what = mode == CarPhysics::Carries ? "car carried by its physics"
                                                   : "car whose teleport zeroes its speed, velocity given back";
    if (physicsCarries) {
        Check(pins == 0 && snaps == 0, (std::string(what) + ": blended, never pinned or snapped").c_str(),
              Fmt("%.0f pins, %.0f snaps", pins, snaps));
        Check(errMax < 1.0f, (std::string(what) + ": error below 1 m").c_str(), Fmt("%.2f m", errMax));
    } else {
        Check(firstPin > 0 && firstPin * dt < 2.5f, "car whose teleport zeroes its speed: pinned within 2.5 s",
              Fmt("after %.2f s", firstPin * dt));
        Check(vc.Retries() >= 1 && vc.Pins() >= 2, "pinned copy retries its physics after 1.5 s and is pinned again",
              Fmt("%.0f pins, %.0f retries", vc.Pins(), vc.Retries()));
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


// ---- gait hint: an honest simulation of the game's AI walker ------------------------------------------------------
// The copy: its real gait speeds are off by up to 20 % (walk 20 % slow, run 20 % fast, sprint 15 % slow), it acts on
// a walk command 150 ms after it was given (the game's AI task start), turns at most 360 deg/s (slower while turning
// hard), changes its speed at most 10 m/s^2 and brakes to arrive at a walk point. Nudges and snaps are position sets
// the glue applies at once. The remote player: a human whose own game reports his gait (PlayerState mode, walk while
// still slowing down), whose velocity (speed and direction) changes at most 10 m/s^2. A second run: 200 ms lag and
// the opposite speed errors.
struct LaggedWalker {
    float realSpeed[4] = {0, 1.5f * 0.8f, 4.0f * 1.2f, 6.5f * 0.85f};
    float lag = 0.15f;
    float x = 0, y = 0, hx = 1, hy = 0, speed = 0;
    Gait gait = Gait::Stand;
    float mult = 1, tx = 0, ty = 0;
    bool walking = false;
    struct Pending {
        float at;
        Gait gait;
        float mult;
        bool reissue;
        float tx, ty;
    };
    std::deque<Pending> queue;
    float clock = 0;
    static constexpr float kTurn = 360.0f * 3.14159265f / 180, kAccel = 10.0f;  // m/s^2, as the player (the same game, the same body)

    void Apply(const LocomotionCommand& c, const Vec3& target) {
        if (c.fix == LocomotionCommand::Fix::Snap) {
            x = target.x;
            y = target.y;
            speed = 0;
        } else if (c.fix == LocomotionCommand::Fix::Nudge) {
            x += c.nudge.x;
            y += c.nudge.y;
        }
        queue.push_back({clock + lag, c.gait, c.speedMult, c.reissue, c.moveTo.x, c.moveTo.y});
    }
    void Step(float dt) {
        clock += dt;
        while (!queue.empty() && queue.front().at <= clock) {
            const Pending& p = queue.front();
            if (p.reissue) {
                gait = p.gait == Gait::Stand ? Gait::Walk : p.gait;
                walking = true;
                tx = p.tx;
                ty = p.ty;
            }
            mult = p.gait == Gait::Stand ? 1.0f : p.mult;
            queue.pop_front();
        }
        float want = 0, dx = tx - x, dy = ty - y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (walking && d > 0.1f) {
            want = realSpeed[static_cast<int>(gait)] * mult;
            // turn toward the walk point at most kTurn
            const float a = std::atan2(hx * dy - hy * dx, hx * dx + hy * dy);
            const float r = std::clamp(a, -kTurn * dt, kTurn * dt);
            const float c = std::cos(r), s = std::sin(r);
            const float nx = hx * c - hy * s, ny = hx * s + hy * c;
            hx = nx;
            hy = ny;
            want *= std::max(0.3f, std::cos(a));  // slows down while turning (on the spot beyond 70 deg)
            want = std::min(want, std::sqrt(2 * kAccel * d));  // a walk to a point arrives there (brakes in time)
        } else if (walking) {
            walking = false;  // arrived: stands
        }
        speed += std::clamp(want - speed, -kAccel * dt, kAccel * dt);
        x += hx * speed * dt;
        y += hy * speed * dt;
    }
};

struct Leg {
    float seconds;
    int gait;        // what his game reports: 0 idle, 1 walk, 2 run, 3 sprint
    float zigzagDeg; // heading swings +-this every zigzagPeriod
    float turnDeg;   // heading change at the start of the leg
};

double P95(std::vector<float> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(std::min<double>(static_cast<double>(v.size() - 1), std::ceil(static_cast<double>(v.size()) * 0.95) - 1))];
}

struct HintRun {
    int ticks = 0, match = 0, executed = 0, snaps = 0;
    double p95Moving = 0, p95Standing = 0;
};

HintRun RunHintPath(float lag, float walkErr, float runErr, float sprintErr) {
    LocomotionController ctl;
    LaggedWalker w;
    w.lag = lag;
    w.realSpeed[1] = 1.5f * walkErr;
    w.realSpeed[2] = 4.0f * runErr;
    w.realSpeed[3] = 6.5f * sprintErr;
    const float dt = 1.0f / 60;
    const float gaitSpeed[4] = {0, 1.6f, 4.4f, 6.3f};  // the player's own speeds
    const std::vector<Leg> legs = {
        // straight run, sprint with a gentle weave, run zig-zag, stop
        {1.5f, 0, 0, 0},   {6.0f, 2, 0, 0},    {8.0f, 3, 15, 30}, {6.0f, 2, 30, 0},  {3.0f, 0, 0, 0},
        // walk, stop-and-go (short stops, run and sprint bursts with turns)
        {4.0f, 1, 0, 90},  {1.0f, 0, 0, 0},    {2.0f, 2, 0, 0},   {1.0f, 0, 0, 0},   {2.5f, 3, 0, -60},
        {1.0f, 0, 0, 0},   {5.0f, 1, 30, 0},   {2.5f, 0, 0, 0},
        // turn around into a sprint zig-zag, run, stop
        {8.0f, 3, 20, 180}, {4.0f, 2, 0, 45},  {3.0f, 0, 0, 0},
    };
    float px = 0, py = 0, pvx = 0, pvy = 0, pspeed = 0, heading = 0;
    const float accel = 10;  // m/s^2: how fast the player's own character changes its velocity (speed and direction)
    int ticks = 0, match = 0, snaps = 0, executed = 0;
    std::deque<int> hints;  // the hint `lag` ago is what the walker can be doing now
    const size_t lagTicks = static_cast<size_t>(std::lround(lag / dt)) + 1;
    std::vector<float> moving, standing;
    float standFor = 0;
    float zig = 0;
    for (const Leg& leg : legs) {
        heading += leg.turnDeg * 3.14159265f / 180;
        for (float s = 0; s < leg.seconds; s += dt) {
            // the player
            const float want = gaitSpeed[leg.gait];
            if (leg.zigzagDeg > 0)
                zig = (std::fmod(s, 2.4f) < 1.2f ? 1.0f : -1.0f) * leg.zigzagDeg * 3.14159265f / 180;
            else
                zig = 0;
            const float hd = heading + zig;
            const float dvx = std::cos(hd) * want - pvx, dvy = std::sin(hd) * want - pvy;
            const float dv = std::sqrt(dvx * dvx + dvy * dvy), dk = dv > accel * dt ? accel * dt / dv : 1.0f;
            pvx += dvx * dk;
            pvy += dvy * dk;
            pspeed = std::sqrt(pvx * pvx + pvy * pvy);
            const float vx = pvx, vy = pvy;
            px += vx * dt;
            py += vy * dt;
            const int hint = leg.gait == 0 && pspeed > 0.35f ? 1 : leg.gait;  // still slowing down: his game says walk
            LocomotionInput in;
            in.target = {px, py, 0};
            in.targetVel = {vx, vy, 0};
            in.heading[0] = std::cos(hd);
            in.heading[1] = std::sin(hd);
            in.sprint = hint == 3;
            in.gaitHint = static_cast<int8_t>(hint);
            in.haveCopy = true;
            in.copy = {w.x, w.y, 0};
            in.copyHeading[0] = w.hx;
            in.copyHeading[1] = w.hy;
            in.dt = dt;
            const LocomotionCommand c = ctl.Update(in);
            if (c.fix == LocomotionCommand::Fix::Snap) ++snaps;
            w.Apply(c, in.target);
            w.Step(dt);
            ++ticks;
            if (static_cast<int>(c.gait) == hint) ++match;
            hints.push_back(hint);
            if (hints.size() > lagTicks) hints.pop_front();
            const int was = hints.front();
            const bool wStands = !w.walking || w.speed < 0.3f;
            if (was == 0 ? wStands : static_cast<int>(w.gait) == was) ++executed;
            standFor = pspeed < 0.05f ? standFor + dt : 0;
            if (pspeed > 0.35f && s > 0.5f) moving.push_back(c.error);
            if (standFor > 1.0f) standing.push_back(c.error);
        }
    }
    HintRun r;
    r.ticks = ticks;
    r.match = match;
    r.executed = executed;
    r.snaps = snaps;
    r.p95Moving = P95(moving);
    r.p95Standing = P95(standing);
    return r;
}

void TestGaitHint() {
    const float dt = 1.0f / 60;
    struct Variant {
        const char* name;
        float lag, walk, run, sprint;
    };
    const Variant variants[] = {
        {"lag 150 ms, walk -20 %, run +20 %, sprint -15 %", 0.15f, 0.8f, 1.2f, 0.85f},
        {"lag 200 ms, walk +20 %, run -20 %, sprint +15 %", 0.20f, 1.2f, 0.8f, 1.15f},
    };
    for (const Variant& v : variants) {
        const HintRun r = RunHintPath(v.lag, v.walk, v.run, v.sprint);
        const std::string n = std::string("gait hint (") + v.name + "): ";
        Check(r.match >= r.ticks * 95 / 100, (n + "commanded gait = sender's >= 95 % of ticks").c_str(),
              Fmt("%.1f %%", 100.0 * r.match / r.ticks));
        Check(r.executed >= r.ticks * 90 / 100, (n + "walker's gait = sender's (lag ago) >= 90 %").c_str(),
              Fmt("%.1f %%", 100.0 * r.executed / r.ticks));
        Check(r.p95Moving <= 0.6, (n + "p95 error moving <= 0.6 m").c_str(), Fmt("%.2f m", r.p95Moving));
        Check(r.p95Standing <= 0.25, (n + "p95 error standing <= 0.25 m").c_str(), Fmt("%.2f m", r.p95Standing));
        Check(r.snaps == 0, (n + "no snaps on a smooth path").c_str(), Fmt("%.0f snaps", r.snaps));
    }

    // hint 0 but the copy is 2 m off: it walks there (does not stand), and stands once close
    LocomotionController h0;
    LocomotionInput in;
    in.gaitHint = 0;
    in.haveCopy = true;
    in.target = {2, 0, 0};
    in.dt = dt;
    Check(h0.Update(in).gait == Gait::Walk, "hint idle, copy 2 m off: walks there");
    in.copy = {1.85f, 0, 0};
    Check(h0.Update(in).gait == Gait::Stand, "hint idle, copy within the stop distance: stands");
    in.copy = {1.5f, 0, 0};
    Check(h0.Update(in).gait == Gait::Stand, "... and stays standing up to the start distance");
    // hint run while the wanted speed is a walk: run with a low multiplier, not a walk
    LocomotionController h2;
    in.gaitHint = 2;
    in.target = {0, 0, 0};
    in.copy = {0, 0, 0};
    in.targetVel = {1.5f, 0, 0};
    const LocomotionCommand c2 = h2.Update(in);
    Check(c2.gait == Gait::Run && std::fabs(c2.speedMult - 0.6f) < 1e-4f, "hint run at walk speed: run at the lowest multiplier",
          Fmt("mult %.2f", c2.speedMult));
    in.gaitHint = -1;
    LocomotionController hn;
    Check(hn.Update(in).gait == Gait::Walk, "no hint: the speed decides (walk)");
}

// ---- vehicle pin policy -------------------------------------------------------------------------------------------
// A copy whose physics follows only when `fine(t)`: otherwise it crawls at 1 m/s whatever it is given. Pinned: set on
// the target with the wanted velocity (the glue).
void RunPinCase(float seconds, bool (*fine)(float), std::vector<float>& retryTimes, std::vector<float>& pinTimes,
                float& maxPinned, bool& pinnedAtEnd, float& lastPinAt) {
    VehicleController vc;
    const float dt = 1.0f / 60;
    Vec3 t, p, v;
    maxPinned = 0;
    lastPinAt = -1;
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        const float now = i * dt;
        t.x += 15 * dt;
        VehicleInput in;
        in.target = t;
        in.targetVel = {15, 0, 0};
        in.haveCopy = true;
        in.copy = p;
        in.copyVel = v;
        in.power = 0.6f;
        in.dt = dt;
        const VehicleCommand c = vc.Update(in);
        if (c.justPinned) {
            pinTimes.push_back(now);
            lastPinAt = now;
        }
        if (c.retried) retryTimes.push_back(now);
        maxPinned = std::max(maxPinned, c.pinnedFor);
        if (c.fix == VehicleCommand::Fix::Pin || c.fix == VehicleCommand::Fix::Snap) {
            p = t;
            v = c.vel;
        } else if (c.fix == VehicleCommand::Fix::Blend) {
            p.x += c.nudge.x;
            v = c.vel;
        }
        if (!fine(now)) v.x = std::min(v.x, 1.0f);
        p.x += v.x * dt;
        pinnedAtEnd = c.pinned;
    }
}

void TestPinPolicy() {
    std::vector<float> retries, pins;
    float maxPinned = 0, lastPin = -1;
    bool pinnedEnd = false;
    RunPinCase(20, [](float t) { return t >= 2.0f; }, retries, pins, maxPinned, pinnedEnd, lastPin);
    const float firstFree = retries.empty() ? 99 : retries.front();
    Check(!pins.empty() && pins.front() < 2.0f, "slow physics for 2 s: pinned", Fmt("at %.2f s", pins.empty() ? -1 : pins.front()));
    Check(firstFree >= 2.0f && firstFree - 2.0f <= 2.0f, "... physics back within 2 s of recovering",
          Fmt("retried at %.2f s", firstFree));
    Check(pins.size() == 1 && !pinnedEnd, "... and stays unpinned afterwards", Fmt("%.0f pins", static_cast<double>(pins.size())));

    retries.clear();
    pins.clear();
    RunPinCase(60, [](float) { return false; }, retries, pins, maxPinned, pinnedEnd, lastPin);
    float maxGap = 0;
    for (size_t i = 1; i < retries.size(); ++i) maxGap = std::max(maxGap, retries[i] - retries[i - 1]);
    Check(maxPinned <= 8.0f + 1e-3f, "permanently slow: never pinned longer than 8 s at a time", Fmt("%.2f s", maxPinned));
    Check(retries.size() >= 6 && maxGap <= 8.0f + 1.2f + 0.05f, "permanently slow: retried at most every 8 s (+ the 1 s re-pin)",
          Fmt("%.0f retries, max gap %.2f s", static_cast<double>(retries.size()), maxGap));
    Check(retries.size() >= 3 && retries[1] - retries[0] > retries[0] - pins[0] + 0.5f,
          "permanently slow: the retry interval doubles");

    // the doubling resets after the copy kept up unpinned for 10 s
    retries.clear();
    pins.clear();
    RunPinCase(40, [](float t) { return (t >= 3.5f && t < 22.0f) ? true : t >= 22.0f + 10.0f; }, retries, pins, maxPinned,
               pinnedEnd, lastPin);
    // phase 1 (0-3.5 s): pinned at ~1 s, retried at 2.5 s, re-pinned ~3.5 s (3 s); fine until 22 s; slow again:
    // pinned at ~23 s for retryFirst again (1.5 s), not the doubled 3 s
    float secondSeries = -1;
    for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i] > 20) {
            for (float r : retries)
                if (r > pins[i]) {
                    secondSeries = r - pins[i];
                    break;
                }
            break;
        }
    Check(secondSeries > 0 && std::fabs(secondSeries - 1.5f) < 0.05f, "after 10 s kept up unpinned: the next pin retries after 1.5 s again",
          Fmt("%.2f s", secondSeries));

    VehicleController vc;
    VehicleInput in;
    in.haveCopy = true;
    in.target = {0, 0, 0};
    in.targetVel = {5, 0, 0};
    in.copyVel = {30, 0, 0};
    in.dt = 0.1f;
    const VehicleCommand c0 = vc.Update(in);
    const VehicleCommand c1 = vc.Update(in);
    Check(c0.justPinned && c0.pinnedFor == 0 && std::fabs(c1.pinnedFor - 0.1f) < 1e-5f && std::fabs(c1.retryIn - 1.4f) < 1e-4f,
          "pinnedFor / retryIn for traces", Fmt("%.2f %.2f", c1.pinnedFor, c1.retryIn));
}

}  // namespace

int main() {
    TestQuant();
    TestNlerp();
    TestLocomotion();
    TestSlowSprint();
    TestVehicle(CarPhysics::Carries);
    TestVehicle(CarPhysics::ZeroedByTeleport);
    TestVehicle(CarPhysics::ZeroedButVelocityGiven);
    TestVehicleRules();
    TestGaitHint();
    TestPinPolicy();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
