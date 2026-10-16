#include "net/interest.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mc::net {

using namespace interest;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr double kNeverSentBonus = 1000.0;  // a just-entered entity outranks every staleness ratio of its class
constexpr int32_t kCellLimit = 1 << 30;

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

int32_t CellOf(float coord, float cell) {
    double c = std::floor(static_cast<double>(coord) / cell);
    if (c < -kCellLimit) c = -kCellLimit;
    if (c > kCellLimit) c = kCellLimit;
    return static_cast<int32_t>(c);
}

uint32_t CellHash(int32_t cx, int32_t cy) {
    uint32_t h = static_cast<uint32_t>(cx) * 0x9E3779B1u ^ static_cast<uint32_t>(cy) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

float Dist2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

Config::Config() {
    KindConfig& player = kinds[static_cast<int>(Kind::Player)];
    player.alwaysRelevant = true;
    player.priority = Priority::Critical;
    player.nearInterval = player.farInterval = 0.05;

    KindConfig& pcar = kinds[static_cast<int>(Kind::PlayerCar)];
    pcar.alwaysRelevant = true;
    pcar.priority = Priority::Critical;
    pcar.nearInterval = pcar.farInterval = 0.05;

    KindConfig& npc = kinds[static_cast<int>(Kind::Npc)];
    npc.priority = Priority::Normal;
    npc.nearDistance = 50;
    npc.nearInterval = 0.05;
    npc.farInterval = 0.5;
    npc.restSpeed = 0.2f;
    npc.restIntervalScale = 2;

    KindConfig& veh = kinds[static_cast<int>(Kind::Vehicle)];
    veh.priority = Priority::Normal;
    veh.nearDistance = 80;
    veh.nearInterval = 0.05;
    veh.farInterval = 0.4;
    veh.restSpeed = 0.3f;
    veh.restIntervalScale = 4;  // parked cars

    KindConfig& obj = kinds[static_cast<int>(Kind::Object)];
    obj.enterRadius = 150;
    obj.leaveRadius = 180;
    obj.priority = Priority::Low;
    obj.nearDistance = 30;
    obj.nearInterval = 0.1;
    obj.farInterval = 1.0;
    obj.restSpeed = 0.1f;
    obj.restIntervalScale = 2;
}

InterestManager::InterestManager(Config cfg) : cfg_(cfg) {
    if (!(cfg_.cellSize > 1)) cfg_.cellSize = 64;
    if (!(cfg_.burstSeconds > 0)) cfg_.burstSeconds = 0.25;
    for (KindConfig& k : cfg_.kinds) {
        if (!(k.enterRadius >= 0)) k.enterRadius = 0;
        if (!(k.leaveRadius >= k.enterRadius)) k.leaveRadius = k.enterRadius;
        if (k.priority == Priority::Default) k.priority = Priority::Normal;
        if (!(k.nearInterval >= 0)) k.nearInterval = 0;
        if (!(k.farInterval >= k.nearInterval)) k.farInterval = k.nearInterval;
        if (!(k.restIntervalScale > 0)) k.restIntervalScale = 1;
        if (!(k.maxInterval > 0)) k.maxInterval = 2.0;
        if (!k.alwaysRelevant) queryRadius_ = std::max(queryRadius_, k.leaveRadius);
    }
}

// ---- entities ------------------------------------------------------------------------------------------------------

InterestManager::Entity* InterestManager::Find(EntityId id) {
    auto it = ids_.find(id);
    return it == ids_.end() ? nullptr : &ents_[it->second];
}

bool InterestManager::AddEntity(const EntityDesc& d) {
    if (ids_.count(d.id) || d.kind >= Kind::Count || !Finite(d.pos)) return false;
    uint32_t slot;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        slot = static_cast<uint32_t>(ents_.size());
        ents_.emplace_back();
        for (Peer& p : peers_) p.ents.resize(ents_.size());
    }
    for (Peer& p : peers_) p.ents[slot] = PeerEnt{};
    const KindConfig& kc = cfg_.kinds[static_cast<int>(d.kind)];
    Entity& e = ents_[slot];
    e = Entity{};
    e.id = d.id;
    e.owner = d.owner;
    e.kind = d.kind;
    e.priority = d.priority >= Priority::Default ? kc.priority : d.priority;
    e.pos = d.pos;
    e.hasVelocity = d.hasVelocity && Finite(d.vel);
    if (e.hasVelocity) e.vel = d.vel;
    e.size = d.sizeBytes;
    e.always = d.alwaysRelevant || kc.alwaysRelevant;
    e.alive = true;
    ids_.emplace(d.id, slot);
    return true;
}

bool InterestManager::RemoveEntity(EntityId id) {
    auto it = ids_.find(id);
    if (it == ids_.end()) return false;
    uint32_t slot = it->second;
    for (Peer& p : peers_) {
        PeerEnt& pe = p.ents[slot];
        if (pe.inSet) pendingEvents_.push_back({p.key, id, EventType::Leave, LeaveReason::Removed});
        pe = PeerEnt{};
    }
    ents_[slot].alive = false;
    freeSlots_.push_back(slot);
    ids_.erase(it);
    return true;
}

bool InterestManager::UpdateEntity(EntityId id, const Vec3& pos) {
    Entity* e = Find(id);
    if (!e || !Finite(pos)) return false;
    e->pos = pos;
    return true;
}

bool InterestManager::UpdateEntity(EntityId id, const Vec3& pos, const Vec3& vel) {
    Entity* e = Find(id);
    if (!e || !Finite(pos)) return false;
    e->pos = pos;
    e->hasVelocity = Finite(vel);
    e->vel = e->hasVelocity ? vel : Vec3{};
    return true;
}

bool InterestManager::SetEntityOwner(EntityId id, PeerKey owner) {
    Entity* e = Find(id);
    if (!e) return false;
    e->owner = owner;
    return true;
}

bool InterestManager::SetEntityPriority(EntityId id, Priority p) {
    Entity* e = Find(id);
    if (!e) return false;
    e->priority = p >= Priority::Default ? cfg_.kinds[static_cast<int>(e->kind)].priority : p;
    return true;
}

bool InterestManager::SetEntitySize(EntityId id, uint32_t bytes) {
    Entity* e = Find(id);
    if (!e) return false;
    e->size = bytes;
    return true;
}

// ---- peers ---------------------------------------------------------------------------------------------------------

InterestManager::Peer* InterestManager::FindPeer(PeerKey key) {
    for (Peer& p : peers_)
        if (p.key == key) return &p;
    return nullptr;
}

const InterestManager::Peer* InterestManager::FindPeer(PeerKey key) const {
    for (const Peer& p : peers_)
        if (p.key == key) return &p;
    return nullptr;
}

double InterestManager::BucketCap(const Peer& p) const {
    return std::max(static_cast<double>(p.budget) * cfg_.burstSeconds,
                    static_cast<double>(maxSize_) + cfg_.perMessageOverhead);
}

bool InterestManager::AddPeer(PeerKey key) {
    if (FindPeer(key)) return false;
    Peer& p = peers_.emplace_back();
    p.key = key;
    p.budget = cfg_.bytesPerSecond;
    p.ents.resize(ents_.size());
    p.tokens = BucketCap(p);
    return true;
}

bool InterestManager::RemovePeer(PeerKey key) {
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        if (it->key == key) {
            peers_.erase(it);
            return true;
        }
    }
    return false;
}

bool InterestManager::SetPeerViewpoints(PeerKey key, std::span<const Vec3> points) {
    Peer* p = FindPeer(key);
    if (!p) return false;
    p->viewCount = 0;
    for (const Vec3& v : points) {
        if (p->viewCount >= kMaxViewpoints) break;
        if (Finite(v)) p->views[p->viewCount++] = v;
    }
    return true;
}

bool InterestManager::SetPeerBudget(PeerKey key, uint32_t bytesPerSecond) {
    Peer* p = FindPeer(key);
    if (!p) return false;
    p->budget = bytesPerSecond;
    p->tokens = std::min(p->tokens, BucketCap(*p));
    return true;
}

bool InterestManager::ChargeBytes(PeerKey key, uint32_t bytes) {
    Peer* p = FindPeer(key);
    if (!p) return false;
    p->tokens = std::max(p->tokens - bytes, -BucketCap(*p));
    p->stats.totalBytes += bytes;
    return true;
}

std::span<const SendItem> InterestManager::SendList(PeerKey key) const {
    const Peer* p = FindPeer(key);
    if (!p) return {};
    return p->sendList;
}

const PeerStats* InterestManager::Stats(PeerKey key) const {
    const Peer* p = FindPeer(key);
    return p ? &p->stats : nullptr;
}

bool InterestManager::IsRelevant(PeerKey key, EntityId id) const {
    const Peer* p = FindPeer(key);
    auto it = ids_.find(id);
    return p && it != ids_.end() && p->ents[it->second].inSet;
}

// ---- tick ----------------------------------------------------------------------------------------------------------

void InterestManager::BuildGrid() {
    always_.clear();
    maxSize_ = 0;
    uint32_t gridCount = 0;
    for (Entity& e : ents_) {
        if (!e.alive) continue;
        maxSize_ = std::max(maxSize_, e.size);
        if (e.always) continue;
        e.cx = CellOf(e.pos.x, cfg_.cellSize);
        e.cy = CellOf(e.pos.y, cfg_.cellSize);
        ++gridCount;
    }
    uint32_t nb = 64;
    while (nb < gridCount * 2u) nb <<= 1;
    bucketMask_ = nb - 1;
    bucketStart_.assign(nb + 1, 0);
    gridSlots_.resize(gridCount);
    // counting sort by bucket: count, inclusive prefix (= bucket end), then place back to front
    for (const Entity& e : ents_)
        if (e.alive && !e.always) ++bucketStart_[CellHash(e.cx, e.cy) & bucketMask_];
    for (uint32_t b = 1; b < nb; ++b) bucketStart_[b] += bucketStart_[b - 1];
    bucketStart_[nb] = gridCount;
    for (uint32_t slot = static_cast<uint32_t>(ents_.size()); slot-- > 0;) {
        const Entity& e = ents_[slot];
        if (!e.alive) continue;
        if (e.always) continue;
        gridSlots_[--bucketStart_[CellHash(e.cx, e.cy) & bucketMask_]] = slot;
    }
    for (uint32_t slot = 0; slot < ents_.size(); ++slot)
        if (ents_[slot].alive && ents_[slot].always) always_.push_back(slot);
}

void InterestManager::QueryPeer(Peer& p, float r) {
    const float cell = cfg_.cellSize;
    const float r2 = r * r;
    for (int v = 0; v < p.viewCount; ++v) {
        const Vec3& vp = p.views[v];
        int32_t x0 = CellOf(vp.x - r, cell), x1 = CellOf(vp.x + r, cell);
        int32_t y0 = CellOf(vp.y - r, cell), y1 = CellOf(vp.y + r, cell);
        for (int32_t cy = y0; cy <= y1; ++cy) {
            // nearest point of the cell row/column to the viewpoint (2D) -> skip cells outside the circle
            float ny = std::clamp(vp.y, static_cast<float>(cy) * cell, static_cast<float>(cy + 1) * cell) - vp.y;
            for (int32_t cx = x0; cx <= x1; ++cx) {
                float nx = std::clamp(vp.x, static_cast<float>(cx) * cell, static_cast<float>(cx + 1) * cell) - vp.x;
                if (nx * nx + ny * ny > r2) continue;
                uint32_t b = CellHash(cx, cy) & bucketMask_;
                for (uint32_t i = bucketStart_[b], end = bucketStart_[b + 1]; i < end; ++i) {
                    uint32_t slot = gridSlots_[i];
                    const Entity& e = ents_[slot];
                    if (e.cx != cx || e.cy != cy) continue;  // hash collision
                    float d2 = Dist2(e.pos, vp);
                    PeerEnt& pe = p.ents[slot];
                    if (pe.stamp != p.stamp) {
                        pe.stamp = p.stamp;
                        pe.d2 = d2;
                        p.touched.push_back(slot);
                    } else if (d2 < pe.d2) {
                        pe.d2 = d2;
                    }
                }
            }
        }
    }
}

double InterestManager::Interval(const Entity& e, float dist) const {
    const KindConfig& kc = cfg_.kinds[static_cast<int>(e.kind)];
    double t = 0;
    if (kc.enterRadius > kc.nearDistance)
        t = std::clamp((static_cast<double>(dist) - kc.nearDistance) / (kc.enterRadius - kc.nearDistance), 0.0, 1.0);
    else
        t = dist > kc.nearDistance ? 1.0 : 0.0;
    double iv = kc.nearInterval + (kc.farInterval - kc.nearInterval) * t;
    if (kc.restSpeed > 0 && e.hasVelocity && Dist2(e.vel, {}) < kc.restSpeed * kc.restSpeed) {
        iv *= kc.restIntervalScale;
        iv = std::min(iv, std::max(kc.maxInterval, kc.farInterval));
    }
    return iv;
}

void InterestManager::UpdatePeer(Peer& p, double now, double dt) {
    const double cap = BucketCap(p);
    p.tokens = std::min(p.tokens + static_cast<double>(p.budget) * dt, cap);
    uint64_t total = p.stats.totalBytes;
    p.stats = PeerStats{};
    p.stats.totalBytes = total;

    auto ownerBlocked = [&](const Entity& e) { return !cfg_.sendToOwner && e.owner == p.key; };
    auto nearestD2 = [&](const Entity& e) {
        if (p.viewCount == 0) return 0.0f;
        float best = kInf;
        for (int v = 0; v < p.viewCount; ++v) best = std::min(best, Dist2(e.pos, p.views[v]));
        return best;
    };
    auto enter = [&](uint32_t slot, PeerEnt& pe, float d2) {
        pe.inSet = true;
        pe.sent = false;
        pe.enteredAt = now;
        pe.d2 = d2;
        p.inSetNext.push_back(slot);
        tickEvents_.push_back({p.key, ents_[slot].id, EventType::Enter, LeaveReason::None});
    };
    auto leave = [&](PeerEnt& pe, const Entity& e, LeaveReason why) {
        pe.inSet = false;
        pe.sent = false;
        tickEvents_.push_back({p.key, e.id, EventType::Leave, why});
    };

    // 1. currently relevant: keep or leave (hysteresis: leave only beyond leaveRadius)
    p.inSetNext.clear();
    for (uint32_t slot : p.inSet) {
        PeerEnt& pe = p.ents[slot];
        if (!pe.inSet) continue;  // removed (and maybe reused) since the last tick
        const Entity& e = ents_[slot];
        if (ownerBlocked(e)) {
            leave(pe, e, LeaveReason::OwnerChanged);
            continue;
        }
        if (e.always) {
            pe.d2 = nearestD2(e);
        } else {
            float d2 = pe.stamp == p.stamp ? pe.d2 : kInf;
            float lr = cfg_.kinds[static_cast<int>(e.kind)].leaveRadius;
            if (d2 > lr * lr) {
                leave(pe, e, LeaveReason::OutOfRange);
                continue;
            }
            pe.d2 = d2;
        }
        p.inSetNext.push_back(slot);
    }
    // 2. found by the grid query: enter within enterRadius
    for (uint32_t slot : p.touched) {
        PeerEnt& pe = p.ents[slot];
        if (pe.inSet) continue;
        const Entity& e = ents_[slot];
        if (ownerBlocked(e)) continue;
        float er = cfg_.kinds[static_cast<int>(e.kind)].enterRadius;
        if (pe.d2 <= er * er) enter(slot, pe, pe.d2);
    }
    // 3. always relevant
    for (uint32_t slot : always_) {
        PeerEnt& pe = p.ents[slot];
        if (pe.inSet) continue;
        const Entity& e = ents_[slot];
        if (ownerBlocked(e)) continue;
        enter(slot, pe, nearestD2(e));
    }
    std::swap(p.inSet, p.inSetNext);

    // 4. due entities, scored by priority weight x staleness (time since last send / its interval)
    cands_.clear();
    const double slack = 0.5 * dt + 1e-6;  // tick quantisation: due when the interval ends within half a tick
    for (uint32_t slot : p.inSet) {
        PeerEnt& pe = p.ents[slot];
        const Entity& e = ents_[slot];
        pe.dist = std::sqrt(pe.d2);
        double iv = std::max(Interval(e, pe.dist), 1e-3);
        bool critical = e.priority == Priority::Critical;
        double weight = cfg_.priorityWeight[static_cast<int>(e.priority)];
        double score;
        if (!pe.sent) {
            score = weight * (kNeverSentBonus + (now - pe.enteredAt) / iv);
        } else {
            double since = now - pe.lastSent;
            if (since + slack < iv) continue;
            score = weight * since / iv;
        }
        cands_.push_back({score, slot, critical});
    }
    p.stats.relevant = static_cast<uint32_t>(p.inSet.size());
    p.stats.due = static_cast<uint32_t>(cands_.size());
    std::sort(cands_.begin(), cands_.end(), [&](const Candidate& a, const Candidate& b) {
        if (a.critical != b.critical) return a.critical;
        if (a.score != b.score) return a.score > b.score;
        return ents_[a.slot].id < ents_[b.slot].id;
    });

    // 5. fill the send list from the token bucket; Critical always goes, the rest stops at the first that does not fit
    //    (head of line: the best-scored entity is never overtaken by smaller ones, so large messages cannot starve)
    p.sendList.clear();
    for (const Candidate& c : cands_) {
        const Entity& e = ents_[c.slot];
        uint32_t cost = e.size + cfg_.perMessageOverhead;
        if (!c.critical && static_cast<double>(cost) > p.tokens) break;
        PeerEnt& pe = p.ents[c.slot];
        p.tokens -= cost;
        p.sendList.push_back({e.id, cost, pe.dist, !pe.sent});
        pe.sent = true;
        pe.lastSent = now;
        p.stats.bytes += cost;
    }
    p.tokens = std::max(p.tokens, -cap);
    p.stats.sent = static_cast<uint32_t>(p.sendList.size());
    p.stats.deferred = p.stats.due - p.stats.sent;
    p.stats.totalBytes += p.stats.bytes;
    p.stats.tokens = p.tokens;
}

void InterestManager::Tick(double now) {
    double dt = ticked_ ? std::clamp(now - lastNow_, 0.0, 1.0) : 0.0;
    lastNow_ = now;
    ticked_ = true;

    events_.clear();
    events_.insert(events_.end(), pendingEvents_.begin(), pendingEvents_.end());
    pendingEvents_.clear();

    BuildGrid();
    for (Peer& p : peers_) {
        ++p.stamp;
        p.touched.clear();
        if (queryRadius_ > 0 && !gridSlots_.empty()) QueryPeer(p, queryRadius_);
        tickEvents_.clear();
        UpdatePeer(p, now, dt);
        std::sort(tickEvents_.begin(), tickEvents_.end(), [](const Event& a, const Event& b) {
            if (a.type != b.type) return a.type == EventType::Leave;
            return a.entity < b.entity;
        });
        events_.insert(events_.end(), tickEvents_.begin(), tickEvents_.end());
    }
}

}  // namespace mc::net
