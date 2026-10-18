// net_latency: how fast things get across (docs/protocol.md "Latency").
//   1. PlayoutClock (net/playout.h): adaptive playout delay against simulated streams (steady, jitter, loss, clock
//      drift, a jitter burst that calms down) compared with the old fixed 50 ms; Hermite / Extrapolate.
//   2. Loopback UDP sessions in one process: send -> receive delay with and without Session::Flush, reliable delivery
//      delay under loss (fast retransmit), lanes against head-of-line blocking, the session clock (skewed clocks),
//      one-way delay measured with SessionTime stamps.
// Prints PASS/FAIL per check plus the measured numbers, exits non-zero on any failure. "-v" prints net logs.
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "net/playout.h"
#include "net/session.h"
#include "net/udp_transport.h"
#include "net/wire.h"

#pragma comment(lib, "winmm.lib")  // timeBeginPeriod

using namespace mc::net;

namespace {

int g_failures = 0;
bool g_verbose = false;

double Seconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Fmt(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}

void PrintLog(int level, const char* msg) {
    if (level >= 2 || g_verbose) std::printf("    %s\n", msg);
}

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1);
    return v[k];
}

// ---- 1. playout ------------------------------------------------------------------------------------------------
struct StreamSim {
    double rate = 60;        // sender snapshots per second
    double frame = 1 / 60.0; // receiver frame (arrivals are seen at the next frame)
    double base = 0.030;     // one-way delay
    double jitter = 0;       // + uniform [0, jitter] (std::exp-like spikes added by spikeProb)
    double loss = 0;
    double drift = 0;        // receiver clock runs (1 + drift) x the sender's
    double skew = 1000;      // receiver clock - sender clock at t=0
};

struct StreamResult {
    double delayEnd = 0;          // ms, Delay() at the end
    double lateFrac = 0;          // snapshots that arrived after the render time passed them
    double fixedLateFrac = 0;     // the same with a fixed 50 ms delay after the min offset
    double starvedFrac = 0;       // frames where the render time was past the newest snapshot
    double maxStep = 0;           // ms: largest deviation of a render-time step from the frame time
    double meanDelay = 0;         // ms
};

// Runs `seconds` of a stream through a PlayoutClock; phases may change the jitter at given times.
StreamResult RunStream(const StreamSim& s, double seconds, std::mt19937& rng,
                       const std::function<void(double t, StreamSim&)>& phase = {}) {
    StreamSim cur = s;
    playout::PlayoutClock clk;
    std::uniform_real_distribution<double> U(0, 1);
    struct Pkt {
        double arrive, stamp;
    };
    std::vector<Pkt> inflight;
    double nextSend = 0, newest = -1e9;
    double minRaw = 1e9;
    uint64_t arrived = 0, late = 0, fixedLate = 0, frames = 0, starved = 0;
    double lastRt = 0, sumDelay = 0, maxStep = 0;
    bool haveRt = false;
    int measuredFrom = 0;
    for (double t = 0; t < seconds; t += cur.frame) {
        if (phase) phase(t, cur);
        while (nextSend <= t) {
            if (U(rng) >= cur.loss) inflight.push_back({nextSend + cur.base + U(rng) * cur.jitter, nextSend});
            nextSend += 1.0 / cur.rate;
        }
        const double local = cur.skew + t * (1 + cur.drift);
        // Fixed-delay comparison: render = local - minRaw - 50 ms.
        std::vector<Pkt> keep;
        for (const Pkt& p : inflight) {
            if (p.arrive > t) {
                keep.push_back(p);
                continue;
            }
            const double rt = clk.RenderTime(local);
            const bool measuring = t > 2.0;  // after the start-up
            if (measuring) {
                ++arrived;
                if (clk.Ready() && p.stamp < rt) ++late;
                if (minRaw < 1e8 && p.stamp < local - minRaw - 0.050) ++fixedLate;
            }
            minRaw = std::min(minRaw, local - p.stamp);
            clk.Observe(p.stamp, local);
            newest = std::max(newest, p.stamp);
        }
        inflight.swap(keep);
        clk.Update(local);
        const double rt = clk.RenderTime(local);
        if (t > 2.0) {
            ++frames;
            if (rt > newest) ++starved;
            sumDelay += clk.Delay();
            if (haveRt) maxStep = std::max(maxStep, std::fabs((rt - lastRt) - cur.frame * (1 + cur.drift)));
            ++measuredFrom;
        }
        lastRt = rt;
        haveRt = clk.Ready();
    }
    StreamResult r;
    r.delayEnd = clk.Delay() * 1000;
    r.lateFrac = arrived ? static_cast<double>(late) / static_cast<double>(arrived) : 0;
    r.fixedLateFrac = arrived ? static_cast<double>(fixedLate) / static_cast<double>(arrived) : 0;
    r.starvedFrac = frames ? static_cast<double>(starved) / static_cast<double>(frames) : 0;
    r.maxStep = maxStep * 1000;
    r.meanDelay = frames ? sumDelay / static_cast<double>(frames) * 1000 : 0;
    return r;
}

void TestPlayout() {
    std::printf("-- playout clock\n");
    std::mt19937 rng(7);
    {
        StreamSim s;
        StreamResult r = RunStream(s, 20, rng);
        Check(r.delayEnd > 15 && r.delayEnd < 45, "steady 60 Hz: delay below the old 50 ms",
              Fmt("delay %.1f ms (mean %.1f), late %.2f%%, starved %.2f%%", r.delayEnd, r.meanDelay, r.lateFrac * 100,
                  r.starvedFrac * 100));
        Check(r.lateFrac < 0.02, "steady 60 Hz: nothing arrives too late", Fmt("%.2f%%", r.lateFrac * 100));
        Check(r.maxStep < 0.25 * 1000 / 60.0 + 0.5, "steady 60 Hz: render clock never jumps",
              Fmt("max step deviation %.2f ms", r.maxStep));
    }
    {
        StreamSim s;
        s.jitter = 0.060;
        StreamResult r = RunStream(s, 30, rng);
        Check(r.delayEnd > 55 && r.delayEnd < 110, "60 ms jitter: delay grows to cover it",
              Fmt("delay %.1f ms, late %.2f%% (fixed 50 ms: %.2f%%)", r.delayEnd, r.lateFrac * 100,
                  r.fixedLateFrac * 100));
        Check(r.lateFrac < 0.08 && r.lateFrac < r.fixedLateFrac / 3, "60 ms jitter: far fewer late than fixed 50 ms",
              Fmt("%.2f%% vs %.2f%%", r.lateFrac * 100, r.fixedLateFrac * 100));
    }
    {
        StreamSim s;
        s.loss = 0.15;
        s.jitter = 0.010;
        StreamResult r = RunStream(s, 20, rng);
        Check(r.delayEnd < 60 && r.lateFrac < 0.05, "15% loss + 10 ms jitter: stays short",
              Fmt("delay %.1f ms, late %.2f%%, starved %.2f%%", r.delayEnd, r.lateFrac * 100, r.starvedFrac * 100));
    }
    {
        StreamSim s;
        s.drift = 200e-6;  // 200 ppm, far more than two PCs' clocks differ
        StreamResult r = RunStream(s, 60, rng);
        Check(r.delayEnd < 45 && r.lateFrac < 0.02, "200 ppm clock drift: offset follows",
              Fmt("delay %.1f ms, late %.2f%%", r.delayEnd, r.lateFrac * 100));
        StreamSim n = s;
        n.drift = -200e-6;
        StreamResult r2 = RunStream(n, 60, rng);
        Check(r2.delayEnd < 45 && r2.lateFrac < 0.02, "-200 ppm clock drift: offset follows",
              Fmt("delay %.1f ms, late %.2f%%", r2.delayEnd, r2.lateFrac * 100));
    }
    {
        // 10 s of 80 ms jitter, then calm: the delay comes back down.
        StreamSim s;
        double peak = 0;
        playout::PlayoutTuning tun;
        StreamResult r = RunStream(s, 40, rng, [&](double t, StreamSim& c) { c.jitter = t < 10 ? 0.080 : 0.0; });
        (void)peak;
        (void)tun;
        Check(r.delayEnd < 45, "jitter burst then calm: delay shrinks back", Fmt("delay at the end %.1f ms", r.delayEnd));
    }
    {
        // A 1 s stall (route change) then the stream comes back 200 ms later than before: the offset follows within
        // the offset window, without a jump in the render clock.
        StreamSim s;
        StreamResult r = RunStream(s, 20, rng, [&](double t, StreamSim& c) { c.base = t < 8 ? 0.030 : 0.230; });
        Check(r.delayEnd < 45 && r.maxStep < 0.25 * 1000 / 60.0 + 0.5, "latency step +200 ms: recovers, no jump",
              Fmt("delay %.1f ms, max step deviation %.2f ms", r.delayEnd, r.maxStep));
    }
    {
        // 20 Hz sender (traffic/NPC streams): the interval part dominates
        StreamSim s;
        s.rate = 20;
        StreamResult r = RunStream(s, 20, rng);
        Check(r.delayEnd > 45 && r.delayEnd < 90 && r.starvedFrac < 0.05, "20 Hz stream: delay ~ one interval",
              Fmt("delay %.1f ms, starved %.2f%%", r.delayEnd, r.starvedFrac * 100));
    }
    {
        playout::PlayoutClock c;
        Check(!c.Ready() && c.Delay() <= 0.0 + 1e-9, "fresh clock: not ready");
        c.Observe(10.0, 110.03);
        c.Update(110.03);
        Check(c.Ready() && std::fabs(c.Delay() - 0.050) < 1e-6, "first sample: start delay 50 ms",
              Fmt("%.1f ms", c.Delay() * 1000));
    }
    {
        using playout::Vec3;
        Vec3 p0{0, 0, 0}, v0{10, 0, 0}, p1{1, 0, 0}, v1{10, 0, 0};
        Vec3 m = playout::Hermite(p0, v0, p1, v1, 0.1, 0.5);
        Check(std::fabs(m.x - 0.5f) < 1e-4f, "Hermite: constant velocity is a line", Fmt("%.4f", m.x));
        Vec3 c0{0, 0, 0}, cv0{10, 0, 0}, c1{1, 1, 0}, cv1{0, 10, 0};
        Vec3 e = playout::Hermite(c0, cv0, c1, cv1, 0.1, 1.0);
        Check(e.x == 1 && e.y == 1, "Hermite: ends at the second snapshot");
        Vec3 x = playout::Extrapolate({0, 0, 0}, {10, 0, 0}, {-50, 0, 0}, 1.0, 0.25, 0.1);
        // 0.1 s braking: 1 - 0.25 = 0.75 m, speed 5 m/s, then 0.15 s at 5: 0.75 -> 1.5 m
        Check(std::fabs(x.x - 1.5f) < 1e-4f, "Extrapolate: clamped, acceleration only briefly", Fmt("%.4f", x.x));
    }
}

// ---- 2. loopback sessions --------------------------------------------------------------------------------------
struct Node {
    UdpTransport transport;
    Session session;
    Node(const char* name, SessionConfig cfg, UdpOptions opt = {})
        : transport(opt), session(transport, (cfg.playerName = name, cfg)) {}
};

void SpinUntil(double t) {
    while (Seconds() < t) Sleep(0);
}

bool Connect(Node& host, Node& guest) {
    if (!host.session.Host(0)) return false;
    const std::string addr = "127.0.0.1:" + std::to_string(host.transport.BoundPort());
    if (!guest.session.Join(addr)) return false;
    const double until = Seconds() + 5;
    while (Seconds() < until) {
        host.session.Poll();
        guest.session.Poll();
        if (guest.session.State() == SessionState::Connected && host.session.Players().size() == 2) return true;
        Sleep(1);
    }
    return false;
}

constexpr uint16_t kMsgStamp = 0x7001;

// The guest runs 60 fps frames (Poll, send one stamped message, optionally Flush, wait for the next frame); the host
// polls every ~0.5 ms and records how long each message took.
std::vector<double> FrameDelays(bool flush, int frames) {
    SessionConfig cfg;
    Node host("host", cfg), guest("guest", cfg);
    std::vector<double> delays;
    host.session.On(kMsgStamp, [&](PlayerId, ByteReader& r) {
        const double sent = r.F64();
        if (r.Ok()) delays.push_back((Seconds() - sent) * 1000);
    });
    if (!Connect(host, guest)) return {};
    const double frame = 1 / 60.0;
    double next = Seconds();
    for (int f = 0; f < frames; ++f) {
        guest.session.Poll();
        ByteWriter w(16);
        w.F64(Seconds());
        guest.session.SendToHost(kMsgStamp, w.Span(), Reliability::Unreliable);
        if (flush) guest.session.Flush();
        next += frame;
        while (Seconds() < next) {
            host.session.Poll();
            SpinUntil(std::min(next, Seconds() + 0.0005));
        }
    }
    return delays;
}

void TestFlush() {
    std::printf("-- send -> receive delay (60 fps sender, loopback)\n");
    std::vector<double> before = FrameDelays(false, 180), after = FrameDelays(true, 180);
    Check(before.size() > 150 && after.size() > 150, "flush: messages arrive",
          Fmt("%zu / %zu", before.size(), after.size()));
    const double b50 = Pct(before, 0.5), a50 = Pct(after, 0.5), a95 = Pct(after, 0.95), b95 = Pct(before, 0.95);
    Check(a50 < 3 && b50 > 10, "flush: no frame of waiting for the next Poll",
          Fmt("without Flush p50 %.2f p95 %.2f ms; with Flush p50 %.2f p95 %.2f ms", b50, b95, a50, a95));
}

constexpr uint16_t kMsgSeq = 0x7002;

void TestLossRecovery() {
    std::printf("-- reliable delivery under loss (fast retransmit)\n");
    SessionConfig cfg;
    UdpOptions lossy;
    lossy.simLoss = 0.10f;
    lossy.simLatencyMs = 20;
    lossy.simSeed = 99;
    UdpOptions plain;
    plain.simLatencyMs = 20;
    Node host("host", cfg, plain), guest("guest", cfg);
    std::vector<double> delays;
    host.session.On(kMsgSeq, [&](PlayerId, ByteReader& r) {
        const double sent = r.F64();
        if (r.Ok()) delays.push_back((Seconds() - sent) * 1000);
    });
    if (!Connect(host, guest)) {
        Check(false, "loss: connect");
        return;
    }
    guest.transport.SetOptions(lossy);
    // 60 reliable messages per second for 6 s, both sides polled every ~1 ms
    const double frame = 1 / 60.0;
    double next = Seconds();
    int sent = 0;
    for (int f = 0; f < 360; ++f) {
        ByteWriter w(16);
        w.F64(Seconds());
        guest.session.SendToHost(kMsgSeq, w.Span(), Reliability::Reliable);
        guest.session.Flush();
        ++sent;
        next += frame;
        while (Seconds() < next) {
            host.session.Poll();
            guest.session.Poll();
            SpinUntil(std::min(next, Seconds() + 0.001));
        }
    }
    const double until = Seconds() + 2;
    while (Seconds() < until && static_cast<int>(delays.size()) < sent) {
        host.session.Poll();
        guest.session.Poll();
        Sleep(1);
    }
    PeerStats st;
    for (const auto& p : guest.session.Players())
        if (p.id == kHostPlayer) guest.transport.GetStats(p.peer, st);
    Check(static_cast<int>(delays.size()) == sent, "loss: every reliable message arrives",
          Fmt("%zu / %d", delays.size(), sent));
    const double p50 = Pct(delays, 0.5), p95 = Pct(delays, 0.95), mx = Pct(delays, 1.0);
    Check(st.fastResends > 0, "loss: losses repaired by fast retransmit",
          Fmt("resends %u, of them fast %u", st.resends, st.fastResends));
    // 20 ms each way + 1 ms polling: ~21 ms; a loss costs about one round trip more (in order: later ones wait)
    Check(p95 < 110, "loss: delivery p95 within ~2 round trips",
          Fmt("p50 %.1f p95 %.1f max %.1f ms (rtt %.0f ms)", p50, p95, mx, st.rttMs));
}

constexpr uint16_t kMsgLaneA = 0x7101, kMsgLaneB = 0x7201, kMsgLane0 = 0x7301;

void TestLanes() {
    std::printf("-- lanes (head-of-line blocking)\n");
    SessionConfig cfg;
    bool dropA = true;
    int droppedA = 0;
    UdpOptions gopt;
    // Drop the first datagram that carries message A ("AAAA") once.
    gopt.testDrop = [&](std::span<const uint8_t> d) {
        if (!dropA) return false;
        for (size_t i = 0; i + 4 <= d.size(); ++i)
            if (std::memcmp(d.data() + i, "AAAA", 4) == 0) {
                dropA = false;
                ++droppedA;
                return true;
            }
        return false;
    };
    for (int round = 0; round < 2; ++round) {
        const bool laned = round == 0;
        Node host("host", cfg), guest("guest", cfg, gopt);
        if (laned) {
            for (Session* s : {&host.session, &guest.session}) {
                s->SetLane(0x7100, 0x71FF, 1);
                s->SetLane(0x7200, 0x72FF, 2);
            }
        }
        std::vector<std::string> order;
        std::vector<double> at;
        auto rec = [&](const char* n) {
            return [&, n](PlayerId, ByteReader& r) {
                order.push_back(n + std::to_string(r.U8()));
                at.push_back(Seconds());
            };
        };
        host.session.On(kMsgLaneA, rec("A"));
        host.session.On(kMsgLaneB, rec("B"));
        host.session.On(kMsgLane0, rec("C"));
        if (!Connect(host, guest)) {
            Check(false, "lanes: connect");
            return;
        }
        for (int i = 0; i < 30; ++i) {  // settle the rtt estimate
            host.session.Poll();
            guest.session.Poll();
            Sleep(2);
        }
        dropA = true;
        auto send = [&](uint16_t id, uint8_t n, bool marker) {
            ByteWriter w(8);
            w.U8(n);
            if (marker) w.Bytes(reinterpret_cast<const uint8_t*>("AAAA"), 4);
            guest.session.SendToHost(id, w.Span(), Reliability::Reliable);
            guest.session.Flush();  // its own datagram
        };
        const double t0 = Seconds();
        send(kMsgLaneA, 1, true);   // lost once
        send(kMsgLaneB, 1, false);  // lane 2: does not wait for A1
        send(kMsgLaneA, 2, false);  // lane 1: after A1
        send(kMsgLane0, 1, false);  // lane 0: does not wait for the lanes
        send(kMsgLaneB, 2, false);  // lane 2, after C1 (lane 0): after C1 and B1
        const double until = Seconds() + 2;
        while (Seconds() < until && order.size() < 5) {
            host.session.Poll();
            guest.session.Poll();
            Sleep(0);
        }
        std::string got;
        for (auto& o : order) got += o + " ";
        const double bDelay = order.size() >= 1 ? (at[0] - t0) * 1000 : -1;
        if (laned) {
            auto pos = [&](const char* n) {
                return static_cast<int>(std::find(order.begin(), order.end(), n) - order.begin());
            };
            Check(order.size() == 5 && order[0] == "B1" && pos("C1") < pos("A1") && pos("B2") < pos("A1"),
                  "lanes: lane 2 and lane 0 not held back by lane 1's loss",
                  Fmt("order %s, B1 after %.2f ms", got.c_str(), bDelay));
            Check(pos("A1") < pos("A2") && pos("B1") < pos("B2") && pos("C1") < pos("B2"),
                  "lanes: lane order, lanes after earlier lane 0", got);
        } else {
            const double firstDelay = bDelay;
            Check(order.size() == 5 && order[0] == "A1", "one lane: everything waits for the lost message",
                  Fmt("order %s, first after %.2f ms", got.c_str(), firstDelay));
        }
    }
    Check(droppedA == 2, "lanes: the test dropped A1 in both rounds", Fmt("%d", droppedA));
}

constexpr uint16_t kMsgClock = 0x7003;

void TestClock() {
    std::printf("-- session clock\n");
    SessionConfig hc, gc;
    hc.testClockSkewS = 12345.678;  // the host's clock is far from ours
    gc.testClockSkewS = -42.0;
    UdpOptions opt;
    opt.simLatencyMs = 15;
    opt.simJitterMs = 3;
    Node host("host", hc, opt), guest("guest", gc, opt);
    std::vector<double> oneWay, ages;
    host.session.On(kMsgClock, [&](PlayerId, ByteReader& r) {
        const double stamp = r.F64();
        if (r.Ok()) oneWay.push_back((host.session.SessionTime() - stamp) * 1000);
        if (host.session.MessageAgeMs() >= 0) ages.push_back(host.session.MessageAgeMs());
    });
    const double t0 = Seconds();
    if (!Connect(host, guest)) {
        Check(false, "clock: connect");
        return;
    }
    double syncedAfter = -1;
    const double until = Seconds() + 3;
    double next = Seconds();
    while (Seconds() < until) {
        host.session.Poll();
        guest.session.Poll();
        if (syncedAfter < 0 && guest.session.ClockSynced()) syncedAfter = Seconds() - t0;
        if (guest.session.ClockSynced() && Seconds() >= next) {
            next = Seconds() + 0.05;
            ByteWriter w(16);
            w.F64(guest.session.SessionTime());
            guest.session.SendToHost(kMsgClock, w.Span(), Reliability::Unreliable);
            guest.session.Flush();
        }
        SpinUntil(Seconds() + 0.0005);
    }
    const double err = (guest.session.SessionTime() - host.session.SessionTime()) * 1000;
    Check(syncedAfter > 0 && syncedAfter < 1.0, "clock: synced soon after the join", Fmt("%.2f s", syncedAfter));
    Check(std::fabs(err) < 5, "clock: guest reads the host's clock",
          Fmt("error %.2f ms (estimate +-%.1f ms)", err, guest.session.ClockErrorMs()));
    const double p50 = Pct(oneWay, 0.5);
    // 15 ms simulated one way (+-3 jitter) plus up to ~0.5 ms polling
    Check(oneWay.size() > 20 && p50 > 10 && p50 < 21, "clock: one-way delay from stamps",
          Fmt("p50 %.2f p95 %.2f ms over %zu", p50, Pct(oneWay, 0.95), oneWay.size()));
    const double a50 = Pct(ages, 0.5);
    Check(ages.size() + 2 >= oneWay.size() && std::fabs(a50 - p50) < 2, "clock: MessageAgeMs (core send stamp)",
          Fmt("p50 %.1f ms over %zu", a50, ages.size()));
    const auto& tr = host.session.Traffic();
    const MsgTraffic t = tr.count(kMsgClock) ? tr.at(kMsgClock) : MsgTraffic{};
    Check(t.agedMsgs > 0 && t.ageMaxMs > 0, "clock: per-message age in Traffic()",
          Fmt("%llu aged, mean %.1f max %.1f ms", static_cast<unsigned long long>(t.agedMsgs),
              t.agedMsgs ? t.ageSumMs / static_cast<double>(t.agedMsgs) : 0.0, t.ageMaxMs));
}

constexpr uint16_t kMsgRelay = 0x7004;

// Guest -> host -> guest keeps the first sender's stamp: the second guest sees both hops.
void TestRelayAge() {
    std::printf("-- message age through the host relay\n");
    SessionConfig cfg;
    UdpOptions opt;
    opt.simLatencyMs = 10;
    Node host("host", cfg, opt), a("a", cfg, opt), b("b", cfg, opt);
    if (!host.session.Host(0)) return;
    const std::string addr = "127.0.0.1:" + std::to_string(host.transport.BoundPort());
    a.session.Join(addr);
    b.session.Join(addr);
    std::vector<double> atHost, atB;
    bool hostOwnZero = true;
    host.session.On(kMsgRelay, [&](PlayerId from, ByteReader&) {
        if (from == kHostPlayer) hostOwnZero &= host.session.MessageAgeMs() == 0;
        else if (host.session.MessageAgeMs() >= 0) atHost.push_back(host.session.MessageAgeMs());
    });
    b.session.On(kMsgRelay, [&](PlayerId, ByteReader&) {
        if (b.session.MessageAgeMs() >= 0) atB.push_back(b.session.MessageAgeMs());
    });
    const double until = Seconds() + 3;
    double next = Seconds() + 1;
    while (Seconds() < until) {
        host.session.Poll();
        a.session.Poll();
        b.session.Poll();
        if (Seconds() >= next && a.session.ClockSynced() && b.session.ClockSynced()) {
            next = Seconds() + 0.05;
            const uint8_t x = 1;
            a.session.Broadcast(kMsgRelay, std::span<const uint8_t>(&x, 1), Reliability::Reliable);
            a.session.Flush();
            host.session.SendToHost(kMsgRelay, std::span<const uint8_t>(&x, 1));
        }
        SpinUntil(Seconds() + 0.0005);
    }
    const double h50 = Pct(atHost, 0.5), b50 = Pct(atB, 0.5);
    Check(atHost.size() > 10 && atB.size() > 10 && h50 > 8 && h50 < 14 && b50 > 18 && b50 < 26 && hostOwnZero,
          "relay: age covers both hops", Fmt("host p50 %.1f ms, other guest p50 %.1f ms (10 ms per hop)", h50, b50));
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
    SetLogSink(&PrintLog);
    timeBeginPeriod(1);
    TestPlayout();
    TestFlush();
    TestLossRecovery();
    TestLanes();
    TestClock();
    TestRelayAge();
    timeEndPeriod(1);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
