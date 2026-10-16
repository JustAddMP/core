// net_interest: unit tests for the interest manager (src/net/interest.h). Prints PASS/FAIL per check and the timing of
// the 2000 entities x 4 peers run; exits non-zero on any failure.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "net/interest.h"

using namespace mc::net;
using namespace mc::net::interest;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-48s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
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
    float Uniform(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Next() & 0xFFFFFF) / 16777216.0f; }
};

EntityDesc Npc(EntityId id, Vec3 pos, PeerKey owner = 0, Kind kind = Kind::Npc) {
    EntityDesc d;
    d.id = id;
    d.owner = owner;
    d.kind = kind;
    d.pos = pos;
    d.sizeBytes = 100;
    return d;
}

size_t CountEvents(const InterestManager& m, PeerKey peer, EntityId id, EventType type) {
    size_t n = 0;
    for (const Event& e : m.Events())
        if (e.peer == peer && e.entity == id && e.type == type) ++n;
    return n;
}

bool OwnerChangedLeave(const InterestManager& m, PeerKey peer, EntityId id) {
    for (const Event& e : m.Events())
        if (e.peer == peer && e.entity == id && e.type == EventType::Leave) return e.reason == LeaveReason::OwnerChanged;
    return false;
}

bool InSendList(const InterestManager& m, PeerKey peer, EntityId id) {
    for (const SendItem& s : m.SendList(peer))
        if (s.entity == id) return true;
    return false;
}

// ---- 1. enter/leave with hysteresis ----
void TestHysteresis() {
    InterestManager m;
    m.AddPeer(1);
    m.SetPeerViewpoint(1, {0, 0, 0});
    m.AddEntity(Npc(10, {290, 0, 0}));
    double t = 0;
    m.Tick(t);
    bool enter = m.Events().size() == 1 && CountEvents(m, 1, 10, EventType::Enter) == 1 && m.IsRelevant(1, 10);
    bool first = InSendList(m, 1, 10) && m.SendList(1)[0].first;

    std::string seq;  // relevance after each step
    const float steps[] = {320, 349, 360, 320, 301, 299, 340};
    size_t events = 0;
    for (float x : steps) {
        m.UpdateEntity(10, {x, 0, 0});
        m.Tick(t += 0.05);
        events += m.Events().size();
        seq += m.IsRelevant(1, 10) ? '1' : '0';
    }
    Check(enter && first, "hysteresis: enter at 290 m, first update flagged");
    // 320 in, 349 in, 360 out, 320 out, 301 out, 299 in, 340 in -> 2 events (leave at 360, enter at 299)
    Check(seq == "1100011" && events == 2, "hysteresis: 300 m enter / 350 m leave", "sequence " + seq);

    // leave event carries the reason
    m.UpdateEntity(10, {1000, 0, 0});
    m.Tick(t += 0.05);
    bool reason = m.Events().size() == 1 && m.Events()[0].type == EventType::Leave &&
                  m.Events()[0].reason == LeaveReason::OutOfRange;
    // 3D distance (height counts) and the nearest of several viewpoints
    m.UpdateEntity(10, {0, 0, 310});
    m.Tick(t += 0.05);
    bool height = !m.IsRelevant(1, 10);
    Vec3 views[2] = {{0, 0, 0}, {2000, 2000, 0}};
    m.SetPeerViewpoints(1, views);
    m.UpdateEntity(10, {2100, 2050, 0});
    m.Tick(t += 0.05);
    bool second = m.IsRelevant(1, 10) && CountEvents(m, 1, 10, EventType::Enter) == 1;
    // no viewpoints: nothing in range
    m.SetPeerViewpoints(1, {});
    m.Tick(t += 0.05);
    bool noView = !m.IsRelevant(1, 10);
    Check(reason && height && second && noView, "range: reason, 3D distance, 2nd viewpoint, none");

    // per-kind override: objects default to 150/180
    m.SetPeerViewpoint(1, {0, 0, 0});
    m.AddEntity(Npc(20, {170, 0, 0}, 0, Kind::Object));
    m.Tick(t += 0.05);
    bool objOut = !m.IsRelevant(1, 20);
    m.UpdateEntity(20, {140, 0, 0});
    m.Tick(t += 0.05);
    bool objIn = m.IsRelevant(1, 20);
    m.UpdateEntity(20, {175, 0, 0});
    m.Tick(t += 0.05);
    bool objStay = m.IsRelevant(1, 20);
    m.UpdateEntity(20, {185, 0, 0});
    m.Tick(t += 0.05);
    bool objLeave = !m.IsRelevant(1, 20);
    Check(objOut && objIn && objStay && objLeave, "per-kind radius override (object 150/180)");
}

// ---- 2. always-relevant kinds, owner filtering, owner change ----
void TestAlwaysRelevant() {
    InterestManager m;
    m.AddPeer(1);
    m.AddPeer(2);
    m.SetPeerViewpoint(1, {0, 0, 0});
    m.SetPeerViewpoint(2, {0, 0, 0});
    m.SetPeerBudget(2, 0);  // players ignore the budget
    EntityDesc player = Npc(1, {5000, 5000, 0}, 1, Kind::Player);
    EntityDesc car = Npc(2, {-8000, 0, 0}, 1, Kind::PlayerCar);
    m.AddEntity(player);
    m.AddEntity(car);
    double t = 0;
    int sentTicks = 0;
    bool ownerNever = true;
    for (int i = 0; i < 20; ++i) {
        m.Tick(t);
        t += 0.05;
        if (InSendList(m, 2, 1) && InSendList(m, 2, 2)) ++sentTicks;
        if (m.IsRelevant(1, 1) || m.IsRelevant(1, 2) || !m.SendList(1).empty()) ownerNever = false;
    }
    Check(m.IsRelevant(2, 1) && m.IsRelevant(2, 2) && sentTicks == 20,
          "always relevant: player + car at 7-8 km, every tick", Fmt("%.0f/20 ticks, budget 0", sentTicks));
    Check(ownerNever, "owner never receives its own entities");

    m.SetEntityOwner(2, 2);  // car hand-off: peer 2 drives it now
    m.Tick(t);
    bool handoff = CountEvents(m, 2, 2, EventType::Leave) == 1 && CountEvents(m, 1, 2, EventType::Enter) == 1 &&
                   OwnerChangedLeave(m, 2, 2) && m.IsRelevant(1, 2) && !m.IsRelevant(2, 2);
    // per-entity alwaysRelevant on a normal kind
    EntityDesc mission = Npc(3, {9000, 0, 0}, 0, Kind::Npc);
    mission.alwaysRelevant = true;
    m.AddEntity(mission);
    m.Tick(t += 0.05);
    Check(handoff && m.IsRelevant(1, 3) && m.IsRelevant(2, 3), "owner change -> Leave/Enter; per-entity always flag");
}

// ---- 3. budget cap and fairness under saturation ----
void TestBudget() {
    Config cfg;
    cfg.bytesPerSecond = 64 * 1024;  // the old default: small enough to saturate with 2000 entities
    cfg.burstSeconds = 0.1;
    InterestManager m(cfg);
    m.AddPeer(1);
    m.SetPeerViewpoint(1, {0, 0, 0});
    Rng rng(7);
    const int kN = 2000;
    std::vector<float> dist(kN);
    for (int i = 0; i < kN; ++i) {
        float r = 290.0f * std::sqrt(rng.Uniform(0, 1)), a = rng.Uniform(0, 6.2831853f);
        EntityDesc d = Npc(static_cast<EntityId>(100 + i), {r * std::cos(a), r * std::sin(a), 0});
        d.hasVelocity = true;
        d.vel = (i & 1) ? Vec3{3, 0, 0} : Vec3{0, 0, 0};  // half walking, half standing
        dist[i] = r;
        m.AddEntity(d);
    }
    const double dt = 0.05, duration = 30;
    const int ticks = static_cast<int>(duration / dt);
    std::vector<int> sends(kN, 0);
    std::vector<double> last(kN, -1), maxGap(kN, 0);
    uint64_t bytes = 0;
    double maxTick = 0;
    for (int k = 0; k < ticks; ++k) {
        double t = k * dt;
        m.Tick(t);
        uint64_t tickBytes = 0;
        for (const SendItem& s : m.SendList(1)) {
            int i = static_cast<int>(s.entity) - 100;
            ++sends[i];
            if (last[i] >= 0) maxGap[i] = std::max(maxGap[i], t - last[i]);
            last[i] = t;
            tickBytes += s.bytes;
        }
        bytes += tickBytes;
        maxTick = std::max(maxTick, static_cast<double>(tickBytes));
    }
    double end = (ticks - 1) * dt;
    double cap = cfg.bytesPerSecond * cfg.burstSeconds;
    double allowed = cfg.bytesPerSecond * end + cap;
    Check(bytes <= allowed && maxTick <= cap + 1, "budget: total and per tick within 64 KB/s",
          Fmt("%.0f B in %.2f s (limit %.0f), max tick %.0f B", static_cast<double>(bytes), end, allowed, maxTick));
    Check(bytes >= 0.95 * cfg.bytesPerSecond * end, "budget: saturated (>= 95 % used)",
          Fmt("%.1f %%", 100.0 * static_cast<double>(bytes) / (cfg.bytesPerSecond * end)));

    int never = 0;
    double worstGap = 0, nearSum = 0, farSum = 0;
    int nearN = 0, farN = 0;
    for (int i = 0; i < kN; ++i) {
        if (sends[i] == 0) ++never;
        // the gap still open at the end counts too
        double open = end - (last[i] < 0 ? 0 : last[i]);
        worstGap = std::max({worstGap, maxGap[i], open});
        if (dist[i] < 50) nearSum += sends[i], ++nearN;
        if (dist[i] > 250) farSum += sends[i], ++farN;
    }
    double nearAvg = nearSum / std::max(nearN, 1), farAvg = farSum / std::max(farN, 1);
    Check(never == 0 && worstGap <= 15, "fairness: every entity updated, bounded staleness",
          Fmt("never sent %.0f, worst gap %.2f s", never, worstGap));
    Check(nearAvg > 2 * farAvg && farAvg >= 2, "fairness: near updated more often, far still updated",
          Fmt("avg sends in 30 s: <50 m %.1f, >250 m %.1f", nearAvg, farAvg));

    // a message larger than one tick's budget is not starved by smaller ones (head of line)
    m.AddEntity([] {
        EntityDesc d = Npc(50, {20, 0, 0});
        d.sizeBytes = 9000;  // > budget x burst (6.5 KB): bucket depth grows to fit it
        d.priority = Priority::Low;
        return d;
    }());
    bool bigSent = false;
    for (int k = 0; k < 40 && !bigSent; ++k) {
        m.Tick(duration + k * dt);
        bigSent = InSendList(m, 1, 50);
    }
    Check(bigSent, "budget: oversized low-priority message still sent");
}

// ---- 4. update rate scales with distance (no budget pressure) ----
void TestRates() {
    Config cfg;
    cfg.bytesPerSecond = 10'000'000;
    InterestManager m(cfg);
    m.AddPeer(1);
    m.SetPeerViewpoint(1, {0, 0, 0});
    auto add = [&](EntityId id, float x, float speed) {
        EntityDesc d = Npc(id, {x, 0, 0});
        d.hasVelocity = true;
        d.vel = {speed, 0, 0};
        m.AddEntity(d);
    };
    add(1, 10, 5);     // near, moving
    add(2, 150, 5);    // middle
    add(3, 295, 5);    // far, moving
    add(4, 295, 0);    // far, standing (rest scale x2)
    std::map<EntityId, int> n;
    for (int k = 0; k < 40; ++k) {  // 2 s at 20 Hz
        m.Tick(k * 0.05);
        for (const SendItem& s : m.SendList(1)) ++n[s.entity];
    }
    bool ok = n[1] == 40 && n[2] > n[3] && n[2] < 40 && n[3] >= 3 && n[3] <= 6 && n[4] < n[3] && n[4] >= 2;
    Check(ok, "rates: near every tick, far less, resting least",
          "2 s: 10 m " + std::to_string(n[1]) + ", 150 m " + std::to_string(n[2]) + ", 295 m " + std::to_string(n[3]) +
              ", 295 m resting " + std::to_string(n[4]));
}

// ---- 5. priority ordering ----
void TestPriority() {
    Config cfg;
    cfg.bytesPerSecond = 2100;  // 105 B per 50 ms tick: exactly one 100 B entity per tick
    cfg.burstSeconds = 0.05;
    InterestManager m(cfg);
    m.AddPeer(1);
    m.SetPeerViewpoint(1, {0, 0, 0});
    const Priority prios[] = {Priority::Low, Priority::Normal, Priority::High};
    for (int i = 0; i < 3; ++i) {
        EntityDesc d = Npc(static_cast<EntityId>(1 + i), {10, static_cast<float>(i), 0});
        d.priority = prios[i];
        m.AddEntity(d);
    }
    std::string order;
    std::map<EntityId, int> n;
    for (int k = 0; k < 120; ++k) {
        m.Tick(k * 0.05);
        for (const SendItem& s : m.SendList(1)) {
            ++n[s.entity];
            if (k < 3) order += static_cast<char>('0' + s.entity);
        }
    }
    Check(order == "321", "priority: High, Normal, Low on a 1-per-tick budget", "first sends " + order);
    Check(n[3] > n[2] && n[2] > n[1] && n[1] >= 5, "priority: weighted share, Low not starved",
          Fmt("6 s: High %.0f, Normal %.0f, Low %.0f", n[3], n[2], n[1]));

    // Critical goes first and ignores the budget; ties broken by id (deterministic)
    EntityDesc crit = Npc(9, {10, 0, 0});  // near: due every tick
    crit.priority = Priority::Critical;
    m.AddEntity(crit);
    int critTicks = 0;
    bool firstInList = true;
    for (int k = 120; k < 160; ++k) {
        m.Tick(k * 0.05);
        auto list = m.SendList(1);
        if (InSendList(m, 1, 9)) ++critTicks;
        if (list.empty() || list[0].entity != 9) firstInList = false;
    }
    const PeerStats* st = m.Stats(1);
    Check(critTicks == 40 && firstInList && st && st->tokens >= -105.0 - 1e-9,
          "priority: Critical first every tick despite budget", Fmt("tokens %.0f", st ? st->tokens : 0));
}

// ---- 6. removal of entities and peers ----
void TestRemoval() {
    InterestManager m;
    m.AddPeer(1);
    m.AddPeer(2);
    m.SetPeerViewpoint(1, {0, 0, 0});
    m.SetPeerViewpoint(2, {10, 0, 0});
    for (EntityId id = 10; id < 15; ++id) m.AddEntity(Npc(id, {static_cast<float>(id), 0, 0}));
    double t = 0;
    m.Tick(t);
    bool dup = !m.AddEntity(Npc(10, {0, 0, 0}));
    bool removed = m.RemoveEntity(12) && !m.RemoveEntity(12) && !m.HasEntity(12);
    m.Tick(t += 0.05);
    bool leaves = m.Events().size() == 2 && m.Events()[0].reason == LeaveReason::Removed &&
                  m.Events()[0].peer == 1 && m.Events()[1].peer == 2 && !m.IsRelevant(1, 12) &&
                  !InSendList(m, 1, 12) && !InSendList(m, 2, 12);
    Check(dup && removed && leaves, "remove entity: Leave(Removed) to every peer");

    // remove + re-add the same id between ticks: Leave then Enter, fresh state (first update flagged)
    m.RemoveEntity(11);
    m.AddEntity(Npc(11, {5, 0, 0}));
    m.AddEntity(Npc(20, {6, 0, 0}));  // reuses the freed slot of 12
    m.Tick(t += 0.05);
    bool reAdd = m.Events().size() == 6 && m.Events()[0].type == EventType::Leave &&
                 m.Events()[0].entity == 11 && CountEvents(m, 1, 11, EventType::Enter) == 1 &&
                 CountEvents(m, 2, 20, EventType::Enter) == 1;
    int firsts = 0;
    for (const SendItem& s : m.SendList(1))
        if ((s.entity == 11 || s.entity == 20) && s.first) ++firsts;
    bool fresh = firsts == 2;
    Check(reAdd && fresh, "re-add id / reuse slot: Leave, Enter, first update", Fmt("%.0f events", static_cast<double>(m.Events().size())));

    // remove a peer: silent, others unaffected; re-add: everything enters again
    bool rp = m.RemovePeer(2) && !m.RemovePeer(2) && m.PeerCount() == 1;
    m.Tick(t += 0.05);
    bool silent = m.Events().empty() && m.SendList(2).empty() && !m.Stats(2) && m.IsRelevant(1, 10);
    m.AddPeer(2);
    m.SetPeerViewpoint(2, {0, 0, 0});
    m.Tick(t += 0.05);
    size_t enters = 0;
    for (const Event& e : m.Events())
        if (e.peer == 2 && e.type == EventType::Enter) ++enters;
    Check(rp && silent && enters == m.EntityCount(), "remove/re-add peer: silent removal, fresh enters",
          Fmt("%.0f enters for %.0f entities", static_cast<double>(enters), static_cast<double>(m.EntityCount())));

    // remove everything
    for (EntityId id : {10u, 11u, 13u, 14u, 20u}) m.RemoveEntity(id);
    m.Tick(t += 0.05);
    m.Tick(t += 0.05);
    Check(m.EntityCount() == 0 && m.SendList(1).empty() && m.Events().empty() && !m.UpdateEntity(10, {0, 0, 0}),
          "remove all entities: empty lists");
}

// ---- 7. determinism + the 2000 x 4 run ----
struct World {
    std::vector<Vec3> pos, vel;
    std::vector<Vec3> peers;
};

uint64_t RunScenario(InterestManager& m, int ticks, bool timed, double* avgUs, double* maxUs, double* p99Us,
                     uint32_t* relevantAvg, bool* invariants) {
    Rng rng(12345);
    const int kN = 2000, kPeers = 4;
    World w;
    for (int i = 0; i < kN; ++i) {
        w.pos.push_back({rng.Uniform(-1000, 1000), rng.Uniform(-1000, 1000), rng.Uniform(0, 20)});
        w.vel.push_back({rng.Uniform(-10, 10), rng.Uniform(-10, 10), 0});
        Kind kind = i < 1200 ? Kind::Npc : i < 1800 ? Kind::Vehicle : Kind::Object;
        EntityDesc d = Npc(static_cast<EntityId>(1000 + i), w.pos[i], 0, kind);
        d.hasVelocity = true;
        d.vel = w.vel[i];
        d.sizeBytes = 40 + rng.Next() % 100;
        if (i % 50 == 0) d.priority = Priority::High;
        m.AddEntity(d);
    }
    for (int p = 0; p < kPeers; ++p) {
        m.AddPeer(static_cast<PeerKey>(p + 1));
        w.peers.push_back({rng.Uniform(-600, 600), rng.Uniform(-600, 600), 0});
        // the players themselves
        m.AddEntity(Npc(static_cast<EntityId>(p + 1), w.peers[p], static_cast<PeerKey>(p + 1), Kind::Player));
    }
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    std::vector<double> times;
    times.reserve(ticks);
    uint64_t relevantSum = 0;
    const double dt = 0.05;
    for (int k = 0; k < ticks; ++k) {
        for (int i = 0; i < kN; ++i) {
            Vec3& p = w.pos[i];
            Vec3& v = w.vel[i];
            p.x += v.x * static_cast<float>(dt);
            p.y += v.y * static_cast<float>(dt);
            if (p.x < -1000 || p.x > 1000) v.x = -v.x;
            if (p.y < -1000 || p.y > 1000) v.y = -v.y;
            m.UpdateEntity(static_cast<EntityId>(1000 + i), p, v);
        }
        for (int p = 0; p < kPeers; ++p) {
            Vec3& pp = w.peers[p];
            pp.x += (p & 1 ? 15.0f : -15.0f) * static_cast<float>(dt);  // drive across the map
            pp.y += (p & 2 ? 8.0f : -8.0f) * static_cast<float>(dt);
            Vec3 views[2] = {pp, {pp.x + 3, pp.y, pp.z + 2}};  // player + camera
            m.SetPeerViewpoints(static_cast<PeerKey>(p + 1), views);
            m.UpdateEntity(static_cast<EntityId>(p + 1), pp);
        }
        auto t0 = std::chrono::steady_clock::now();
        m.Tick(k * dt);
        auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        for (const Event& e : m.Events()) mix((uint64_t(e.peer) << 40) ^ (uint64_t(e.entity) << 8) ^ uint64_t(e.type));
        for (int p = 0; p < kPeers; ++p) {
            for (const SendItem& s : m.SendList(static_cast<PeerKey>(p + 1))) mix(s.entity * 31ull + s.bytes);
            relevantSum += m.Stats(static_cast<PeerKey>(p + 1))->relevant;
        }
    }
    if (timed) {
        double sum = 0, mx = 0;
        for (double x : times) sum += x, mx = std::max(mx, x);
        std::vector<double> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        *avgUs = sum / static_cast<double>(times.size());
        *maxUs = mx;
        *p99Us = sorted[sorted.size() * 99 / 100];
        *relevantAvg = static_cast<uint32_t>(relevantSum / (static_cast<uint64_t>(ticks) * kPeers));
    }
    if (invariants) {  // brute force: every entity within enterRadius is relevant, every relevant one within leaveRadius
        bool ok = true;
        const Config& cfg = m.GetConfig();
        for (int p = 0; p < kPeers && ok; ++p) {
            Vec3 views[2] = {w.peers[p], {w.peers[p].x + 3, w.peers[p].y, w.peers[p].z + 2}};
            for (int i = 0; i < kN; ++i) {
                float best = 1e30f;
                for (const Vec3& v : views) {
                    float dx = w.pos[i].x - v.x, dy = w.pos[i].y - v.y, dz = w.pos[i].z - v.z;
                    best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                Kind kind = i < 1200 ? Kind::Npc : i < 1800 ? Kind::Vehicle : Kind::Object;
                const KindConfig& kc = cfg.kinds[static_cast<int>(kind)];
                bool rel = m.IsRelevant(static_cast<PeerKey>(p + 1), static_cast<EntityId>(1000 + i));
                if (best <= kc.enterRadius - 0.01f && !rel) ok = false;
                if (best > kc.leaveRadius + 0.01f && rel) ok = false;
            }
            for (int q = 0; q < kPeers; ++q)  // other players always, own player never
                if (m.IsRelevant(static_cast<PeerKey>(p + 1), static_cast<EntityId>(q + 1)) != (p != q)) ok = false;
        }
        *invariants = ok;
    }
    return h;
}

void TestScaleAndDeterminism() {
    double avg = 0, mx = 0, p99 = 0;
    uint32_t relevant = 0;
    bool inv = false;
    InterestManager a;
    uint64_t ha = RunScenario(a, 1000, true, &avg, &mx, &p99, &relevant, &inv);
    std::printf("      2000 entities x 4 peers (2 viewpoints each), 1000 ticks: avg %.1f us/tick, p99 %.1f us, "
                "max %.1f us, ~%u relevant per peer\n",
                avg, p99, mx, relevant);
    Check(avg < 2000 && relevant > 50, "perf: 2000 entities x 4 peers", Fmt("avg %.1f us/tick, p99 %.1f us", avg, p99));
    Check(inv, "grid relevance matches brute force (enter/leave radii)");
    InterestManager b;
    uint64_t hb = RunScenario(b, 200, false, nullptr, nullptr, nullptr, nullptr, nullptr);
    InterestManager c;
    uint64_t hc = RunScenario(c, 200, false, nullptr, nullptr, nullptr, nullptr, nullptr);
    Check(hb == hc && ha != 0, "determinism: identical runs give identical events/send lists");
}

}  // namespace

// ---- 3b. the default budget carries a busy co-op scene without deferring anything ----
// 4 players and their cars (Critical, 20 Hz, 160 B), 48 NPCs within 50 m (20 Hz) and 150 more up to 120 m, 40 moving
// cars nearby: about 300 KB/s for one peer. With the old 64 KB/s default most of it was deferred.
void TestDefaultBudget() {
    Config cfg;
    InterestManager m(cfg);
    m.AddPeer(1);
    m.SetPeerViewpoint(1, {0, 0, 0});
    EntityId id = 1;
    for (int i = 0; i < 4; ++i) {
        EntityDesc p = Npc(id++, {static_cast<float>(i * 5), 0, 0}, 2 + i, Kind::Player);
        p.sizeBytes = 160;
        m.AddEntity(p);
        EntityDesc c = Npc(id++, {static_cast<float>(i * 5), 3, 0}, 2 + i, Kind::PlayerCar);
        c.sizeBytes = 160;
        m.AddEntity(c);
    }
    Rng rng(11);
    for (int i = 0; i < 198; ++i) {
        float r = i < 48 ? 45.0f * std::sqrt(rng.Uniform(0, 1)) : 50 + 70 * rng.Uniform(0, 1);
        float a = rng.Uniform(0, 6.2831853f);
        EntityDesc d = Npc(id++, {r * std::cos(a), r * std::sin(a), 0});
        d.sizeBytes = 40;
        d.hasVelocity = true;
        d.vel = {2, 0, 0};
        m.AddEntity(d);
    }
    for (int i = 0; i < 40; ++i) {
        float r = 20 + 60 * rng.Uniform(0, 1), a = rng.Uniform(0, 6.2831853f);
        EntityDesc d = Npc(id++, {r * std::cos(a), r * std::sin(a), 0}, 0, Kind::Vehicle);
        d.sizeBytes = 60;
        d.hasVelocity = true;
        d.vel = {12, 0, 0};
        m.AddEntity(d);
    }
    uint64_t deferred = 0, bytes = 0;
    const double dt = 0.05;
    const int ticks = 200;
    for (int k = 0; k < ticks; ++k) {
        m.Tick(k * dt);
        const PeerStats* st = m.Stats(1);
        if (k > 0) deferred += st->deferred;  // the first tick sends everything at once (Enter)
        bytes += st->bytes;
    }
    const double rate = static_cast<double>(bytes) / ((ticks - 1) * dt);
    Check(cfg.bytesPerSecond >= 512 * 1024 && deferred == 0 && rate > 64 * 1024,
          "default budget: busy scene, nothing deferred",
          Fmt("%.0f KB/s used of %.0f, deferred %.0f", rate / 1024, cfg.bytesPerSecond / 1024.0,
              static_cast<double>(deferred)));
}

int main() {
    TestHysteresis();
    TestDefaultBudget();
    TestAlwaysRelevant();
    TestBudget();
    TestRates();
    TestPriority();
    TestRemoval();
    TestScaleAndDeterminism();
    std::printf(g_failures ? "FAILED: %d check(s)\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
