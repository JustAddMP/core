# JustAddMP core network protocol

Sessions, transports and Steam lobbies (`src/net/`). The app messages a game sends over a session (player sync,
world state) are defined in that game's repo. All integers are little-endian. `varint` = unsigned LEB128.
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
| `kProtocolVersion` (session.h, currently 7; 5 added the session password, 6 Kicked and the ban Reject, 7 the lobby) | Hello | Must match exactly. Bump on ANY incompatible change to session or app messages. |
| `modBuild` (SessionConfig, derived by the dll from `GameInfo::modVersion`, "1.2.3" -> 10203) | Hello | Must match exactly (sync code must be identical on both sides). |
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
| 0x03 | Reject | H->G | R | u8 code (1 version, 2 full, 3 bad handshake, 4 password required, 5 wrong password, 6 too many attempts, 7 banned), str reason (human readable) |
| 0x04 | PeerJoined | H->G | R | u8 id, str name |
| 0x05 | PeerLeft | H->G | R | u8 id, str reason ("closed by remote", "timed out", ...) |
| 0x06 | Ping | both | U | u32 sender time ms |
| 0x07 | Pong | both | U | u32 echoed time ms |
| 0x08 | Chat | both | R | u8 from (host overwrites with the real sender), str text (<= 256) |
| 0x09 | PingTable | H->G | U | u8 count, count x {u8 id, u16 rtt ms} (every 2 s, for a ping display) |
| 0x0A | Challenge | H->G | R | 32 random bytes (only when the host has a password; see "Session password") |
| 0x0B | Auth | G->H | R | u8 has (0 = guest has no password), then if has: 32-byte HMAC-SHA256 answer |
| 0x0C | Kicked | H->G | R | u8 code (1 kicked, 2 kicked and banned), str reason (<= 256, may be empty); see "Kick and ban" |
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

### Kick and ban

Host only (`Session::Kick(id, reason)`, `Session::KickAndBan(id, reason)`; the dll: the pause CO-OP panel, select a
guest in the player list, KICK or KICK AND BAN, confirm).

```
host                                      kicked guest                    other guests
  Kicked{code, reason} --------------->     onDisconnected("You were kicked by the host[: reason]")
                                            (code 2: "You were kicked and banned by the host[: reason]"),
                                            WasKicked() / WasBanned(); disconnects from the host
  PeerLeft{id, "kicked by the host[: reason]"} ---------------------------------->  onPlayerLeft
  (host: onPlayerLeft too; the connection is kept for 1 s like after a Reject, everything it sends is ignored,
   then dropped)
```

- Guests cannot kick: the API returns false unless hosting, there is no guest-to-host kick request, and the host
  ignores a Kicked (or any unknown type) sent by a guest. App messages relayed between guests keep their App framing,
  so they can never arrive as a Kicked.
- Ban list (`Session::BanList()`, `Unban(index)`, `ClearBans()`): KickAndBan records the player's name, its address
  (`Transport::PeerAddress`: UDP IPv4 without the port) and its SteamID (`Transport::PeerSteamId`: the SteamID64 of a
  peer that came over Steam, else 0). A Hello from a peer matching either gets `Reject{7 "You are banned from this
  session"}` (guest: `LastReject() == RejectReason::Banned`). On one PC every UDP guest is 127.0.0.1, so a ban by
  address refuses all local guests.
- The list lives as long as the hosted session: `Leave()` and every `Host()` clear it. A kicked but not banned player
  can rejoin at once.

Ping/Pong: each side pings every 1 s (unreliable). Guests keep their own RTT to the host; the host's PingTable fills
in the others.

### App messages and relay

The game layer registers handlers (in `GameModule::OnSessionStart`, `include/mc/game_module.h`) with `session.On(appId, handler(from, ByteReader&))` and send with
`SendToHost`, `Broadcast`, `SendTo(player)`. Guests only ever talk to the host:

- guest `SendToHost` -> target 0: the host handles it.
- guest `Broadcast` -> target 0xFF: the host handles it AND relays it to every other guest.
- guest `SendTo(p)` -> target p: the host relays it to p only (or handles it if p = 0).
- host `Broadcast` / `SendTo` go straight to the guests. Host `SendToHost` loops back to its own handler.

The host always rewrites `from` with the real sender (guests cannot spoof each other) and relays on the same
channel the sender used (flags bit0).

### Rate limits

The host keeps two token buckets per guest (`SessionConfig`, a rate <= 0 turns one off):

| Limit | Default | Applies to |
|---|---|---|
| `chatPerS` / `chatBurst` | 5 messages/s, burst 10 | Chat from a guest |
| `relayBytesPerS` / `relayBurstBytes` | 1 MB/s, burst 2 MB (payload bytes; core 0.6, was 64 KB/s and 128 KB) | App messages the host relays (target another guest or 0xFF) |

A message over its limit is dropped whole (the host neither handles nor relays it, so host and guests stay
consistent; reliable ones too) and counted in `Session::RateLimits()`; the host logs the drops per guest at most once
per second. A message larger than the burst still passes when the bucket is full and then has to refill. App
messages to the host only (`SendToHost`) are not limited. Nothing changes on the wire.

The dll sets the relay limit from the per-peer bandwidth setting `net_budget_kbps` (ini or `--net_budget_kbps`, KiB/s,
default 1024, at least 64; burst twice that). Games use the same number for their interest manager
(`Coop::NetBudgetBytesPerS()`, `interest::Config::bytesPerSecond`, default 512 KiB/s).

### Traffic statistics

Measuring what a session really sends (core 0.6):

- `Session::Traffic()`: per app message id, messages and bytes sent (one count per destination peer; session header +
  payload), received from the network, relayed by the host, and sends the transport refused (`MsgTraffic`).
- `PeerStats::reliableQueued` (UDP): reliable units (<= 1100 bytes each) not yet acknowledged: the reliable backlog.
- `net::NetMonitor` (src/net/net_monitor.h) turns both, plus `PeerStats` (rtt, loss, bytes, packets, resends) and
  `RateLimits()`, into rates over an interval. The dll logs a report every `net_log_interval` seconds (default 5,
  0 = off) while another player is in the session, and the debug status text (F7) shows the last second:

```
net: Guest (1): rtt 7 ms, loss 0.0 %, out 15.4 KB/s (76 pkt/s), in 4.4 KB/s (66 pkt/s), resends +0, reliable queued 0
net: messages: NpcStates out 10.76 KB/s 29/s, in 0.00 KB/s 0/s; PlayerState out 3.53 KB/s 60/s, in 3.53 KB/s 60/s; ...
net: DROPPED: relay rate limit +3 messages, chat +0, refused sends +0      (only when something was dropped)
```

  Message names come from `GameModule::MessageName(id)` (hex without one).

App id allocation (u16, varint-encoded):

| Range | Owner |
|---|---|
| 0x0000-0x00FF | core: 0x0020-0x0022 the lobby (below); the rest tests / debug |
| 0x0100-0xFFFF | the game (its repo documents its ids; e.g. MafiaDE: 0x01xx player sync, 0x02xx world) |

### Lobby

`net::Lobby` (src/net/lobby.h) runs on top of a session; the dll attaches one to every session. Players gather before
a game starts: guests toggle "ready", the host picks a game mode (an index into the game's `GameModule::LobbyModes`)
and starts, and every player loads that mode at the same moment. The host also says when it is already playing, so a
player who joins later can follow it ("JOIN GAME"). Host-authoritative: only the host sends state and starts; a
State/Start from anyone else (a guest's Broadcast the host relays) is ignored.

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0020 | LobbyState | host -> guests (`Broadcast`) | R | u8 version (1), u8 mode (0xFF none), u8 inGame, u8 gameMode (0xFF none), u32 startSeq, u8 count, count x {u8 id, u8 ready} |
| 0x0021 | LobbyReady | guest -> host (`SendToHost`) | R | u8 ready |
| 0x0022 | LobbyStart | host -> guests (`Broadcast`) | R | u8 mode, u32 startSeq |

- The host sends LobbyState whenever something in it changes and whenever the player set changes (a newcomer has
  none; he starts not ready, a leaver is forgotten).
- `Lobby::Start(mode)` refuses while a guest is not ready (unless forced), sends LobbyStart, clears every ready flag
  and calls `onStart(mode)` on the host itself; guests call it when the LobbyStart arrives. A start is only acted on
  once (`startSeq` must grow); a late joiner learns `startSeq` from the state and never replays an old start.
- `SetInGame(inGame, mode)`: the dll reports `GameModule::InWorld()` / `CurrentMode()` of the host every frame.
- The host also writes `mc_state` (`lobby` / `playing`) and `mc_mode` into the Steam lobby data (informational).
- Tests: `tests/net_lobby.cpp` (ready reaches everyone, start waits for ready and reaches every guest once, guests
  cannot start or fake the state, late joiner, clean leaving below).

### Leaving cleanly

`Session::Leave()` disconnects every peer through the transport, which tells the other side at once (UDP: a
Disconnect packet sent 4 times; Steam: a reliable Disconnect): a guest sees "host closed the session" (the dll shows
**HOST LEFT**), the host sees PeerLeft "closed by remote" ("<name> left the game"), within milliseconds instead of the
10 s timeout. The dll also leaves cleanly when the game process ends normally: `ExitProcess` and
`TerminateProcess(self)` are hooked (dll/mod.cpp) and call `Coop::Shutdown()` first (verified in Mafia DE: closing the
window ends the process through ExitProcess; the others were told 3 ms later). A process killed from outside (Task
Manager, `taskkill /F`) cannot say goodbye; the others then time out after 10 s.

## UDP transport

Datagrams are at most 1200 bytes (MTU). IPv4, address `ip[:port]` (default port `UdpOptions::defaultPort`: the
game's `GameInfo::defaultPort`, 27960 unless a game picks another).

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
connection and sends ACCEPT (re-sent if a duplicate CONNECT arrives). Incoming connections are limited (`UdpOptions::maxPeers` 8,
`maxPeersPerAddress` 4); at a limit the host evicts its oldest incoming connection that has not completed the
session handshake (DISCONNECT to it, `Disconnected(Refused)` locally; the session marks admitted players with
`Transport::MarkEstablished`) and accepts the new one. Established players are never evicted; with only those left
the new connection gets REFUSE(full). Any DATA/PING from the host also completes the
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

**Which steam_api.** Core never relies on the game's own steam_api: Mafia DE, the first game, does not even load the
`steam_api64.dll` in its folder (it memory-loads an embedded 2015 steam_api without a flat API; MafiaDE
`docs/re-notes.md` 3.1). So the dll loads the on-disk `steam_api64.dll` itself (`LoadLibraryW` from the game folder,
falling back to a copy next to the mod dll), three seconds after the first
frame calls ITS `SteamAPI_Init` (a second pipe to the same running Steam client; the game's own Steam keeps working)
and from then on calls ITS `SteamAPI_RunCallbacks` once per frame from `Coop::Tick` (`src/dll/coop.cpp`). We never
call `SteamAPI_Shutdown`. Verified in game: `SteamAPI_Init` succeeds in two concurrent instances, persona name and
SteamID are read, and a friends-only lobby is created.

**Interface versions.** The reference is Mafia DE's on-disk `steam_api64.dll` (Steam Client API, source-control id
5697398). Its
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
  (default: `Join`). The callbacks can fire inside `SteamTransport::Poll` (it pumps Steam callbacks), i.e. inside
  `Session::Poll`, so the dll's handler only records the lobby id; `Coop::Tick` leaves the current session and joins
  at the start of the next tick, after Steam polling and before the session is polled.
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
