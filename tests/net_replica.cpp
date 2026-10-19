// net_replica: unit tests for the shared replication layer: replica (snapshot tracks and per-sender streams,
// src/net/replica.h), correction (src/net/correction.h) and authority (NetRef, ownership book, region authority,
// src/net/authority.h). Includes a check that a track reproduces the game layers' own interpolation (Mafia DE
// player_sync / traffic) bit for bit, so a game can switch to it without a visible change. Prints PASS/FAIL per
// check; exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "net/authority.h"
#include "net/correction.h"
#include "net/replica.h"
#include "net/wire.h"

using namespace mc::net;
using motion::Vec3;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Fmt(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, a, b, c, d);
    return buf;
}

struct Rng {  // deterministic LCG
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    double Uniform(double lo, double hi) { return lo + (hi - lo) * static_cast<double>(Next() & 0xFFFFFF) / 16777216.0; }
};

float Dist(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

replica::Sample S(double t, Vec3 p, Vec3 v, uint32_t bits = 0) {
    replica::Sample s;
    s.t = t;
    s.pos = p;
    s.vel = v;
    s.bits = bits;
    return s;
}

// ---- replica: track basics ---------------------------------------------------------------------------------------
void TestTrackBasics() {
    replica::SnapshotTrack tr;
    Check(tr.Empty() && !tr.At(0).valid, "empty track: invalid output");
    Check(tr.Push(S(1.0, {0, 0, 0}, {10, 0, 0})), "first sample accepted");
    Check(!tr.Push(S(1.0, {1, 0, 0}, {10, 0, 0})) && !tr.Push(S(0.5, {1, 0, 0}, {10, 0, 0})),
          "a sample not newer than the newest is ignored");
    Check(tr.Size() == 1, "size 1");
    replica::SnapshotTrack cap;
    for (int i = 0; i < 40; ++i) cap.Push(S(i * 0.1, {static_cast<float>(i), 0, 0}, {10, 0, 0}));
    Check(cap.Size() == 32 && std::fabs(cap.Oldest().t - 0.8) < 1e-9 && std::fabs(cap.Newest().t - 3.9) < 1e-9,
          "bounded ring: 32 kept, the oldest trimmed", Fmt("size %.0f oldest %.2f newest %.2f", static_cast<double>(cap.Size()), cap.Oldest().t, cap.Newest().t));
    // constant velocity: Hermite is exact between samples
    replica::SnapshotTrack lin;
    for (int i = 0; i < 10; ++i) lin.Push(S(i * 0.05, {i * 0.5f, 1, 0}, {10, 0, 0}, static_cast<uint32_t>(i)));
    replica::Out o = lin.At(0.112);
    Check(o.valid && std::fabs(o.pos.x - 1.12f) < 1e-4f && !o.extrapolated && !o.teleport,
          "between samples: on the path (constant velocity exact)", Fmt("x %.4f", o.pos.x));
    Check(o.bits == 2 && std::fabs(o.fromT - 0.10) < 1e-9 && std::fabs(o.toT - 0.15) < 1e-9 && std::fabs(o.u - 0.24f) < 1e-4f,
          "bits from the nearer sample; from/to/u report the pair", Fmt("bits %.0f u %.3f", o.bits, o.u));
    Check(lin.At(0.14).bits == 3, "past the middle: bits of the later sample");
    o = lin.At(-1);
    Check(o.pos.x == 0 && !o.extrapolated, "before the oldest: the oldest sample");
    o = lin.At(0.45 + 0.1);
    Check(o.extrapolated && !o.held && std::fabs(o.pos.x - 5.5f) < 1e-4f && std::fabs(o.starved - 0.1) < 1e-9,
          "past the newest by 0.1 s: extrapolated along its velocity", Fmt("x %.3f", o.pos.x));
    o = lin.At(0.45 + 0.6);
    Check(o.held && std::fabs(o.pos.x - (4.5f + 2.5f)) < 1e-4f && std::fabs(o.starved - 0.6) < 1e-9,
          "past the newest by 0.6 s: held at 0.25 s of extrapolation", Fmt("x %.3f", o.pos.x));
    // Present: the lead along the velocity, capped at 1.5 m
    o = lin.Present(0.112, 0.05);
    Check(std::fabs(o.pos.x - 1.62f) < 1e-4f, "Present: moved forward by velocity x lead", Fmt("x %.4f", o.pos.x));
    o = lin.Present(0.112, 1.0);
    Check(std::fabs(o.pos.x - (1.12f + 1.5f)) < 1e-4f, "Present: lead capped at 1.5 m", Fmt("x %.4f", o.pos.x));
    // teleport: a 30 m jump is not interpolated across
    replica::SnapshotTrack tp;
    tp.Push(S(0.0, {0, 0, 0}, {0, 0, 0}, 1));
    tp.Push(S(0.1, {30, 0, 0}, {0, 0, 0}, 2));
    o = tp.At(0.03);
    Check(o.teleport && o.pos.x == 0 && o.bits == 1, "teleport: the nearer (older) sample, not a sweep");
    o = tp.At(0.07);
    Check(o.teleport && o.pos.x == 30 && o.bits == 2, "teleport: the nearer (newer) sample");
    // rotation nlerp
    replica::SnapshotTrack rt;
    replica::Sample a = S(0, {0, 0, 0}, {0, 0, 0}), b = S(1, {0, 0, 0}, {0, 0, 0});
    a.hasRot = b.hasRot = true;
    b.rot[2] = std::sin(0.5f);  // 60 deg about z
    b.rot[3] = std::cos(0.5f);
    rt.Push(a);
    rt.Push(b);
    o = rt.At(0.5);
    float ref[4];
    motion::Nlerp(a.rot, b.rot, 0.5f, ref);
    Check(o.hasRot && std::fabs(o.rot[2] - ref[2]) < 1e-6f && std::fabs(o.rot[3] - ref[3]) < 1e-6f, "rotation nlerped");
    // acceleration-aware extrapolation (accelTime > 0)
    replica::TrackTuning tt;
    tt.accelTime = 0.1;
    replica::SnapshotTrack ac(tt);
    ac.Push(S(0.0, {0, 0, 0}, {10, 0, 0}));
    ac.Push(S(0.1, {0.75f, 0, 0}, {5, 0, 0}));  // braking at 50 m/s^2
    o = ac.At(0.15);
    Check(std::fabs(o.pos.x - (0.75f + 0.25f - 0.0625f)) < 1e-3f, "accelTime: a brake is followed (p + v t + a t^2 / 2)",
          Fmt("x %.4f", o.pos.x));
    o = ac.At(0.4);
    Check(std::fabs(o.pos.x - (0.75f + 0.5f - 0.25f + 0.0f * 0.15f)) < 1e-3f, "accelTime: acceleration only for accelTime, then the velocity reached",
          Fmt("x %.4f", o.pos.x));
}

// ---- replica: same results as the game layers' interpolation (copied from the game repo's player_sync.cpp
// Interpolate + Present and traffic_model.cpp GuestTraffic::Present, positions/velocities only) --------------------
struct RefSnap {
    double t;
    Vec3 p, v;
};

Vec3 RefPlayer(const std::deque<RefSnap>& sn, double rt, double lead, Vec3* velOut) {
    Vec3 p, v;
    if (rt <= sn.front().t) {
        p = sn.front().p;
        v = sn.front().v;
    } else if (rt >= sn.back().t) {
        p = sn.back().p;
        v = sn.back().v;
        const float dt = static_cast<float>(std::min(rt - sn.back().t, 0.25));
        p.x += v.x * dt;
        p.y += v.y * dt;
        p.z += v.z * dt;
    } else {
        size_t i = 0;
        while (i + 1 < sn.size() && sn[i + 1].t < rt) ++i;
        const RefSnap& a = sn[i];
        const RefSnap& b = sn[i + 1];
        const float t = static_cast<float>((rt - a.t) / std::max(1e-6, b.t - a.t));
        const RefSnap& n = t < 0.5f ? a : b;
        const float dx = a.p.x - b.p.x, dy = a.p.y - b.p.y, dz = a.p.z - b.p.z;
        if (dx * dx + dy * dy + dz * dz > 20.0f * 20.0f) {
            p = n.p;
            v = n.v;
        } else {
            p = playout::Hermite(a.p, a.v, b.p, b.v, b.t - a.t, t);
            auto l = [t](float x, float y) { return x + (y - x) * t; };
            v = {l(a.v.x, b.v.x), l(a.v.y, b.v.y), l(a.v.z, b.v.z)};
        }
    }
    const float ld = static_cast<float>(std::max(0.0, lead));
    float d[3] = {v.x * ld, v.y * ld, v.z * ld};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float k = len > 1.5f ? 1.5f / len : 1.0f;
    p.x += d[0] * k;
    p.y += d[1] * k;
    p.z += d[2] * k;
    *velOut = v;
    return p;
}

void TestMatchesGameLayer() {
    Rng rng(7);
    replica::SnapshotTrack tr;  // defaults = the player configuration (64 kept there; 32 is plenty for 0.3 s at 60 Hz)
    std::deque<RefSnap> ref;
    double t = 0;
    Vec3 p, v{3, 0, 0};
    float worst = 0, worstV = 0;
    int compared = 0;
    for (int i = 0; i < 2000; ++i) {
        t += rng.Uniform(0.010, 0.030);
        v.x += static_cast<float>(rng.Uniform(-0.5, 0.5));
        v.y += static_cast<float>(rng.Uniform(-0.5, 0.5));
        p.x += v.x * 0.02f;
        p.y += v.y * 0.02f;
        if (i % 500 == 250) p.x += 40;  // a teleport now and then
        tr.Push(S(t, p, v));
        ref.push_back({t, p, v});
        while (ref.size() > 32) ref.pop_front();
        for (int k = 0; k < 3; ++k) {
            const double rt = t - rng.Uniform(-0.4, 0.3);
            const double lead = rng.Uniform(0, 0.3);
            Vec3 rv;
            const Vec3 rp = RefPlayer(ref, rt, lead, &rv);
            const replica::Out o = tr.Present(rt, lead);
            worst = std::max(worst, Dist(rp, o.pos));
            worstV = std::max(worstV, Dist(rv, o.vel));
            ++compared;
        }
    }
    Check(worst < 1e-5f && worstV < 1e-5f, "Present() equals the player interpolation + lead (Mafia DE 0.20)",
          Fmt("%.0f queries, max pos diff %.2g m, vel diff %.2g", compared, worst, worstV));
}

// ---- replica: stream ---------------------------------------------------------------------------------------------
void TestStream() {
    replica::Stream st;
    // a 60 Hz sender, 3 entities on circles, arriving 40-60 ms later with jitter; render 10 s
    Rng rng(11);
    const double dt = 1.0 / 60;
    std::vector<std::pair<double, std::pair<double, int>>> inflight;  // arrival, (send time, n)
    auto truth = [](uint32_t key, double t) {
        const double r = 5.0 + key, w = 0.8 + 0.1 * key;
        return Vec3{static_cast<float>(r * std::cos(w * t)), static_cast<float>(r * std::sin(w * t)), 0};
    };
    auto truthVel = [](uint32_t key, double t) {
        const double r = 5.0 + key, w = 0.8 + 0.1 * key;
        return Vec3{static_cast<float>(-r * w * std::sin(w * t)), static_cast<float>(r * w * std::cos(w * t)), 0};
    };
    std::vector<std::pair<double, double>> queue;  // (arrival local, send sender-clock)
    double worst = 0;
    int extrapolated = 0, frames = 0;
    const double clockOffset = 1000.0;  // local = sender + 1000 + one-way delay
    double nextSend = 0;
    for (double now = clockOffset; now < clockOffset + 10; now += 1.0 / 144) {
        while (nextSend <= now - clockOffset) {
            queue.push_back({nextSend + clockOffset + rng.Uniform(0.040, 0.060), nextSend});
            nextSend += dt;
        }
        std::stable_sort(queue.begin(), queue.end());
        while (!queue.empty() && queue.front().first <= now) {
            const double ts = queue.front().second;
            st.Observe(ts, queue.front().first);
            for (uint32_t key : {30u, 10u, 20u}) st.Push(key, S(ts, truth(key, ts), truthVel(key, ts)));
            queue.erase(queue.begin());
        }
        st.Update(now);
        if (now - clockOffset < 2) continue;
        ++frames;
        const double rt = st.RenderTime(now);
        for (uint32_t key : st.Keys()) {
            const replica::Out o = st.At(key, now);
            if (o.extrapolated) ++extrapolated;
            worst = std::max(worst, static_cast<double>(Dist(o.pos, truth(key, rt))));
        }
    }
    const std::vector<uint32_t> keys = st.Keys();
    Check(keys.size() == 3 && keys[0] == 10 && keys[1] == 20 && keys[2] == 30, "keys in ascending order");
    Check(worst < 0.01, "stream with 20 ms jitter: interpolated within 1 cm of the truth", Fmt("%.4f m", worst));
    Check(extrapolated < frames * 3 / 50, "rarely extrapolates (adaptive delay covers the jitter)",
          Fmt("%.0f of %.0f", extrapolated, frames * 3));
    Check(st.Delay() > 0.015 && st.Delay() < 0.06, "playout delay follows the jitter", Fmt("%.1f ms", st.Delay() * 1000));
    const replica::Out pr = st.Present(10, clockOffset + 10);
    Check(pr.valid, "Present by key");
    Check(!st.At(99, clockOffset + 10).valid && st.Track(99) == nullptr, "unknown key: invalid, no track");
    // an old sample is dropped; Forget drops tracks without fresh samples
    const uint32_t before = st.Dropped();
    st.Push(10, S(0.0, {}, {}));
    Check(st.Dropped() == before + 1, "an old sample for a key is counted as dropped");
    st.Observe(nextSend, clockOffset + 20);
    st.Push(20, S(nextSend, truth(20, nextSend), {}));
    const std::vector<uint32_t> gone = st.Forget(clockOffset + 15);
    Check(gone.size() == 2 && gone[0] == 10 && gone[1] == 30 && st.Size() == 1, "Forget drops stale tracks (key order)");
    Check(st.Remove(20) && st.Size() == 0 && !st.Remove(20), "Remove");
    st.Reset();
    Check(!st.Clock().Ready(), "Reset forgets the clock");
}

// ---- correction --------------------------------------------------------------------------------------------------
void TestCorrection() {
    using correction::Corrector;
    using correction::Result;
    using correction::Tuning;
    const float dt = 1.0f / 60;
    {
        Corrector c(Tuning::ForFoot());
        Result r = c.Update({0, 0, 0}, {0.25f, 0, 0}, {0, 0, 0}, dt);
        Check(r.kind == Result::Kind::None && std::fabs(r.error - 0.25f) < 1e-6f, "inside the dead zone: nothing");
        r = c.Update({0, 0, 0}, {1.0f, 0, 0}, {0, 0, 0}, dt);
        Check(r.kind == Result::Kind::Blend && std::fabs(r.delta.x - 0.15f * dt) < 1e-6f,
              "standing target: blend capped at restSpeed (0.15 m/s)", Fmt("%.5f m", r.delta.x));
        r = c.Update({0, 0, 0}, {1.0f, 0, 0}, {5, 0, 0}, dt);
        const float want = std::min(0.7f * 2.0f * dt, 0.35f * 5 * dt);
        Check(r.kind == Result::Kind::Blend && std::fabs(r.delta.x - want) < 1e-6f,
              "moving target: gain x dt of the excess, capped at 35 % of its speed", Fmt("%.5f m (want %.5f)", r.delta.x, want));
        r = c.Update({0, 0, 0}, {0.4f, 0, 0}, {6, 0, 0}, dt);
        Check(std::fabs(r.delta.x - 0.1f * 2.0f * dt) < 1e-6f, "small excess: only gain x dt of it", Fmt("%.5f m", r.delta.x));
        r = c.Update({0, 0, 0}, {0.5f, 0, 2.0f}, {0, 0, 0}, dt);
        Check(r.delta.z == 0 && std::fabs(r.error - 0.5f) < 1e-6f, "foot: ground plane only (height not blended)");
        r = c.Update({0, 0, 0}, {4, 0, 0}, {0, 0, 0}, dt);
        Check(r.kind == Result::Kind::Snap && r.why == Result::Why::Far && r.delta.x == 4, "beyond snapDist: snap (far)");
        r = c.Update({0, 0, 0}, {4, 0, 3.5f}, {0, 0, 0}, dt);  // target moved 3.5 m: no teleport
        r = c.Update({0, 0, 0}, {0, 0, 3.5f}, {0, 0, 0}, dt);
        Check(r.kind == Result::Kind::Snap && r.why == Result::Why::Height, "3.5 m height: snap (height)");
        r = c.Update({0, 0, 0}, {0, 0, 3.5f}, {0, 0, 0}, dt, true);
        Check(r.kind != Result::Kind::Snap, "the same in the air: no snap (doubled)");
        r = c.Update({0, 0, 0}, {0, 0, 7.0f}, {0, 0, 0}, dt, true);
        Check(r.kind == Result::Kind::Snap && r.why == Result::Why::Height, "7 m height in the air: snap");
        c.Reset();
        c.Update({0, 0, 0}, {0, 0, 0}, {0, 0, 0}, dt);
        r = c.Update({24.9f, 0, 0}, {25, 0, 0}, {0, 0, 0}, dt);
        Check(r.kind == Result::Kind::Snap && r.why == Result::Why::Teleport, "target jumped 25 m: snap (teleport), even with the copy close");
        // stuck: target stands, copy 0.8 m off, blending cannot fix it (the copy is held back)
        c.Reset();
        int at = -1;
        for (int i = 0; i < 300 && at < 0; ++i)
            if (c.Update({0, 0, 0}, {0.8f, 0, 0}, {0, 0, 0}, dt).kind == Result::Kind::Snap) at = i;
        Check(at > 115 && at < 125, "stuck 0.8 m off a standing target for 2 s: snap (stuck)", Fmt("after %.2f s", at * dt));
        Check(c.Snaps(Result::Why::Stuck) == 1, "stuck counted");
        c.Reset();
        at = -1;
        for (int i = 0; i < 300 && at < 0; ++i)
            if (c.Update({0, 0, 0}, {0.8f, 0, 0}, {2, 0, 0}, dt).kind == Result::Kind::Snap) at = i;
        Check(at < 0, "0.8 m off a moving target: never stuck");
        Check(c.Snaps() == 6 && c.Snaps(Result::Why::Far) == 2 && c.Snaps(Result::Why::Height) == 2 &&
                  c.Snaps(Result::Why::Teleport) == 1 && c.Snaps(Result::Why::Stuck) == 1,
              "snap counters by reason", Fmt("%.0f snaps", c.Snaps()));
        Check(c.Blends() > 0 && c.BlendMetres() > 0, "blend counters", Fmt("%.0f blends, %.3f m", c.Blends(), c.BlendMetres()));
        c.ResetCounters();
        Check(c.Snaps() == 0 && c.Blends() == 0 && c.BlendMetres() == 0, "ResetCounters");
        Check(std::string(correction::WhyName(Result::Why::Stuck)) == "stuck", "WhyName");
    }
    {
        Corrector c(Tuning::ForCar());
        const Result r = c.Update({0, 0, 0}, {0, 0, 1.0f}, {10, 0, 0}, dt);
        Check(r.kind == Result::Kind::Blend && r.delta.z > 0 && std::fabs(r.error - 1.0f) < 1e-6f, "car: errors in 3D");
    }
    // a copy that walks 10 % too slow behind a 4 m/s target: blending keeps it near the dead zone, never snaps
    {
        Corrector c(Tuning::ForFoot());
        Vec3 copy, target;
        float worst = 0;
        for (int i = 0; i < 600; ++i) {
            target.x += 4 * dt;
            copy.x += 3.6f * dt;
            const Result r = c.Update(copy, target, {4, 0, 0}, dt);
            copy.x += r.delta.x;
            if (i > 300) worst = std::max(worst, r.error);
        }
        Check(c.Snaps() == 0 && worst < 0.6f, "10 % slow copy: blended, held near the dead zone, no snap", Fmt("%.2f m", worst));
    }
    const Tuning f = Tuning::ForFoot(), n = Tuning::ForNpc(), car = Tuning::ForCar(), tr = Tuning::ForTraffic();
    Check(f.deadZone <= n.deadZone && n.deadZone <= tr.deadZone && f.snapDist < tr.snapDist && !car.groundOnly &&
              !tr.groundOnly && f.groundOnly && n.groundOnly,
          "presets ordered: foot tightest, traffic loosest; vehicles in 3D");
    Check(correction::BlendStep(1.0f, 0.3f, 2.0f, 0.35f, 0.15f, 0, 0) == 0, "BlendStep: dt 0 moves nothing");
}

// ---- authority ---------------------------------------------------------------------------------------------------
void TestNetRef() {
    using namespace authority;
    const NetRef r{2, static_cast<uint8_t>(RefKind::Traffic), 0xDEADBEEF};
    Check(NetRef::Unpack(r.Pack()) == r && r.Pack() == 0x0203DEADBEEFull, "NetRef pack/unpack");
    ByteWriter w;
    r.Write(w);
    Check(w.Size() == NetRef::kWireSize, "NetRef is 6 bytes on the wire", Fmt("%.0f", static_cast<double>(w.Size())));
    ByteReader rd(w.Data(), w.Size());
    const NetRef back = NetRef::Read(rd);
    Check(back == r && !rd.Failed(), "NetRef wire round trip");
    ByteReader shortR(w.Data(), 3);
    NetRef::Read(shortR);
    Check(shortR.Failed(), "NetRef read from 3 bytes fails cleanly");
    Check(r.ToString() == "2:3:3735928559", "NetRef ToString", r.ToString());
    Check(NetRef{1, 2, 3} < NetRef{1, 3, 0} && NetRef{0, 9, 9} < NetRef{1, 0, 0}, "NetRef order: home, kind, local");
}

void TestOwnership() {
    using namespace authority;
    OwnershipBook b;
    const NetRef car{1, static_cast<uint8_t>(RefKind::PlayerCar), 7};
    Check(b.Owner(car) == kNoPlayer && b.Epoch(car) == 0, "unknown ref: no owner, epoch 0");
    ClaimResult r = b.Claim(car, 1, 0);
    Check(r.granted && r.owner == 1 && b.Owner(car) == 1, "no owner: the claimer gets it");
    b.Tick();
    r = b.Claim(car, 2, 0);
    Check(!r.granted && r.owner == 1, "equal epoch, later tick: the current owner keeps it");
    r = b.Claim(car, 2, 1);
    Check(r.granted && r.owner == 2 && r.epoch == 1, "higher epoch wins");
    r = b.Claim(car, 1, 0);
    Check(!r.granted && r.owner == 2 && r.epoch == 1, "stale epoch refused");
    r = b.Claim(car, 2, 1);
    Check(r.granted && r.owner == 2, "owner re-claiming: granted");
    // two claims of one epoch in one tick: lower id, whatever the arrival order
    for (int order = 0; order < 2; ++order) {
        OwnershipBook c;
        const NetRef npc{0, static_cast<uint8_t>(RefKind::Npc), 42};
        c.Claim(npc, 0, 0);
        c.Tick();
        const PlayerId first = order == 0 ? 3 : 1, second = order == 0 ? 1 : 3;
        c.Claim(npc, first, 1);
        const ClaimResult s = c.Claim(npc, second, 1);
        Check(c.Owner(npc) == 1 && s.owner == 1, order == 0 ? "same epoch, same tick: lower id wins (3 then 1)"
                                                            : "same epoch, same tick: lower id wins (1 then 3)");
        c.Tick();
        Check(!c.Claim(npc, 0, 1).granted, "... and in the next tick the owner keeps it against a lower id");
    }
    // release
    Check(!b.Release(car, 1) && b.Owner(car) == 2, "release by a non-owner: refused");
    Check(b.Release(car, 2) && b.Owner(car) == kNoPlayer && b.Epoch(car) == 1, "release: no owner, epoch kept");
    Check(!b.Claim(car, 3, 0).granted && b.Claim(car, 3, 1).granted, "after a release: equal epoch claims it, stale does not");
    // a player leaving
    OwnershipBook l;
    const NetRef a{2, 2, 1}, bb{2, 2, 2}, c2{0, 3, 5}, d{3, 1, 9};
    l.Claim(a, 2, 0);
    l.Claim(bb, 2, 4);
    l.Claim(c2, 2, 0);
    l.Claim(d, 3, 0);
    const std::vector<NetRef> moved = l.OnPlayerLeft(2, 0);
    Check(moved.size() == 3 && moved[0] == c2 && moved[1] == a && moved[2] == bb, "player left: his refs, in ref order");
    Check(l.Owner(a) == 0 && l.Owner(bb) == 0 && l.Owner(c2) == 0 && l.Owner(d) == 3, "... go to the host, others untouched");
    Check(l.Epoch(a) == 1 && l.Epoch(bb) == 5, "... with the epoch raised (his late claims are stale)");
    Check(!l.Claim(bb, 2, 4).granted, "the left player's in-flight claim with the old epoch is refused");
    Check(l.OwnedBy(0).size() == 3 && l.OwnedBy(2).empty(), "OwnedBy");
    const std::vector<NetRef> hostLeft = l.OnPlayerLeft(0, 0);
    Check(hostLeft.size() == 3 && l.Owner(a) == kNoPlayer, "the fallback leaving himself: refs unowned");
}

void TestRegions() {
    using namespace authority;
    RegionAuthority ra;
    std::vector<PlayerSpot> ps = {{0, {0, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    Check(ra.AuthorityAt({1000, 0, 0}) == 0 && ra.Regions().size() == 1, "host alone: owns everything");
    // guest near the host
    ps = {{0, {0, 0, 0}, true}, {1, {100, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    Check(ra.AuthorityAt({150, 0, 0}) == 0, "guest near host: the host owns the point between them");
    Check(ra.Regions().size() == 1 && ra.RegionOf(1) == 0, "guest near host: one region (the host's)");
    Check(ra.AuthorityAt({400, 0, 0}) == 1, "beyond the host radius: the nearer guest");
    // guest far away
    ps = {{0, {0, 0, 0}, true}, {1, {2000, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    Check(ra.AuthorityAt({2050, 0, 0}) == 1 && ra.AuthorityAt({50, 0, 0}) == 0, "guest far away: owns his surroundings");
    std::vector<Region> rs = ra.Regions();
    Check(rs.size() == 2 && rs[0].owner == 0 && rs[1].owner == 1 && ra.RegionOf(1) == 1, "guest far away: his own region");
    // two far guests together: the lower id owns, the other shows his
    ps = {{0, {0, 0, 0}, true}, {2, {2030, 0, 0}, true}, {1, {2000, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    rs = ra.Regions();
    Check(rs.size() == 2 && rs[1].owner == 1 && ra.RegionOf(2) == 1 && ra.RegionOf(1) == 1,
          "two far guests together: the lower id's region, the other shows it");
    Check(ra.AuthorityAt({2014, 0, 0}) == 1 && ra.AuthorityAt({2016, 0, 0}) == 2,
          "AuthorityAt alone is the nearest (Keep / Regions add the stickiness)");
    // hysteresis at the edges
    ps = {{0, {0, 0, 0}, true}, {1, {600, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    Check(ra.AuthorityAt({320, 0, 0}) == 1, "a point 320 m from the host, 280 m from the guest: guest (no history)");
    Check(ra.Keep(0, {320, 0, 0}) == 0, "... but the host keeps it (within the release radius)");
    Check(ra.Keep(0, {420, 0, 0}) == 1, "beyond 400 m from the host and the guest 230 m nearer: hand over");
    ps = {{0, {0, 0, 0}, true}, {1, {800, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    Check(ra.Keep(0, {410, 0, 0}) == 0, "beyond 400 m but the other only 20 m nearer (< margin): kept");
    Check(ra.Keep(1, {200, 0, 0}) == 0 && ra.Keep(1, {500, 0, 0}) == 1, "guest keeps within 400 m, loses beyond to a much nearer host");
    // a guest leaving the world
    ps = {{0, {0, 0, 0}, true}, {1, {2000, 0, 0}, false}};
    ra.SetPlayers(ps, 0);
    Check(ra.AuthorityAt({2000, 0, 0}) == 0 && ra.Keep(1, {2000, 0, 0}) == 0 && ra.Regions().size() == 1 &&
              ra.RegionOf(1) == 0,
          "guest out of the world: his area falls back to the host");
    // nobody in the world (host loading): host
    ps = {{0, {0, 0, 0}, false}, {1, {2000, 0, 0}, true}};
    ra.SetPlayers(ps, 0);
    Check(ra.AuthorityAt({0, 0, 0}) == 1 && ra.Regions().size() == 2, "host not in the world: the guest owns, the host region stays listed");
}

}  // namespace

int main() {
    TestTrackBasics();
    TestMatchesGameLayer();
    TestStream();
    TestCorrection();
    TestNetRef();
    TestOwnership();
    TestRegions();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
