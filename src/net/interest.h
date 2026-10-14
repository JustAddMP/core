// net: interest management. Decides, per receiving peer, which replicated entities are relevant (in range
// of one of the peer's viewpoints, with enter/leave hysteresis), emits Enter/Leave events so the sync layer can
// spawn/despawn them on that peer, and builds a per-peer send list ordered by priority and staleness, with the update
// rate scaled by distance and capped by a per-peer byte budget. See docs/interest.md.
//
// No game, transport or OS dependencies: pure data in, decisions out. Single-threaded, deterministic (same calls in
// the same order give the same events and send lists), allocation-light (per-tick work reuses its buffers; only
// registering new entities/peers or growing past the previous high-water mark allocates).
#pragma once
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace mc::net {

namespace interest {

using EntityId = uint32_t;  // caller's network id for the entity (unique while registered)
using PeerKey = uint32_t;   // caller's id for a receiving peer (e.g. a session PlayerId)
constexpr PeerKey kNoOwner = 0xFFFFFFFFu;

struct Vec3 {
    float x = 0, y = 0, z = 0;  // world metres; x/y = ground plane, z = up (the game's convention)
};

enum class Kind : uint8_t { Player, PlayerCar, Npc, Vehicle, Object, Count };
constexpr int kKindCount = static_cast<int>(Kind::Count);

// Send order: Critical before everything (and not limited by the budget), then the others by weight x staleness.
enum class Priority : uint8_t { Critical, High, Normal, Low, Default, Count = Default };
constexpr int kPriorityCount = static_cast<int>(Priority::Count);

struct KindConfig {
    float enterRadius = 300;       // not relevant -> relevant when the nearest viewpoint is within this
    float leaveRadius = 350;       // relevant -> not relevant when every viewpoint is farther than this
    bool alwaysRelevant = false;   // ignore distance (players and their cars)
    Priority priority = Priority::Normal;  // default for entities of this kind
    float nearDistance = 50;       // up to here: nearInterval
    double nearInterval = 0.05;    // seconds between updates when near (0.05 = every tick at 20 Hz)
    double farInterval = 0.5;      // seconds between updates at enterRadius (linear in between)
    float restSpeed = 0;           // > 0 and the entity has a velocity below it: interval x restIntervalScale
    double restIntervalScale = 1;
    double maxInterval = 2.0;      // cap after rest scaling
};

struct Config {
    float cellSize = 64;                  // uniform grid cell (metres)
    uint32_t bytesPerSecond = 64 * 1024;  // per receiving peer
    double burstSeconds = 0.1;            // token bucket depth (budget x this, at least the largest message)
    uint32_t perMessageOverhead = 0;      // added to every entity's size (headers)
    bool sendToOwner = false;             // false: an entity is never relevant to the peer that owns it
    float priorityWeight[kPriorityCount] = {1, 4, 2, 1};  // Critical's weight is unused (always first)
    KindConfig kinds[kKindCount];

    Config();  // fills the per-kind defaults (docs/interest.md)
};

struct EntityDesc {
    EntityId id = 0;
    PeerKey owner = kNoOwner;            // peer that simulates it (players: the player; NPCs: the host)
    Kind kind = Kind::Npc;
    Priority priority = Priority::Default;  // Default = the kind's priority
    Vec3 pos;
    Vec3 vel;
    bool hasVelocity = false;
    bool alwaysRelevant = false;         // forced on, in addition to the kind's flag
    uint32_t sizeBytes = 64;             // caller's estimate of one state update
};

enum class EventType : uint8_t { Enter, Leave };
enum class LeaveReason : uint8_t { None, OutOfRange, Removed, OwnerChanged };

struct Event {
    PeerKey peer = 0;
    EntityId entity = 0;
    EventType type = EventType::Enter;
    LeaveReason reason = LeaveReason::None;
};

struct SendItem {
    EntityId entity = 0;
    uint32_t bytes = 0;      // size charged (sizeBytes + perMessageOverhead)
    float distance = 0;      // to the peer's nearest viewpoint (metres)
    bool first = false;      // first update since Enter
};

struct PeerStats {
    uint32_t relevant = 0;   // entities in range this tick
    uint32_t due = 0;        // of those, due for an update
    uint32_t sent = 0;       // put in the send list
    uint32_t deferred = 0;   // due but over the budget (sent later, staleness keeps growing)
    uint32_t bytes = 0;      // bytes in this tick's send list
    uint64_t totalBytes = 0; // since AddPeer (send lists + ChargeBytes)
    double tokens = 0;       // budget left (can be negative after Critical sends)
};

}  // namespace interest

class InterestManager {
public:
    using EntityId = interest::EntityId;
    using PeerKey = interest::PeerKey;
    static constexpr int kMaxViewpoints = 4;

    explicit InterestManager(interest::Config cfg = {});
    InterestManager(const InterestManager&) = delete;
    InterestManager& operator=(const InterestManager&) = delete;

    const interest::Config& GetConfig() const { return cfg_; }

    // ---- entities ----
    bool AddEntity(const interest::EntityDesc& d);  // false if the id is already registered
    bool RemoveEntity(EntityId id);                  // Leave(Removed) to every peer that had it, at the next Tick
    bool HasEntity(EntityId id) const { return ids_.count(id) != 0; }
    bool UpdateEntity(EntityId id, const interest::Vec3& pos);  // keeps the previous velocity
    bool UpdateEntity(EntityId id, const interest::Vec3& pos, const interest::Vec3& vel);
    bool SetEntityOwner(EntityId id, PeerKey owner);  // e.g. car hand-off; the new owner gets Leave(OwnerChanged)
    bool SetEntityPriority(EntityId id, interest::Priority p);
    bool SetEntitySize(EntityId id, uint32_t bytes);
    size_t EntityCount() const { return ids_.size(); }

    // ---- receiving peers ----
    bool AddPeer(PeerKey peer);  // false if already registered
    bool RemovePeer(PeerKey peer);  // drops its state silently (no events: the peer is gone)
    // The peer's current positions (its player, its camera...); up to kMaxViewpoints, extra ones are ignored.
    // A peer without viewpoints sees only always-relevant entities.
    bool SetPeerViewpoints(PeerKey peer, std::span<const interest::Vec3> points);
    bool SetPeerViewpoint(PeerKey peer, const interest::Vec3& p) { return SetPeerViewpoints(peer, {&p, 1}); }
    bool SetPeerBudget(PeerKey peer, uint32_t bytesPerSecond);
    // Deducts bytes sent outside the send list (spawn messages on Enter, events) from the peer's budget.
    bool ChargeBytes(PeerKey peer, uint32_t bytes);
    size_t PeerCount() const { return peers_.size(); }

    // ---- per tick ----
    // Recomputes relevance, appends this tick's events, builds every peer's send list. now: monotonic seconds.
    // Everything in a send list is assumed sent at `now` (the caller must send all of it).
    void Tick(double now);

    // Events of the last Tick (removal Leaves first, then per peer in AddPeer order: Leaves, then Enters, each by
    // entity id). Valid until the next Tick.
    std::span<const interest::Event> Events() const { return events_; }
    // Send list of the last Tick, in send order. Empty for an unknown peer.
    std::span<const interest::SendItem> SendList(PeerKey peer) const;
    const interest::PeerStats* Stats(PeerKey peer) const;
    bool IsRelevant(PeerKey peer, EntityId id) const;

private:
    struct Entity {
        EntityId id = 0;
        PeerKey owner = interest::kNoOwner;
        interest::Kind kind = interest::Kind::Npc;
        interest::Priority priority = interest::Priority::Normal;
        interest::Vec3 pos, vel;
        uint32_t size = 0;
        int32_t cx = 0, cy = 0;
        bool hasVelocity = false;
        bool always = false;
        bool alive = false;
    };
    struct PeerEnt {  // per peer, per entity slot
        double lastSent = 0;
        double enteredAt = 0;
        float d2 = 0;
        float dist = 0;     // to the nearest viewpoint, this tick (relevant entities only)
        uint32_t stamp = 0;
        bool inSet = false;
        bool sent = false;  // sent at least once since Enter
    };
    struct Candidate {
        double score;
        uint32_t slot;
        bool critical;
    };
    struct Peer {
        PeerKey key = 0;
        interest::Vec3 views[kMaxViewpoints];
        int viewCount = 0;
        uint32_t budget = 0;
        double tokens = 0;
        uint32_t stamp = 0;
        std::vector<PeerEnt> ents;      // indexed by entity slot
        std::vector<uint32_t> inSet;    // slots currently relevant
        std::vector<uint32_t> inSetNext;
        std::vector<uint32_t> touched;  // slots found by this tick's grid queries
        std::vector<interest::SendItem> sendList;
        interest::PeerStats stats;
    };

    Entity* Find(EntityId id);
    Peer* FindPeer(PeerKey key);
    const Peer* FindPeer(PeerKey key) const;
    void BuildGrid();
    void QueryPeer(Peer& p, float radius);
    void UpdatePeer(Peer& p, double now, double dt);
    double Interval(const Entity& e, float dist) const;
    double BucketCap(const Peer& p) const;

    interest::Config cfg_;
    std::vector<Entity> ents_;
    std::vector<uint32_t> freeSlots_;
    std::unordered_map<EntityId, uint32_t> ids_;
    std::vector<Peer> peers_;
    std::vector<interest::Event> events_, pendingEvents_, tickEvents_;
    std::vector<uint32_t> always_;  // slots of always-relevant entities (rebuilt per tick)
    // grid: counting-sorted slots by hashed cell
    std::vector<uint32_t> bucketStart_, gridSlots_;
    uint32_t bucketMask_ = 0;
    std::vector<Candidate> cands_;
    float queryRadius_ = 0;
    uint32_t maxSize_ = 0;
    double lastNow_ = 0;
    bool ticked_ = false;
};

}  // namespace mc::net
