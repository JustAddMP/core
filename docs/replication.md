# JustAddMP core: replication

How a game layer shows things that another player's game simulates: players, their cars, traffic, NPCs,
pedestrians. Core gives one shared layer for it (core 0.12):

| Header | What |
|---|---|
| `net/playout.h` | `PlayoutClock`: one adaptive playout delay per sender (docs/protocol.md "Playout delay") |
| `net/replica.h` | `SnapshotTrack` (one entity's snapshots -> its state at a render time), `Stream` (one sender: clock + tracks) |
| `net/correction.h` | `Corrector`: one policy that keeps a locally simulated copy on its network target |
| `net/remote_motion.h` | `LocomotionController` (an AI human copy), `VehicleController` (a car copy) |
| `net/authority.h` | `NetRef` (stable ids), `OwnershipBook` (who simulates a thing), `RegionAuthority` (who simulates the ambient world where) |

All of it is pure logic: no game, transport or OS calls, deterministic (ordered containers only), not thread-safe.
Unit tests: `tests/net_replica.cpp`, `tests/net_motion.cpp`.

## The model

1. **One authority per thing.** Exactly one game simulates each thing (its *authority* / owner). Every other game
   shows a *replica* driven toward the authority's state. Never two AIs for one thing.
2. **Stable identity.** Every replicated thing has a `NetRef`.
3. **Time base = the sender's clock.** Snapshots carry the sender's sample time (seconds, unwrapped). Each sender has
   one `PlayoutClock`; it measures the offset (local - sender + smallest one-way delay) and the jitter, and gives the
   *render time*: the sender-clock time to show now, a little in the past so there is a snapshot after it.
4. **Interpolation.** A replica's state at the render time is interpolated between the two snapshots around it.
5. **Lead.** For a copy that the local game simulates (an AI walker, a car with physics) the target is the
   interpolated state moved forward along its velocity by the playout delay (`Present`): where the authority's thing
   is *now*.
6. **Correction.** The copy moves by its own simulation; the remaining error is corrected so the correction is hidden
   in the motion, and snapped only when clearly wrong.

## Interpolation (`net/replica.h`)

`replica::Sample { t, pos, vel, rot[4], hasRot, bits }` - one snapshot. `bits` is anything discrete (flags, mode,
gait): it is never blended, it comes from the nearer sample.

`SnapshotTrack` keeps the last `capacity` (32) samples of one entity; a sample not newer than the newest is ignored.
`At(renderTime)`:

| Render time | Result |
|---|---|
| before the oldest sample | the oldest sample |
| between two samples | position on the cubic Hermite curve through both with their velocities (no corner at each sample); velocity lerped; rotation nlerped (when both have one); `bits` from the nearer sample; `fromT`, `toT`, `u` say which pair (for a caller's own payload) |
| ... but they are more than `teleportDist` (20 m) apart | the nearer sample, `teleport = true` (a car is not swept across the map) |
| past the newest | extrapolated along its velocity (`playout::Extrapolate`; `accelTime` > 0 also follows the acceleration of the last two samples that long) for at most `maxExtrapolate` (0.25 s), then held (`held = true`); `starved` = seconds past the newest |

`Present(renderTime, lead)` = `At()` moved by velocity x `lead`, at most `maxLead` (1.5 m).

`Stream` is one sender's set of entities: `Observe(senderTime, localTime)` once per received message, `Push(key,
sample)` per entity in it, `Update(localNow)` once per frame, then `At(key, now)` / `Present(key, now)` (the stream's
render time and delay as lead), `Forget(olderThanLocal)` for entities that stopped coming, `Keys()` in key order.

The defaults reproduce the interpolation the Mafia DE game layer had in `player_sync.cpp` (Interpolate + Present)
and `traffic_model.cpp` (GuestTraffic::Present) - a test compares them on 6000 random queries (identical to 1e-5 m) -
so a game layer can switch to it without a visible change. Differences a game layer handles itself: the traffic
stream took `flags` from the newest sample (read `Newest().bits`), the player stream blends extra fields (car controls)
between the samples `fromT` / `toT` with `u`.

## Correction (`net/correction.h`)

`Corrector::Update(copy, target, targetVel, dt, inAir)` -> `Result { kind None / Blend / Snap, delta, error, height,
why none / far / height / stuck / teleport }`:

1. **Snap** when the target itself jumped more than `teleportDist` since the last update (teleport), the error is
   beyond `snapDist` (far), the height difference beyond `snapHeight` (doubled in the air) (height), or the error is
   beyond `stuckDist` for `stuckTime` while the target stands (stuck: the copy cannot get there).
2. **Nothing** inside `deadZone`.
3. **Blend** beyond it: move `gain x dt` of the excess, at most `max(restSpeed, speedShare x |targetVel|) x dt`. A
   moving copy slides at most `speedShare` of its own speed (hidden in the motion), a standing one only `restSpeed`.

Counters (`Snaps()`, `Snaps(why)`, `Blends()`, `BlendMetres()`) are for trace records. `groundOnly` presets measure and
blend in the ground plane (the game keeps a walker on the ground).

### Correction presets

| Preset | deadZone | gain | speedShare | restSpeed | snapDist | snapHeight | stuck | Why |
|---|---|---|---|---|---|---|---|---|
| `ForFoot()` | 0.3 m | 2 /s | 0.35 | 0.15 m/s | 3.5 m | 3 m | 0.5 m, 2 s | a co-op partner on foot is watched closely: small errors are corrected, slowly enough that the feet do not visibly slide |
| `ForCar()` | 0.5 m | 3 /s | 0.3 | 0.3 m/s | 4 m | 3 m | 1 m, 2 s | a car is big and fast: 0.5 m is invisible, a sliding body is not; 3D (a car jumps, climbs) |
| `ForNpc()` | 0.5 m | 2 /s | 0.35 | 0.15 m/s | 5 m | 3 m | 0.8 m, 3 s | looked at less closely; its own AI may route around obstacles differently, so it is disturbed less |
| `ForTraffic()` | 1 m | 2 /s | 0.3 | 0.3 m/s | 8 m | 4 m | 2 m, 3 s | many cars, mostly seen from a distance: a snapping passing car is more visible than a 1 m error |

All presets: `teleportDist` 20 m, `standSpeed` 0.35 m/s. The values are first estimates from the 2026-10-19 two-PC
traces (UNVERIFIED in game for NPC and traffic until those packages use them).

## Copies the game simulates (`net/remote_motion.h`)

`LocomotionController` (an AI human walked by the game's own locomotion; core 0.12 changes):

- **Gait hint.** `LocomotionInput::gaitHint` = the gait the sender's game reports (0 idle, 1 walk, 2 run, 3 sprint;
  -1 unknown). With a hint the gait IS the hint: it stands only when the hint is 0 and the copy is within the stop
  distance (stays standing up to the start distance), with a hint of 0 farther off it walks there (at least at its
  normal walk pace); the speed multiplier alone absorbs the correction, and when even the fastest multiplier cannot
  reach the wanted speed the carried nudge covers the rest. Without a hint (-1) the old speed-based choice with
  hysteresis.
- **Hidden nudges.** The position nudge uses the correction rule: dead zone `nudgeStart` 0.3 m (was 0.9), capped at
  `max(nudgeRestSpeed 0.15, nudgeSpeedShare 0.35 x target speed)` m/s (was uncapped).
- **Response lag.** The copy acts on a command `responseLag` (0.15 s) later. The error used for the wanted velocity is
  the one expected then (target and copy moved on by their velocities, plus the target's acceleration), and the copy is
  asked for the velocity the target will have then. A target braking to a stop (and not running / sprinting by his
  gait) makes the copy walk to his predicted stop point (`LocomotionCommand::stopping`), so it does not overshoot.
- **Carry only while behind.** The carried shortfall is no longer pushed onto a copy that is on or ahead of its target.

Simulation (tests/net_motion.cpp "gait hint"): an AI walker with speed errors of +-20 %, 150 ms / 200 ms command lag,
a 360 deg/s turn limit and 10 m/s^2 acceleration, on a 60 s path (straight run, sprint with weave, run zig-zag, walk,
stop-and-go, 180-degree turn into a sprint zig-zag): commanded gait = sender's gait 97-98 % of ticks, p95 error moving
0.34-0.41 m, standing 0.07-0.09 m, no snaps. How closely the game's AI walk matches the simulated walker (lag, arrival
at a walk point, turn rate) is UNVERIFIED until the next in-game test.

`VehicleController` pin policy: a copy whose physics does not keep up is pinned (set on its target every tick). It
gets its physics back after `retryFirst` 1.5 s (was 5 s), doubling after each pin up to `retryMax` 8 s (was 40 s);
after `retryReset` 10 s kept up unpinned the next pin starts at 1.5 s again. `VehicleCommand::pinnedFor` /
`retryIn` for traces.

## Authority (`net/authority.h`)

`NetRef { home, kind, local }`: the player whose game created the thing, its `RefKind` (PlayerCar 1, Npc 2, Traffic 3,
Pedestrian 4, Door 5, Item 6, Object 7) and its id on the home game. `Pack()` -> `uint64` (home << 40 | kind << 32 |
local), `Unpack`, `Write` / `Read` (6 bytes on the wire: home u8, kind u8, local u32 little endian), `ToString()`
"home:kind:local". Ordered by (home, kind, local).

`OwnershipBook` - who simulates each NetRef now, with a claim epoch (uint16):

| Claim (ref, by, epoch) | Result |
|---|---|
| epoch lower than the ref's | refused (stale) |
| epoch higher | granted |
| equal, no owner (never claimed / released) | granted |
| equal, `by` is the owner | granted (no change) |
| equal, another owner | refused - unless the owner won it by a claim of this epoch in this same tick (`Tick()` advances) and `by` has the lower id: simultaneous claims go to the lower player id, whatever the arrival order |

`Release(ref, by)` (owner only; epoch kept), `Owner`, `Epoch`, `OwnedBy(id)`, `OnPlayerLeft(id, fallback)`: every ref of
the leaving player goes to the fallback (the host) with the epoch raised by one (his in-flight claims become stale);
returned in ref order. The fallback leaving himself makes the refs unowned.

`RegionAuthority` - which game simulates the ambient world (traffic, pedestrians) at a point. `SetPlayers({id, pos,
inWorld}..., host)` each tick, then (horizontal distances):

- `AuthorityAt(pos)`: the host when he is in the world and within `hostRadius` (250 m); else the nearest in-world
  player (ties: lower id); the host when nobody is in the world.
- `Keep(current, pos)`: hysteresis for a thing that already has an owner: the owner keeps it until it is beyond
  `releaseRadius` (400 m) from him AND `AuthorityAt`'s choice is `margin` (50 m) nearer. An owner who left or is not in
  the world: `AuthorityAt`.
- `Regions()`: the host, then in id order every in-world guest farther than `releaseRadius` from the host (when in the
  world) and from every owner before him. Two guests close together share the lower id's region; `RegionOf(id)` says
  whose region a player is in (the other shows the lower id's world).

## Using it from a game layer

```cpp
#include "net/correction.h"
#include "net/replica.h"
using namespace mc::net;

// receiving: one Stream per sender
replica::Stream npcs;                                   // tuning: replica::StreamTuning
void OnNpcStates(double senderTime, const std::vector<NpcState>& states, double now) {
    npcs.Observe(senderTime, now);
    for (const NpcState& s : states) {
        replica::Sample smp;
        smp.t = senderTime;
        smp.pos = s.pos;
        smp.vel = s.vel;
        smp.bits = s.flags;
        npcs.Push(s.id, smp);                           // older / duplicate samples are ignored
    }
}

// every frame
std::map<uint32_t, correction::Corrector> fix;          // one per copy, Tuning::ForNpc()
void Tick(double now, float dt) {
    npcs.Update(now);
    for (uint32_t id : npcs.Forget(now - 3.0)) RemoveCopy(id);
    for (uint32_t id : npcs.Keys()) {
        const replica::Out t = npcs.Present(id, now);   // where it is now
        auto [it, fresh] = fix.try_emplace(id, correction::Tuning::ForNpc());
        const correction::Result r = it->second.Update(CopyPos(id), t.pos, t.vel, dt);
        if (r.kind == correction::Result::Kind::Snap) SetCopyPos(id, t.pos);
        else if (r.kind == correction::Result::Kind::Blend) MoveCopy(id, r.delta);
        // ... plus the copy's own motion toward t.pos (an AI walk: motion::LocomotionController)
    }
}
```

Ownership: the authority of a thing sends its states; a guest that wants to drive / simulate something sends a claim
with `book.Epoch(ref) + 1` (reliable); the host decides with `OwnershipBook::Claim` and announces the winner. The
ambient world: every tick the host runs `RegionAuthority` over all players' positions; a car / pedestrian near a
point is simulated by `Keep(owner, pos)`.
