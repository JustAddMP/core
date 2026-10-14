# MafiaCoop network protocol

Phase 2 (connection) and phase 3 (player sync, below). Code: `src/net/`, `src/game/player_sync.*`. All integers are little-endian. `varint` = unsigned LEB128.
`str` = varint byte length + UTF-8 bytes (no terminator). Everything runs single-threaded inside `Poll()` on the
game's render thread; there are no network threads.

```
Session  (session.cpp)   handshake, player list, ping, chat, app messages, host relay
   |  Transport interface (transport.h): Host / Connect / Send(Reliable|Unreliable) / Poll / Disconnect / Close
   +-- UdpTransport   (udp_transport.cpp)   Winsock UDP, own reliability       peer id = 1, 2, 3 ...
   +-- SteamTransport (steam_transport.cpp) ISteamNetworking006 P2P             peer id = SteamID64
SteamLobby (steam_lobby.cpp)  lobby, invites, rich presence -> gives the guest the host's SteamID
```

Transport guarantees: `Reliable` messages arrive once, in order, per peer, up to 256 KB. `Unreliable` messages may
be lost or reordered, up to 1100 bytes. A peer that is silent for 10 s is dropped (`Timeout`). Local `Disconnect()`
raises no event; remote close, timeout, refused and failed connects raise `Disconnected(reason)`.

## Version rules

| Value | Where | Rule |
|---|---|---|
| `kProtocolVersion` (session.h, currently 5; 5 added the session password) | Hello | Must match exactly. Bump on ANY incompatible change to session or app messages. |
| `modBuild` (SessionConfig, set by the dll from its build number) | Hello | Must match exactly (sync code must be identical on both sides). |
| UDP wire version (byte 2 of every datagram, currently 1) | UDP header | Datagrams with another version are ignored. Bump only if the UDP framing changes. |
| Lobby data `mc_version` / `mc_proto` | Steam lobby | Informational (shown before connecting); the Hello check is authoritative. |

A mismatch is answered with `Reject{code 1, "version mismatch: host has protocol X build Y, you have ..."}` and the
guest's `onDisconnected` receives `"rejected by host: <reason>"`.

## Session messages

Every transport message starts with a one-byte session message type.

| Id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x01 | Hello | G->H | R | u32 magic `0x504F434D` ("MCOP"), u32 protocolVersion, u32 modBuild, str name (<= 32 bytes used) |
| 0x02 | Welcome | H->G | R | u8 yourId, u8 maxPlayers, u8 count, count x {u8 id, str name} (includes host and you) |
| 0x03 | Reject | H->G | R | u8 code (1 version, 2 full, 3 bad handshake, 4 password required, 5 wrong password, 6 too many attempts), str reason (human readable) |
| 0x04 | PeerJoined | H->G | R | u8 id, str name |
| 0x05 | PeerLeft | H->G | R | u8 id, str reason ("closed by remote", "timed out", ...) |
| 0x06 | Ping | both | U | u32 sender time ms |
| 0x07 | Pong | both | U | u32 echoed time ms |
| 0x08 | Chat | both | R | u8 from (host overwrites with the real sender), str text (<= 256) |
| 0x09 | PingTable | H->G | U | u8 count, count x {u8 id, u16 rtt ms} (every 2 s, for a ping display) |
| 0x0A | Challenge | H->G | R | 32 random bytes (only when the host has a password; see "Session password") |
| 0x0B | Auth | G->H | R | u8 has (0 = guest has no password), then if has: 32-byte HMAC-SHA256 answer |
| 0x10 | App | both | R/U | varint appId, u8 from, u8 target, u8 flags (bit0 = sent reliable), payload (rest) |

Handshake: transport connects -> guest sends Hello -> host replies Welcome (and broadcasts PeerJoined to the
others) or Reject (then drops the connection after 1 s so the Reject is delivered). The host drops connections that
send no Hello within 5 s and ignores everything but Hello before the handshake completes. Max 4 players; player ids:
host = 0, guests = 1..3 (lowest free id is reused). `0xFF` = all players.

### Session password

Optional; the host sets it (ini `password=`, launcher `--password <p>`, or CO-OP > SET PASSWORD at runtime; "" = open
session). The password itself is never sent.

```
guest                                   host
  Hello  ---------------------------->    version / full checks as before; no password (or an invited peer,
                                          below) -> Welcome. Otherwise, if this address is locked out ->
                                          Reject{6 "too many attempts, try again later"}, else:
         <----------------------------  Challenge{32 bytes from BCryptGenRandom}, fresh per attempt
  Auth{has, mac} -------------------->    mac = HMAC-SHA256(key = UTF-8 password,
                                                            msg = "MafiaCoop session auth v1" || challenge)
                                          has = 0           -> Reject{4 "password required"} (not a failure)
                                          mac matches       -> Welcome (constant-time compare)
                                          mac wrong         -> failure recorded; Reject{5 "wrong password"} sent
                                                               ~1 s later (queued, Poll sends it; never blocks)
```

- Attempt limit: 5 wrong answers per address (UDP: the IPv4 without the port, `Transport::PeerAddress`; Steam: the
  SteamID) within 60 s lock that address out; further attempts, even with the right password, get Reject 6 at Hello
  until the oldest failure is 60 s old. `SessionConfig::authMaxFailures` / `authWindowS` / `authFailDelayS`.
- The Auth must arrive within the Hello timeout (5 s) after the Challenge, else the connection is dropped.
- Invites: `Session::isInvitedPeer` (the dll: a Steam P2P peer that is a member of the host's Steam lobby) skips the
  challenge while `SessionConfig::invitesSkipPassword` is true (ini `require_password_for_invites = 1` turns it off).
  Guests joining by IP always need it.
- Guest side: `Session::LastReject()` / `NeedsPassword()` after the rejection; `RetryWithPassword(p)` joins the same
  address again. The dll shows a PASSWORD box for "password required" / "wrong password" and repeats the last join
  (UDP address or Steam lobby) with what was typed.
- Changing or clearing the password while hosting affects new joins only; players already in stay.
- Strength: an observer of one Challenge/Auth pair can try passwords offline against the HMAC, so a password only
  keeps strangers out of a game; it is not an account secret. Use something not reused elsewhere.
- Crypto: `src/net/auth.{h,cpp}`, Windows BCrypt only (`bcrypt.lib`).

Ping/Pong: each side pings every 1 s (unreliable). Guests keep their own RTT to the host; the host's PingTable fills
in the others.

### App messages and relay

Later phases register handlers with `session.On(appId, handler(from, ByteReader&))` and send with
`SendToHost`, `Broadcast`, `SendTo(player)`. Guests only ever talk to the host:

- guest `SendToHost` -> target 0: the host handles it.
- guest `Broadcast` -> target 0xFF: the host handles it AND relays it to every other guest.
- guest `SendTo(p)` -> target p: the host relays it to p only (or handles it if p = 0).
- host `Broadcast` / `SendTo` go straight to the guests. Host `SendToHost` loops back to its own handler.

The host always rewrites `from` with the real sender (guests cannot spoof each other) and relays on the same
channel the sender used (flags bit0).

App id allocation (u16, varint-encoded):

| Range | Owner |
|---|---|
| 0x0000-0x00FF | tests / debug |
| 0x0100-0x01FF | phase 3: player sync (ped transform, anim, weapon, vehicle seat) |
| 0x0200-0x02FF | phase 4: world (NPCs, cars, weather, time, mission state) |
| 0x0300-0x03FF | phase 5: UI / notifications |
| 0x0400-0x04FF | phase 6: damage, objectives |

## Phase 3: player sync (app ids 0x0100-0x01FF)

Code: `src/game/player_sync.{h,cpp}` (C++: wire format, buffering, interpolation) and `src/game/player_sync.lua`
(game side, embedded into the dll). Allocated ids:

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0100 | PlayerState | every peer -> everyone (`Broadcast`; the host relays guest states) | U, 20 Hz | see below |
| 0x0101 | CarParked | last driver -> everyone (`Broadcast`) | R | u8 keyOwner, u32 vehId, str model, f32 x, y, z, f32 qx, qy, qz, qw |
| 0x0102-0x01FF | reserved (phase 3 events) | | | |

PlayerState body (54 bytes on foot + weapon name, ~130 bytes in a car):

```
u8  version (3)
u32 senderTimeMs          sender's monotonic clock (QueryPerformanceCounter), for interpolation only
f32 x, y, z               player position (world metres)
f32 hx, hy                heading, unit vector in the ground plane
f32 vx, vy, vz            velocity (m/s; on foot differentiated from positions, in a car the car's GetSpeed())
u8  mode                  0 idle, 1 walk, 2 run, 3 sprint (from the speed; sprint also when IsSprinting())
u8  flags                 bit0 in vehicle, bit1 crouch, bit2 sprint, bit3 aim (aim input held with a gun in hand),
                          bit4 reload (IsReloading(), or for 1 s after the clip was refilled)
u8  health                p.health (0..100)
str weapon                weapon in hand (InventoryGetSelected), "" = none
u8  shots                 shots fired so far, mod 256 (from drops of the ammo in the clip); a change = new shots
f32 ax, ay, az            aim direction: the player camera's direction, unit vector
-- only when flags bit0:
str model                 vehicle model name (GetVehicleModelName, e.g. "smith_v12")
u8  seat                  0 = driver, 1..3 passengers, 9 = getting in/out (no seat yet: claims nothing)
u8  keyOwner              car key, part 1: id of the player who first drove this car
u32 vehId                 car key, part 2: the car's entity GUID on keyOwner's game
f32 cx, cy, cz            car position
f32 qx, qy, qz, qw        car rotation (quaternion, GetRot: v.x v.y v.z s)
f32 cvx, cvy, cvz         car velocity
```

Receivers keep up to 64 snapshots per player and render them **100 ms in the past** on the sender's clock:
`renderTime = now - offset - 0.1`, where `offset = localReceiveTime - senderTime` is tracked as a running minimum
(slowly relaxed upward, 1 % per message, to follow drift). Between two snapshots everything is lerped (heading
renormalised, car quaternion nlerp with sign fix, discrete fields from the nearer one); past the newest snapshot the
position is extrapolated with the velocity for at most 250 ms, then held. Old/duplicate snapshots (sender time not
newer) are dropped, so loss and reordering need no reliability.

### Shared cars, seats and ownership

Every car a player drives gets a network **car key** `(keyOwner, vehId)`: the id of the first player who drove it and
the car's entity GUID on that player's game. The key never changes, whoever drives the car later; every game maps
keys to its own entity (`MafiaCoop.cars` in the Lua glue): the real car on the game where it was first driven, a copy
(`ObtainSpecificCar` + `SpawnCar`, same model) everywhere else. A copy is created when a key is first seen (from a
driver's PlayerState or a CarParked) and keeps that key, so a passenger reports the key of the car he sits in on his
own game, which is the same car on every other game.

**Owner = last driver.** The game whose player sits in the driver seat simulates the car; everyone else moves their
entity for that key to the driver's interpolated transform every tick (`SetPos`/`SetRot` on a normal, unfrozen car;
`FixCarPos` is not used, docs/game-api.md 7). Puppets sit in the cars like real occupants (`GetOnVehicle`, seat
1-based, no animation): the driver's puppet in the driver seat, passengers' puppets in their seats. While a remote
player drives a car, player entry to it is locked on the other games (`LockPlayerEntryPoints`), so E there does not
carjack the remote driver's puppet.

Message flow:

1. **Drive.** A sends PlayerState with `seat 0` and key K. Every other game spawns/uses its entity for K, seats A's
   puppet as driver and follows A's transform.
2. **Ride along.** B presses E (pad Y) within 5 m of a car K driven by someone else: B's game seats B in the first free
   passenger seat (scripted: `LockControls(true)` + `GetOnVehicle`). B's PlayerState then carries K and his seat;
   the others seat B's puppet there (on A's game in A's real car). E in the car gets B out the normal way.
3. **Get out / park.** A's PlayerState stops carrying K (on foot). The others stop moving K: their entity stays where
   it was (a parked car with normal physics, unlocked) and A's puppet climbs out. When A's car has stopped (speed
   < 0.3 m/s, or 8 s after getting out) A's game sends **CarParked** (reliable) with K and the final transform; the
   others put their entity there (creating a copy if they never had one).
4. **Take over.** Anybody gets into the parked car K's driver seat the normal way (E). That game now simulates K and
   sends `seat 0` with K: step 1 with the new owner. On a game whose player drove K before (e.g. A's own real car),
   the entity is swapped for a fresh copy first, because a car the local player has driven throws NPC drivers out
   (docs/game-api.md 7): a copy we spawned is despawned, a real car is deactivated (hidden).

Conflicts: if two players get into the driver seat of the same parked car at the same moment, each game keeps its
own player driving (the other's `seat 0` claim for K is ignored and logged) until one of them gets out; then step 3/4
apply. Copies are capped at 12 per game (the oldest parked copy nobody sits in is despawned). When the session ends,
copies nobody sits in are despawned; the one the local player sits in stays as a normal car. A player who joins late
does not see cars parked before he joined until somebody drives them.

Puppets that cannot be seated (seat taken, the game keeps throwing them out) are hidden at the car and retried every
4 s.

Puppet lifetime: created when the first PlayerState of a player arrives, removed on `PeerLeft` / transport timeout /
session end (the whole set, including the hidden "factory" car, is removed when the session ends). Model per player:
`visuals/hero/game_main_tommy_{gangster_trenchcoat, gangster_suit_2_hat, cab_driver_flatcap, racing_outfit}` by
player id (0..3), overridable per machine with `puppet_model = <spawn profile>`.

Fire, aim, crouch, reload (no damage yet): a receiver turns `aim` (or a new shot, kept for 1 s as hip fire) into
`DbgTesting_AimAtDir(ax, ay, az)` on the puppet, fires one real shot per new `shots` count (trigger press/release, at
most a few per tick; a far backlog is skipped), `crouch` into `SetStalkMove`, and a rising `reload` into a reload
press. Shots are not separate messages: they ride on the 20 Hz state, so a lost packet only delays them by 50 ms.

## Phase 4: world (app ids 0x0200-0x02FF)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0200 | WorldEnv | host -> guests (`Broadcast`) | R | f32 timeOfDay (hours 0..24), f32 timeFlowSpeed, str weatherSet |

WorldEnv is sent while the host is in the world: every 4 s and at once when the weather set or the time flow speed
changes. A guest in the world sets the weather set when it differs (`SetWeatherSet(name, 0, 0)`), the flow speed when it
differs, and its clock when it is more than 0.05 h (3 game minutes) off (`SetTime(t, 0)`). Guests' own weather/time
scripts may change things in between; the next WorldEnv puts them back.

## UDP transport

Datagrams are at most 1200 bytes (MTU). IPv4, address `ip[:port]` (default port 27960).

Header (8 bytes): `u16 0x434D` ("MC"), `u8 wireVersion` (1), `u8 type`, `u32 token`. The token is a random nonce
chosen by the connecting side; every packet after the handshake must carry it (stale or spoofed packets from the
same address are ignored).

| Type | Name | Body |
|---|---|---|
| 1 | CONNECT | u64 cookie (0 on first try), zero padding to 64 bytes |
| 2 | CHALLENGE | u64 cookie |
| 3 | ACCEPT | - |
| 4 | DATA | ack section, then units |
| 5 | PING | u32 id, u32 sender ms |
| 6 | PONG | echoed id, ms |
| 7 | DISCONNECT | - (sent twice) |
| 8 | REFUSE | u8 reason (1 not hosting, 2 full) |

Handshake (stateless anti-spoof): client sends CONNECT(cookie 0) every 250 ms. The host answers with
CHALLENGE(cookie = SipHash-2-4(secret, ip, port, nonce, 5-second time bucket)) without allocating anything; the
padded CONNECT is always larger than the CHALLENGE, so the host cannot be used as an amplifier. The client
repeats CONNECT with the cookie; the host accepts cookies from the current or previous bucket, creates the
connection and sends ACCEPT (re-sent if a duplicate CONNECT arrives). Any DATA/PING from the host also completes the
client's handshake if ACCEPT was lost. The client gives up after 10 s (`ConnectFailed`).

Keepalive: PING every 0.5 s; a peer is dropped after 10 s without any packet (`Timeout`). PING/PONG gives the RTT
and the loss estimate (fraction of settled pings without a pong, last 64).

DATA ack section: `u16 cumulative` (next reliable seq expected; everything before it was received), `u8 n` (0..8),
`n x u32` selective bits: bit k of word w = seq `cumulative + 1 + 32w + k` is buffered. Every DATA carries it; an
ack-only DATA is sent in the same Poll when reliable data arrived and nothing else is going out.

Units (as many as fit in one datagram):

```
u8 kind   0 unreliable | 1 reliable whole | 2 fragment first | 3 fragment middle | 4 fragment last
u16 seq   (reliable kinds only; 16-bit wrapping)
varint length, bytes
```

Reliable channel: each unit has a sequence number; window 256 units; the receiver buffers out-of-order units and
delivers strictly in order. Unacked units are resent after an RTO of `srtt + 4*rttvar` (50 ms .. 1 s, Karn's rule:
only first transmissions give RTT samples), doubled per retry up to 8x. Up to 128 new units per Poll. Messages
over 1100 bytes are split into fragments (first fragment starts with `u32 totalLength`, max 256 KB); since
fragments are reliable and ordered, reassembly is a simple append.

Test simulation (`UdpOptions.simLoss/simLatencyMs/simJitterMs/simDuplicate`) applies to outgoing datagrams; jitter
reorders them. `tests/net_loopback.cpp` runs at 20% loss + 20 +-15 ms + 5% duplicates.

## Steam transport

**Which steam_api.** The game does not load the `steam_api64.dll` in its folder: it memory-loads an embedded 2015
steam_api without a flat API (`docs/re-notes.md` 3.1). So the dll loads the on-disk `steam_api64.dll` itself
(`LoadLibraryW` from the game folder, falling back to a copy next to mafiacoop.dll), three seconds after the first
frame calls ITS `SteamAPI_Init` (a second pipe to the same running Steam client; the game's own Steam keeps working)
and from then on calls ITS `SteamAPI_RunCallbacks` once per frame from `Coop::Tick` (`src/dll/coop.cpp`). We never
call `SteamAPI_Shutdown`. Verified in game: `SteamAPI_Init` succeeds in two concurrent instances, persona name and
SteamID are read, and a friends-only lobby is created.

**Interface versions.** That on-disk `steam_api64.dll` (Steam Client API, source-control id 5697398). Its
exports, checked with pefile, are flat-API accessors `SteamAPI_SteamUser_v020`, `SteamAPI_SteamFriends_v017`,
`SteamAPI_SteamMatchmaking_v009`, `SteamAPI_SteamUtils_v009`, `SteamAPI_SteamNetworking_v006`,
`SteamAPI_SteamNetworkingSockets_v008`, `SteamAPI_SteamNetworkingUtils_v003`. We target exactly those
(`src/net/steam_flat.h`). (The game exe itself was built against older headers - SteamFriends015,
SteamNetworking005 - and uses its embedded copy; we never touch that one.)

**Why ISteamNetworking006 and not ISteamNetworkingMessages.** This dll predates ISteamNetworkingMessages: there
is no `SteamAPI_SteamNetworkingMessages_*` / `SteamAPI_ISteamNetworkingMessages_*` export. (The Steam client itself
does hand out `SteamNetworkingMessages002` through `SteamInternal_FindOrCreateUserInterface`, but using it would
mean calling C++ vtables directly, without any flat API - rejected as too fragile.) ISteamNetworking's P2P API is
the direct predecessor with the same model - message-based, addressed by SteamID, NAT traversal + Valve relay,
session requests the receiver must accept - and today's Steam client implements it on top of the same relay
network. Everything we need is a flat export: `SendP2PPacket`, `IsP2PPacketAvailable`, `ReadP2PPacket`,
`AcceptP2PSessionWithUser`, `CloseP2PSessionWithUser`, `AllowP2PPacketRelay`. Receiving is pure polling.

All our traffic uses P2P channel 7. Packet = `u8 0x4D` ('M'), `u8 ctrl`, body:

| ctrl | Name | Send type | Body |
|---|---|---|---|
| 1 | CONNECT | reliable, re-sent every 1 s | - |
| 2 | ACCEPT | reliable | - |
| 3 | DATA | reliable (`k_EP2PSendReliable`, ordered, <= 1 MB) | message |
| 4 | DATA | unreliable (`k_EP2PSendUnreliable`) | message |
| 5/6 | PING/PONG | unreliable no-delay | u32 id |
| 7 | DISCONNECT | reliable | - |
| 8 | REFUSE | reliable | - |

Connected/Disconnected events come from this control layer (CONNECT/ACCEPT, DISCONNECT, 10 s keepalive timeout,
20 s connect timeout) plus `P2PSessionConnectFail_t`.

**Accepting sessions.** `P2PSessionRequest_t` (id 1202) arrives when an unknown user sends to us. The host accepts
only if the accept filter allows the SteamID; the dll sets it to "is a member of our lobby". A request that is not
allowed yet is remembered for 15 s and accepted as soon as the filter allows it (lobby membership can become
visible a moment after the guest's first packet). Default filter: refuse everyone. Sending to a user implicitly
accepts their session, so the guest side needs nothing.

Two instances on one PC share a SteamID and cannot use this transport; use UDP (`127.0.0.1`) for that.

**Host = UDP + Steam at once.** The host's Session runs over a `MultiTransport` (`src/net/multi_transport.h`) that
owns a `UdpTransport` and a `SteamTransport`, hosts on both and routes each peer to the part it connected through.
Peer ids cannot collide: UDP ids are small counters, Steam ids are SteamID64s (a collision would be logged and the
new peer dropped). Guests use a single transport.

## Steam lobby and invites

`SteamLobby` (ISteamMatchmaking009 + ISteamFriends017):

- `Create(4)`: friends-only lobby, max 4. Lobby data `mc_version`, `mc_proto`, `mc_host` (host SteamID64),
  joinable. Rich presence `connect = "+connect_lobby <lobbyId>"` and `status`, so "Join Game" in the friends list
  works.
- `OpenInviteDialog()`: `ActivateGameOverlayInviteDialog(lobby)`.
- Invite accepted from the overlay -> `GameLobbyJoinRequested_t` (333). "Join Game" while running ->
  `GameRichPresenceJoinRequested_t` (337) with the connect string. Game started by Steam via "Join Game" ->
  `+connect_lobby <id>` on the command line (`SteamLobby::CommandLineLobby()`). All three call `onJoinRequested`
  (default: `Join`).
- `Join(id)` -> `LobbyEnter_t` -> host SteamID = lobby data `mc_host` (fallback: lobby owner) ->
  `SteamTransport::Connect(hostSteamId)`.

## Steam callbacks: approach and risks

Two kinds of results are needed:

1. **Call results** (`CreateLobby` -> `LobbyCreated_t`, `JoinLobby` -> `LobbyEnter_t`): polled every frame with
   `ISteamUtils::IsAPICallCompleted` / `GetAPICallResult`. No registration needed.
2. **Broadcast callbacks** (`P2PSessionRequest_t`, `P2PSessionConnectFail_t`, `GameLobbyJoinRequested_t`,
   `GameRichPresenceJoinRequested_t`, `LobbyChatUpdate_t`, `LobbyEnter_t`): these only reach code that registered for
   them, through the steam_api instance they were registered with.

`steam_flat.cpp` builds a `CCallbackBase`-compatible object (MSVC x64 layout: vtable at +0,
`uint8 m_nCallbackFlags` at +8, `int m_iCallback` at +12; three virtuals: the two `Run` overloads and
`GetCallbackSizeBytes`) and registers it with OUR steam_api's `SteamAPI_RegisterCallback(obj, id)`. Because the
game's callbacks go through its embedded steam_api, nobody else pumps ours: `Coop::Tick` calls our
`SteamAPI_RunCallbacks` every frame on the render thread, which calls `Run`. `Run` only copies the payload into an
SRWLOCK-guarded queue; `steam::Pump()` (called from `SteamTransport::Poll` / `SteamLobby::Poll`) hands it to the
subscribers. Both `Run` slots do the same thing (enqueue, ignore extra arguments), so the MSVC overload-ordering
quirk (overloaded virtuals are laid out in reverse declaration order) cannot misroute a call. (Classic dispatch is
kept rather than `SteamAPI_ManualDispatch_*`; with our own instance either would work, the registration path is the
one verified by the smoke test and in game.)

Verified by `tests/steam_smoke.cpp` against a copy of the dll and in game: the `LobbyEnter_t` broadcast is delivered
through our registered object, and the `LobbyCreated_t` call result is still readable by polling after
`SteamAPI_RunCallbacks` ran.

Risks:

- **Layout assumption.** If a future steam_api changed `CCallbackBase` (stable across SDK releases for many years),
  registration would break. We load a fixed dll from the game folder, so this only matters if the game updates it.
- **Two steam_api instances in one process.** Ours and the game's embedded one each have their own pipe to
  steamclient64.dll. Seen working (init, persona, lobby create); the game's overlay was not exercised by hand yet.
- **Unregistering** while dispatching would race; the dll never unregisters (objects live until process exit); only
  the standalone smoke test calls `steam::UnregisterAll()` before `SteamAPI_Shutdown`.
- **Call result consumed by RunCallbacks.** Steam might drop a call result nobody registered for when
  `RunCallbacks` sees its completion. Not observed, but as a safety net `SteamLobby` also listens for the
  `LobbyEnter_t` broadcast (posted for both create and join) and uses it if the call result cannot be read or does
  not arrive within 2 s.
- **Flat calls from the render thread.** Steam API calls are internally synchronised. The dll calls `SteamAPI_Init`
  once and `SteamAPI_RunCallbacks` per frame on its own instance only; never `SteamAPI_Shutdown`.
