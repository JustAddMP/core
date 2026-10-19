// net: authority - one ownership scheme for everything simulated in a co-op session (docs/replication.md
// "Authority").
//
//   NetRef           a stable name for a networked thing: the player whose game created it (home), its kind and its
//                    id on that game. 6 bytes on the wire.
//   OwnershipBook    who simulates each NetRef now (its owner) and the claim epoch: a claim with a higher epoch wins,
//                    an equal epoch keeps the current owner, two claims of one epoch in the same tick go to the lower
//                    player id; a player leaving hands his refs to a fallback (the host).
//   RegionAuthority  which player's game simulates the ambient world (traffic, pedestrians) at a point: the host near
//                    himself, otherwise the nearest player in the world, with hysteresis so a point does not flip.
//
// Pure logic, deterministic (ordered containers only), no transport calls. Not thread-safe.
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "net/remote_motion.h"
#include "net/session.h"
#include "net/wire.h"

namespace mc::net::authority {

using motion::Vec3;

enum class RefKind : uint8_t { None = 0, PlayerCar = 1, Npc = 2, Traffic = 3, Pedestrian = 4, Door = 5, Item = 6, Object = 7 };

struct NetRef {
    uint8_t home = 0;   // player id whose game created it
    uint8_t kind = 0;   // RefKind
    uint32_t local = 0; // its id on the home game

    static constexpr size_t kWireSize = 6;
    uint64_t Pack() const { return (static_cast<uint64_t>(home) << 40) | (static_cast<uint64_t>(kind) << 32) | local; }
    static NetRef Unpack(uint64_t v) {
        return NetRef{static_cast<uint8_t>(v >> 40), static_cast<uint8_t>(v >> 32), static_cast<uint32_t>(v)};
    }
    void Write(ByteWriter& w) const;  // home u8, kind u8, local u32 (little endian, as ByteWriter)
    static NetRef Read(ByteReader& r);
    std::string ToString() const;     // "home:kind:local"
    bool operator==(const NetRef& o) const { return Pack() == o.Pack(); }
    bool operator<(const NetRef& o) const { return Pack() < o.Pack(); }
};

struct ClaimResult {
    bool granted = false;
    PlayerId owner = kNoPlayer;  // the owner after the claim (the winner)
    uint16_t epoch = 0;          // its epoch
};

class OwnershipBook {
public:
    // Ownership of ref by `by` with `epoch`. Rules, in order: an epoch lower than the current one is refused (stale);
    // a higher one wins; equal: no owner -> granted; the owner re-claiming -> granted; a different claimer: refused,
    // unless the current owner got the ref by a claim of this same epoch in this same tick and the claimer has the
    // lower player id (simultaneous claims: lower id wins).
    ClaimResult Claim(const NetRef& ref, PlayerId by, uint16_t epoch);
    // The owner gives it up (no owner, epoch kept). False when `by` is not the owner.
    bool Release(const NetRef& ref, PlayerId by);
    PlayerId Owner(const NetRef& ref) const;  // kNoPlayer: none
    uint16_t Epoch(const NetRef& ref) const;  // 0 for an unknown ref
    // Every ref owned by `id` goes to `fallback` with the epoch raised by one; returned in ref order. When the fallback
    // is the leaving player himself (or kNoPlayer) the refs become unowned instead (still returned).
    std::vector<NetRef> OnPlayerLeft(PlayerId id, PlayerId fallback);
    // Next tick: claims from now on are no longer "simultaneous" with earlier ones.
    void Tick() { ++tick_; }
    void Forget(const NetRef& ref) { book_.erase(ref.Pack()); }
    size_t Size() const { return book_.size(); }
    std::vector<NetRef> OwnedBy(PlayerId id) const;  // in ref order

private:
    struct Entry {
        PlayerId owner = kNoPlayer;
        uint16_t epoch = 0;
        uint64_t claimTick = UINT64_MAX;  // tick the owner won it by a claim
    };
    std::map<uint64_t, Entry> book_;
    uint64_t tick_ = 0;
};

struct PlayerSpot {
    PlayerId id = 0;
    Vec3 pos;
    bool inWorld = false;  // in the game world (not in a menu / loading / dead screen)
};

struct RegionTuning {
    float hostRadius = 250;    // m: the host owns every point this near him
    float releaseRadius = 400; // m: an owner keeps a point until it is this far from him ...
    float margin = 50;         // m: ... and another player is this much nearer
};

struct Region {
    PlayerId owner = 0;
    Vec3 center;               // the owner's position
};

class RegionAuthority {
public:
    explicit RegionAuthority(const RegionTuning& t = {}) : t_(t) {}
    void SetPlayers(std::span<const PlayerSpot> players, PlayerId host);
    // Who should simulate the ambient world at pos (no history): the host within hostRadius, else the nearest
    // in-world player (ties: lower id); the host when nobody is in the world.
    PlayerId AuthorityAt(const Vec3& pos) const;
    // With hysteresis: currentOwner keeps pos until it is beyond releaseRadius from him AND AuthorityAt's choice is
    // nearer by margin. An owner who left or is not in the world: AuthorityAt.
    PlayerId Keep(PlayerId currentOwner, const Vec3& pos) const;
    // The regions: the host, then (in id order) every in-world guest farther than releaseRadius from the host and from
    // every owner before him. Guests close together share the lower id's region.
    std::vector<Region> Regions() const;
    // Whose region a player is in (himself when he owns one; the host when not in the world).
    PlayerId RegionOf(PlayerId id) const;
    const RegionTuning& Tuning() const { return t_; }

private:
    const PlayerSpot* Find(PlayerId id) const;
    RegionTuning t_;
    std::vector<PlayerSpot> players_;  // sorted by id
    PlayerId host_ = 0;
};

}  // namespace mc::net::authority
