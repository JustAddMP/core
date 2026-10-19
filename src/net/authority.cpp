#include "net/authority.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mc::net::authority {
namespace {

float Dist2D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

// ---- NetRef ----------------------------------------------------------------------------------------------------
void NetRef::Write(ByteWriter& w) const {
    w.U8(home);
    w.U8(kind);
    w.U32(local);
}

NetRef NetRef::Read(ByteReader& r) {
    NetRef n;
    n.home = r.U8();
    n.kind = r.U8();
    n.local = r.U32();
    return n;
}

std::string NetRef::ToString() const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u:%u:%u", static_cast<unsigned>(home), static_cast<unsigned>(kind),
                  static_cast<unsigned>(local));
    return buf;
}

// ---- OwnershipBook ---------------------------------------------------------------------------------------------
ClaimResult OwnershipBook::Claim(const NetRef& ref, PlayerId by, uint16_t epoch) {
    Entry& e = book_[ref.Pack()];
    ClaimResult r;
    bool grant = false;
    if (by == kNoPlayer) {
        grant = false;
    } else if (e.owner == kNoPlayer && e.claimTick == UINT64_MAX && e.epoch == 0) {
        grant = true;  // never claimed
    } else if (epoch > e.epoch) {
        grant = true;
    } else if (epoch < e.epoch) {
        grant = false;  // stale
    } else if (e.owner == kNoPlayer || e.owner == by) {
        grant = true;
    } else {
        grant = e.claimTick == tick_ && by < e.owner;  // simultaneous claims: lower id
    }
    if (grant) {
        if (e.owner != by || epoch != e.epoch) e.claimTick = tick_;
        e.owner = by;
        e.epoch = epoch;
    }
    r.granted = grant;
    r.owner = e.owner;
    r.epoch = e.epoch;
    return r;
}

bool OwnershipBook::Release(const NetRef& ref, PlayerId by) {
    auto it = book_.find(ref.Pack());
    if (it == book_.end() || it->second.owner != by || by == kNoPlayer) return false;
    it->second.owner = kNoPlayer;
    it->second.claimTick = tick_;
    return true;
}

PlayerId OwnershipBook::Owner(const NetRef& ref) const {
    auto it = book_.find(ref.Pack());
    return it == book_.end() ? kNoPlayer : it->second.owner;
}

uint16_t OwnershipBook::Epoch(const NetRef& ref) const {
    auto it = book_.find(ref.Pack());
    return it == book_.end() ? 0 : it->second.epoch;
}

std::vector<NetRef> OwnershipBook::OnPlayerLeft(PlayerId id, PlayerId fallback) {
    std::vector<NetRef> moved;
    if (id == kNoPlayer) return moved;
    for (auto& [key, e] : book_) {
        if (e.owner != id) continue;
        e.owner = (fallback == id) ? kNoPlayer : fallback;
        e.epoch = static_cast<uint16_t>(e.epoch + 1);
        e.claimTick = UINT64_MAX;  // not a claim: no simultaneous-claim race with it
        moved.push_back(NetRef::Unpack(key));
    }
    return moved;
}

std::vector<NetRef> OwnershipBook::OwnedBy(PlayerId id) const {
    std::vector<NetRef> v;
    for (const auto& [key, e] : book_)
        if (e.owner == id) v.push_back(NetRef::Unpack(key));
    return v;
}

// ---- RegionAuthority -------------------------------------------------------------------------------------------
void RegionAuthority::SetPlayers(std::span<const PlayerSpot> players, PlayerId host) {
    players_.assign(players.begin(), players.end());
    std::stable_sort(players_.begin(), players_.end(),
                     [](const PlayerSpot& a, const PlayerSpot& b) { return a.id < b.id; });
    players_.erase(std::unique(players_.begin(), players_.end(),
                               [](const PlayerSpot& a, const PlayerSpot& b) { return a.id == b.id; }),
                   players_.end());
    host_ = host;
}

const PlayerSpot* RegionAuthority::Find(PlayerId id) const {
    for (const PlayerSpot& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

PlayerId RegionAuthority::AuthorityAt(const Vec3& pos) const {
    const PlayerSpot* h = Find(host_);
    if (h && h->inWorld && Dist2D(h->pos, pos) <= t_.hostRadius) return host_;
    PlayerId best = host_;
    float bestD = 0;
    bool any = false;
    for (const PlayerSpot& p : players_) {  // id order: ties keep the lower id
        if (!p.inWorld) continue;
        const float d = Dist2D(p.pos, pos);
        if (!any || d < bestD) {
            best = p.id;
            bestD = d;
            any = true;
        }
    }
    return best;
}

PlayerId RegionAuthority::Keep(PlayerId currentOwner, const Vec3& pos) const {
    const PlayerSpot* cur = Find(currentOwner);
    if (!cur || !cur->inWorld) return AuthorityAt(pos);
    const float dCur = Dist2D(cur->pos, pos);
    if (dCur <= t_.releaseRadius) return currentOwner;
    const PlayerId cand = AuthorityAt(pos);
    if (cand == currentOwner) return currentOwner;
    const PlayerSpot* c = Find(cand);
    if (!c || !c->inWorld) return currentOwner;
    return Dist2D(c->pos, pos) + t_.margin < dCur ? cand : currentOwner;
}

std::vector<Region> RegionAuthority::Regions() const {
    std::vector<Region> out;
    const PlayerSpot* h = Find(host_);
    out.push_back(Region{host_, h ? h->pos : Vec3{}});
    for (const PlayerSpot& p : players_) {
        if (p.id == host_ || !p.inWorld) continue;
        bool own = true;
        for (const Region& r : out) {
            const PlayerSpot* o = Find(r.owner);
            if (r.owner == host_ && (!o || !o->inWorld)) continue;  // host not in the world: no area of his
            if (Dist2D(p.pos, r.center) <= t_.releaseRadius) {
                own = false;
                break;
            }
        }
        if (own) out.push_back(Region{p.id, p.pos});
    }
    return out;
}

PlayerId RegionAuthority::RegionOf(PlayerId id) const {
    const PlayerSpot* p = Find(id);
    if (!p || !p->inWorld) return host_;
    const std::vector<Region> rs = Regions();
    for (const Region& r : rs)
        if (r.owner == id) return id;
    for (const Region& r : rs) {
        const PlayerSpot* o = Find(r.owner);
        if (o && o->inWorld && Dist2D(p->pos, r.center) <= t_.releaseRadius) return r.owner;
    }
    return host_;
}

}  // namespace mc::net::authority
