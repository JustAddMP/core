# Interest management

Code: `src/net/interest.{h,cpp}` (class `mc::net::InterestManager`, types in `mc::net::interest`).
Tests: `tests/net_interest.cpp` (`out\net_interest.exe`).

The host will mirror NPCs, mission cars and traffic to the guests (PLAN section B). A city block can hold hundreds of
those; sending all of them to every guest at 20 Hz would cost several MB/s. The interest manager decides, per
receiving peer, **which** entities that peer needs (only those near one of its viewpoints), **when** it needs an update
(near = every tick, far = less often) and **how much** fits (a per-peer byte budget). It only makes decisions; the sync
layer owns the wire format, spawning and sending.

It has no game, transport or OS dependencies, so it compiles into the dll and into the tests. Single-threaded (call it
from one thread, the render-thread send tick like the rest of the session code), deterministic (the same calls in the
same order give the same events and send lists: no hash-map iteration, no clocks, no randomness), and allocation-light
(per-tick work reuses its buffers; only registering entities/peers or growing past the previous high-water mark
allocates).

## Model

- **Entity**: something one game simulates and others mirror. Registered with an `EntityId` (the caller's network id,
  unique while registered), an **owner** (`PeerKey` of the peer that simulates it: a player for his own player/car,
  the host for NPCs and traffic), a **kind** (`Player`, `PlayerCar`, `Npc`, `Vehicle`, `Object`), a **priority class**
  (`Critical`, `High`, `Normal`, `Low`; `Default` = the kind's), a position, an optional velocity, and the caller's
  estimate of one state update's size in bytes.
- **Peer**: a receiver, identified by a `PeerKey` (e.g. the session `PlayerId`). It has up to 4 **viewpoints** (its
  player, its camera, ...): distance = distance to the nearest viewpoint (3D, metres; x/y ground plane, z up).
- An entity is never relevant to its owner (`Config::sendToOwner = false`): the owner is the authority.

## Per tick

`Tick(now)` (now = monotonic seconds; the caller ticks at its send rate, 20 Hz for player sync):

1. **Grid.** Entities that are not always relevant are counting-sorted into a uniform grid (64 m cells, hashed into a
   power-of-two bucket table sized to the entity count). Each peer queries the cells within the largest leave radius
   (350 m by default: ~13 x 13 cells, cells outside the circle skipped) around each viewpoint and records the nearest
   distance per entity. Cost is O(entities + entities near the peers), not O(entities x peers).
2. **Relevance with hysteresis.** A not-relevant entity **enters** when within `enterRadius` (300 m) of a viewpoint; a
   relevant one **leaves** only beyond `leaveRadius` (350 m) of every viewpoint, so an NPC walking along the edge
   does not flap spawn/despawn. Always-relevant entities (kinds `Player`, `PlayerCar`, or `EntityDesc::alwaysRelevant`)
   enter regardless of distance. An owner change makes the entity leave its new owner (`LeaveReason::OwnerChanged`)
   and enter the old one.
3. **Events.** `Events()` lists this tick's `Enter`/`Leave` per peer: first the `Leave(Removed)` events of entities
   removed since the last tick, then per peer (in `AddPeer` order) its Leaves, then its Enters, each sorted by entity
   id. Valid until the next `Tick`.
4. **Due entities.** Each relevant entity has an update interval from its distance:
   `interval = lerp(nearInterval, farInterval, clamp((d - nearDistance) / (enterRadius - nearDistance)))`,
   multiplied by `restIntervalScale` when it has a velocity below `restSpeed` (parked cars, idle NPCs), capped at
   `maxInterval`. It is due when `now - lastSent >= interval` (with half a tick of slack). Score =
   `priorityWeight[class] x (now - lastSent) / interval` (its **staleness**); an entity that has not been sent since it
   entered scores `weight x (1000 + ...)`, so new entities go out first.
5. **Budget.** Each peer has a token bucket: `bytesPerSecond` per second, depth `bytesPerSecond x burstSeconds`
   (at least the largest entity's size). The due entities are sorted (Critical first, then score descending, then id)
   and taken while their size (`sizeBytes + perMessageOverhead`) fits the tokens. **Critical** entities always go and
   may push the bucket into debt (floored at minus its depth). A non-critical entity that does not fit stops the list
   (head of line): smaller entities do not overtake it, so a large message cannot starve.
6. **Fairness.** An entity that was due but not sent keeps its `lastSent`, so its staleness keeps growing until it
   outranks fresher ones. Under saturation every entity ends up sent at about the same staleness ratio, i.e. far
   entities get proportionally fewer but regular updates (test: 2000 NPCs in 300 m at 64 KB/s: every one updated,
   worst gap ~10 s; near ones ~8x as often as far ones). Classes share the budget in proportion to their weights.

Everything in `SendList(peer)` is assumed sent at `now`: the caller must send all of it this tick.

## Defaults (`interest::Config`)

| | enter / leave | priority | near distance | interval near -> far | rest (speed, scale) |
|---|---|---|---|---|---|
| Player | always | Critical | | 0.05 s | |
| PlayerCar | always | Critical | | 0.05 s | |
| Npc | 300 / 350 m | Normal | 50 m | 0.05 -> 0.5 s | < 0.2 m/s, x2 |
| Vehicle | 300 / 350 m | Normal | 80 m | 0.05 -> 0.4 s | < 0.3 m/s, x4 (parked) |
| Object | 150 / 180 m | Low | 30 m | 0.1 -> 1.0 s | < 0.1 m/s, x2 |

Global: `cellSize` 64 m, `bytesPerSecond` 512 KB/s per peer (core 0.6; was 64 KB/s) (`SetPeerBudget` overrides it per peer), `burstSeconds`
0.25, `perMessageOverhead` 0, `priorityWeight` High 4 / Normal 2 / Low 1, `maxInterval` 2 s. Every `KindConfig` field
can be overridden; the constructor repairs nonsense (leave < enter, negative intervals).

## API

```cpp
#include "net/interest.h"
using namespace mc::net;

InterestManager im;                         // or InterestManager im(cfg) with an interest::Config
im.AddPeer(guestId);                        // RemovePeer(id) drops it silently (no events)
im.SetPeerViewpoint(guestId, guestPos);     // or SetPeerViewpoints(id, span) for up to 4
im.SetPeerBudget(guestId, 48 * 1024);       // optional

interest::EntityDesc d;
d.id = netId; d.owner = kHostPlayer; d.kind = interest::Kind::Npc;
d.priority = interest::Priority::High;      // e.g. mission enemies; Default = the kind's
d.pos = pos; d.vel = vel; d.hasVelocity = true; d.sizeBytes = 48;
im.AddEntity(d);                            // false if the id exists
im.UpdateEntity(netId, pos, vel);           // every tick (positions must be finite)
im.SetEntityOwner / SetEntityPriority / SetEntitySize;
im.RemoveEntity(netId);                     // Leave(Removed) to everyone who had it, at the next Tick

im.Tick(now);
for (const interest::Event& e : im.Events()) ...        // spawn on Enter, despawn on Leave
for (const interest::SendItem& s : im.SendList(guestId)) ... // send these states to that guest now
im.ChargeBytes(guestId, spawnMsgBytes);     // bytes sent outside the send list count against the budget too
im.Stats(guestId);                          // relevant / due / sent / deferred / bytes / tokens
```

`SendItem` carries the entity id, the bytes charged, the distance (handy to pick a coarser encoding far away) and
`first` (the first update since Enter).

## Integration (in a game layer; example: MafiaDE)

The manager lives in core; a game's session code drives it. How MafiaDE uses it:

**Player sync (now).** Only players and their cars are replicated today, and those are always relevant and Critical,
so the manager would only add bookkeeping. `PlayerState` stays a plain 20 Hz `Broadcast`; a `TODO(interest)` comment
in MafiaDE's `PlayerSync::Tick` (`src/game/player_sync.cpp`, where PlayerState is sent) marks the integration point. When the
host starts streaming other entities, the same tick is where the manager runs.

**Phase 4 NPC / vehicle mirroring (host).**

1. The host owns one `InterestManager` (created with the session; `AddPeer` on `onPlayerJoined`, `RemovePeer` on
   `onPlayerLeft`). Each send tick it sets every guest's viewpoint from that guest's latest PlayerState (player
   position, plus his car's if he drives) before `Tick`.
2. The main-thread sampler enumerates the host's mirrored entities (mission NPCs, enemies, mission cars, traffic)
   and hands their id/kind/position/velocity to the render thread under the player-sync mutex, like the local
   sample today; the render thread applies `AddEntity` / `UpdateEntity` / `RemoveEntity`. Mission-critical NPCs
   (targets, escorts) get `Priority::High` or `alwaysRelevant`; ambient ones `Normal`/`Low`.
3. After `Tick`: for each `Enter`, send that guest a reliable spawn message (model, transform, state) and
   `ChargeBytes` its size; for each `Leave`, a reliable despawn. Then for each guest, pack the states of its
   `SendList` into unreliable messages (several entities per message, under the MTU) and send them with
   `SendTo(guest, ...)`; the protocol ids go in the game's app id range (MafiaDE: 0x02xx).
4. Guests create puppets on spawn, drive them from interpolated snapshots like player puppets (the snapshot
   timestamp makes uneven update rates harmless: a far NPC updated every 0.5 s just interpolates over a longer
   gap), and remove them on despawn.
5. Guests never run this: they send only their own player/car state. Guest-owned entities (a car a guest drives)
   are registered on the host with that guest as owner, so they are not echoed back to him.
