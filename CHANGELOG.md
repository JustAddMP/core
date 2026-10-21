# Changelog

All notable changes to JustAddMP core. Versions follow MAJOR.MINOR.PATCH (see README.md "Versions"); each release
is a git tag `vX.Y.Z`. Breaking changes for game repos are marked **Breaking**.

## v0.14.1 � 2026-10-21

One-PC test tools and a test input, additive: game repos keep building without changes. The wire protocol stays
9 (no new messages). Used for the one-PC checks of MafiaCoop 0.17.1 (two instances over UDP with `--net-sim`,
the windows placed with `--window-area`, the rejoin typing the host's IP); not yet between two PCs or over Steam.

- Test input: `text <chars>` types characters into our panels' text boxes (the one-PC rejoin types the host's IP
  into JOIN BY IP); tests/test_input.cpp.
- `net/net_sim.h`: `--net-sim latency=12,jitter=3,loss=0,dup=0` (ini `net_sim`, any section): the UDP transport
  delays (drops, duplicates) what this game sends (`UdpOptions::sim*`, seeded per process); never Steam P2P. Logged
  loudly at the transport start and in the session start block, `SES part=netsim on latency jitter loss dup udp value`.
  `tests/net_sim.cpp`: parsing, the launcher passing it on, two transports on loopback at 23 ms round trip.
- `dll/window_place.h`: `--window-area x0,x1[,y]` (ini `window_area`): every top-level window the game creates
  through its CreateWindowExW / A import is made at x0 (y), no wider than the area (an import table patch from the
  init thread right after hook::Init: a MinHook detour suspends every thread and delayed the game's own hooks).
- launcher: `net_sim` and `window_area` take a value.
- Tests: `net_sim` (new), `test_input` (`text`); `session_log` runs 11.5 s for its two SCK records (10.6 s left one
  out about 1 run in 7).

## v0.14.0 � 2026-10-20

Session diagnostics for two-PC tests: everything about a co-op session in the main log, the sync trace and one zip per
session, so a problem can be diagnosed from the two players' files alone. New features, additive: game repos keep
building without changes. The wire protocol stays 9 (no new messages). Checked in game on one PC with MafiaCoop
0.17.0 (two instances over UDP: the start block, summaries, clock samples, end totals, the bundle at each game exit
and a -recovered one after a killed game); not yet between two PCs or over Steam. Unit and end-to-end tests over UDP.

- `dll/session_log.h` (driven from `Coop::Tick`): while a session's trace is open, a `session: ---- session start`
  block in the log and `SES` records (versions, handshake build, protocol, the exe's CRC-32 read on a background
  thread and the `GameInfo` build it matches, role, local player and Steam account, transport, port, LAN address and
  Steam lobby, every setting with password-like values redacted, window / backbuffer / fullscreen / monitor / DPI,
  OS, RAM, fps, the log / trace / bundle paths, every peer with its Steam id and address, later joiners too); `SCK`
  every 5 s (trace time vs the session clock, offset and its uncertainty, rtt); `SUM` every 10 s (per peer rtt, loss,
  KB/s and packets in / out, resends, queue; KB/s per reliable lane; fps and the lowest frame; warnings / errors; the
  game's numbers) and a `session: summary` log line every 60 s; at the end `FIN` records and a `session: ---- session
  end` block (duration, peers, totals per peer and lane, the busiest messages, worst latency and loss, the game's
  totals, warnings / errors with the five most repeated call sites, the bundle's path).
- `core/diag.h`: named levels and counts a game layer publishes for those summaries (`Set`, `Total` for a running
  count it keeps, `Add`); per-window deltas, formatting into lines, at most 512 names.
- `core/bundle.h`, `core/zip_writer.h`: at the end of each session (and at a game exit with one open; the exit waits
  for it, at most 20 s) `logs\bundles\<mod>-<yyyymmdd>-<hhmmss>-<role>.zip` with the session's trace, the main log,
  the settings file (passwords redacted), the launcher logs of the last 24 h, the crash dumps written since the
  session started and a manifest.txt; written on a worker thread to `.tmp` and renamed when complete; the newest 30
  kept. At start the newest trace of this log without a bundle (crash, kill) gets a `-recovered` bundle with the
  previous log. Deflate of our own (LZ77 with hash chains, dynamic Huffman blocks), CRC-32 and the zip container
  without zip64 (an entry stops at 2 GiB with a note); about 165-200 MB/s and 7-9x on real traces.
- `core/log.h`: ten previous logs kept per log name (was five); warnings and errors rate limited per call site (the
  format string): 10 per 60 s, then `repeated N more times in 60 s (rate limited; last: ...)`; per-level and per-site
  totals (`GetStats`), `FlushRepeats` (every 10 s), `Path()`.
- `core/trace.h`: one file stops at 1 GiB (was 1.5 GiB), and the folder is capped at 4 GiB in all (the oldest go
  first; still at most 20 files); `SetCloseHooks` (before / after a trace closes), `PruneFolder`.
- `dll/window_place.h`: `Window()` (the game window, for the header). `mod.cpp`: the exit hook closes an open session's
  trace with its summary before leaving the session and waits for its bundle afterwards.
- `net/remote_motion.h` `LocomotionController::Applied`: a game layer that fixes a position itself tells the
  controller the nudge really applied, so the walk measurement (calibration) and the copy's walking velocity leave
  it out (the on-foot velocity matching of MafiaCoop 0.17.0); test `net_motion` TestApplied.
- Tests: `zip_writer`, `session_diag`, `session_log` (new), `core_trace` (log runs, repeats, close hooks, byte cap).

## v0.13.0 � 2026-10-20

An update check: once per game start the mod asks justaddmp.com whether a newer release of it is out and, if so,
tells the player at the main menu. New features, additive: game repos keep building without changes (the new
`GameInfo::updateId` defaults to no check). The wire protocol version stays 9. **Not checked in a game yet**; the
check itself ran against the live endpoint from a test harness.

- `core/update_check.h`: `GET https://justaddmp.com/api/version/<GameInfo::updateId>` on a thread of its own (WinHTTP,
  5 s per step), once per game start, from the overlay's first frame. When the answer's `latest` is newer than
  `modVersion`, a banner like the join/leave ones ("UPDATE AVAILABLE: MafiaCoop v0.16.0 is out (you have v0.15.0).
  Get it at justaddmp.com") shows for 20 s once the main menu is up (`GameModule::LobbyCanOpen` and not `InWorld`),
  and CO-OP SETTINGS keeps a line "UPDATE AVAILABLE: V0.16.0 AT JUSTADDMP.COM". Offline, a timeout or an odd answer
  only write a log line; nothing waits for the check.
- Versions: optional `v`, one to three numbers, pre-releases (`-beta.2`, ordered as in semver, below the release)
  and ignored build suffixes (`+5`); anything else is not compared.
- Testing: `--update-as <version>` (dll: `<envPrefix>UPDATE_AS`) checks as if the mod were that version;
  `--update-url <url>` asks another server.
- `core_version.h`: `kCoreVersionMinor` said 11 in v0.12.0; now 13 with `kCoreVersion` "0.13.0".
- Tests: `update_check` (new).

## v0.12.0 — 2026-10-20

One replication layer for every kind of copy a game layer shows (docs/replication.md). New features, additive: game
repos keep building without changes (new fields have defaults). The wire protocol version stays 9; the only new wire
form is NetRef's 6 bytes, used by game messages. **Nothing here is checked in a game yet**; the numbers below come from
unit tests with simulated copies.

- `net/replica.h`: `SnapshotTrack` and `Stream`, snapshot interpolation in one place: one playout clock per sender,
  Hermite between samples, no interpolation across a jump of more than 20 m, extrapolation for at most 0.25 s then
  held, a `Present` lead of at most 1.5 m (tunable). With its default tuning it gives the same results as the Mafia DE
  player interpolation it replaces (net_replica compares them on 6000 queries).
- `net/correction.h`: one `Corrector` for every kind of copy, with foot, car, NPC and traffic presets: nothing inside a
  dead zone, beyond it a blend capped at a share of the target's speed (0.15 m/s when it stands), a snap when far off,
  on a height difference, on a teleport or when stuck.
- `net/authority.h`: `NetRef {home, kind, local}` (6 bytes on the wire); `OwnershipBook` (claim epochs: the higher
  epoch wins, an equal one keeps the owner, simultaneous claims go to the lower id, a leaving player's things go to
  the fallback owner); `RegionAuthority` (who simulates the ambient world: the host within 250 m of himself, else the
  nearest player, kept until 400 m away and another player is 50 m nearer).
- `LocomotionController`: a gait hint (the sender's own gait is used as is, only the speed multiplier absorbs the
  error); hidden nudges (dead zone 0.3 m, was 0.9; at most max(0.15, 0.35 x speed) m/s); commands planned for the
  copy's response lag (0.15 s, an estimate); a braking target's stop point; the carried shortfall only while the copy
  is behind. Simulated AI walker (speeds off by 20 %, 150-200 ms lag, turn limit): gait 95-98 %, p95 0.34-0.41 m
  moving, 0.07-0.09 m standing, no snaps. The response-lag planning and the new nudge rule also apply without a hint.
- `VehicleController`: a pinned copy gets its physics back after 1.5 s, doubling to 8 s (was 5 to 40 s); the doubling
  resets after 10 s keeping up. `pinnedFor` and `retryIn` for traces.
- Tests: `net_replica` (new); `net_motion` gait hint (simulated AI walker) and pin policy.

## v0.11.0 — 2026-10-18

A JOIN FRIEND panel, a working physical gamepad on the co-op panels with button prompts like the games' menus,
automatic reconnect after a lost connection, and lower latency (reliable lanes, send priorities, a session clock and
an adaptive playout delay). New features, so a minor release. Game repos keep building without changes (the new
`GameModule` reconnect hooks have defaults; lanes and priorities are opt-in per message id). The wire protocol
version goes to 9 (8: reconnect trailers and PeerDropped; 9: UDP lane units and the session clock), so players need
the same build.

Tested in game with Mafia DE 0.15.0 on one PC (two instances, 2026-10-18): the JOIN FRIEND panel and its messages
(no friend was playing, so listing and joining a friend are not checked), the pad prompts switching between pad and
keyboard / mouse, reconnect after 15 s (same slot) and after 66 s (slot freed), the session clock and playout
numbers. The pad guard and `open friends` fixes below came from that test and are not yet checked in game. Two PCs,
a real friend list and a physical pad are not tested yet.

- JOIN FRIEND panel (`net/steam_friends.h`): the Steam friends who play the game, with what they are doing:
  hosting, in a friend's game, checking, not hosting, other version, full. Found through the friend's lobby
  (`GetFriendGamePlayed`), else his rich presence `connect`; the lobby data `mc_version` / `mc_proto` / `mc_host` /
  `mc_state` / `mc_mode` / `mc_players` / `mc_max` says the rest. `Coop::GetFriends` / `RefreshFriends` /
  `JoinFriend`; `MenuCommand::JoinFriend` opens this panel instead of the Steam overlay.
  - The version is checked when entering a lobby (invites too), before connecting; Steam lobby join errors
    (`EChatRoomEnterResponse`) are shown in words; rich presence is refreshed every 30 s.
  - `steam_flat`: the friends exports used above, `SetApiForTest`.
- Physical gamepad on the panels (pad navigation):
  - Every XInput user slot is read (not only the first), empty slots are probed once a second, connect / disconnect
    is logged; an unplugged pad no longer blocks the game's input.
  - `PadNav` (`src/dll/pad_nav.h`): presses only, stick hysteresis with the dominant axis (left / right too),
    repeat, a button held when the panel opens is ignored, B / START / BACK = back; it also reports X / Y / LB / RB.
  - The packet number is bumped when the block changes; DirectInput joysticks are blocked with frozen axes.
- Button prompts (`src/dll/ui_prompts.h`): the panel footers follow the input device like the games' menus: gamepad
  glyphs (A / B / X / Y, LB / RB, d-pad) after pad input, key names after keys / mouse (`DeviceTracker`, fed from
  every XInput read and the window messages; switches are logged). One prompt list per panel drives both the footer
  and the pad's X / Y / LB / RB: lobby X Start / Ready / Join game, Y Invite; CO-OP panel Y Invite; JOIN FRIEND
  Y / F5 Refresh; text boxes X / Backspace Delete, Y / Ctrl+V Paste. Footers shrink to fit the panel; X / Y / LB / RB
  held after a panel closes keep the game blocked like A / B. The test dump and the panel-open log show the footer.
- Reconnect after a lost connection (docs/protocol.md "Reconnect"):
  - Host: a guest whose connection timed out or failed (not a clean leave, kick or ban) keeps his slot for
    `reconnectHoldS` (60 s); the others get `PeerDropped` (0x0D) and see him in `Session::DroppedPlayers()`.
  - Guest: the session stays (`SessionState::Reconnecting`, `IsReconnecting`, `ReconnectLeftS`) and rejoins the same
    address with backoff for `reconnectWindowS` (60 s); the resume token from the Welcome gets the same slot and id
    back (and skips the password; bans still apply). LEAVE CO-OP ends the retries.
  - Hello / Welcome carry optional trailers (resume token, resumed flag). `PlayerInfo::joinSeq` counts entries into
    `Players()` so games that diff it see a returning player as new (`net::Lobby` does).
  - `GameModule::OnPlayerDropped` (default: as if he left), `OnConnectionLost`, `OnReconnected(sameSlot)`;
    session callbacks `onPlayerDropped` / `onPlayerResumed` / `onConnectionLost` / `onReconnected`.
  - The CO-OP panels and banners show the state ("CONNECTION LOST - RECONNECTING... 12 S"); over Steam the guest
    re-enters the host's lobby every 10 s if he is no longer in it (UNVERIFIED whether a P2P drop leaves the lobby).
- Latency (docs/protocol.md "Latency"):
  - `Transport::Flush` / `Session::Flush`: `Coop::Tick` flushes right after `GameModule::SessionTick`, pings are
    flushed at once (a message no longer waits up to a frame for the next Poll). UDP fast retransmit as soon as a
    later unit is acknowledged, RTO floor 30 ms; Steam unreliable data goes `NoDelay` once pongs prove it works.
  - Lanes: `Session::SetLane(firstMsg, lastMsg, lane)` puts reliable app messages on one of 8 ordered streams
    (`Transport::SendOnLane`; UDP lane units with lane / laneSeq / barrier, Steam lane channels 7 + k). A big or lost
    message only holds back its own lane; lane 0 never waits for another lane; lane messages still come after the
    lane-0 messages sent before them. Once a connection uses a lane every reliable unit is laned (lane-0 too), so a
    build that never uses lanes sends what protocol 8 sent.
  - Send priorities: `Session::SetPriority(firstMsg, lastMsg, Critical / Normal / Bulk)`, a per-peer send budget
    (`sendBudgetBytesPerS`, `net_budget_kbps`) that follows loss and the reliable queue; bulk states are thinned and
    bulk reliable messages deferred in lane order when the link cannot carry everything; `thinned` / `deferred`
    counters and NetMonitor lines.
  - Session clock: the Pong carries the host's clock (u64 trailer), burst pings after a join, `SessionTime` /
    `SessionTimeMs` / `ClockSynced` / `ClockErrorMs`. Every app message carries a send stamp once the clock is synced
    (App flags bit1, u32, kept by the relay): `Session::MessageAgeMs` / `MessageStampMs` in handlers, per-message age
    in `MsgTraffic` and NetMonitor; `Session::onAppMessage` sees every app message with its age.
  - Playout (`net/playout.h`): `PlayoutClock`, an adaptive playout delay from arrival lateness (windowed-minimum
    offset, slewed render clock) instead of a fixed delay; `Hermite` and `Extrapolate` helpers.
  - `UdpOptions::testDrop` for loss tests.
- Pad guard fix: after a pad button closed a panel, the keyboard stayed blocked for good while the game did not poll
  XInput (a loading screen ignored Enter for minutes after the lobby was closed with pad X). The release guard now
  reads a held pad state older than 100 ms again (`PadHold`, `src/dll/pad_hold.h`); an unreadable pad counts as
  released.
- Test input: `open friends` (the JOIN FRIEND panel) is accepted by the script parser, as `CoopUi::OpenForTest`
  supports it.
- `kProtocolVersion` 9.
- Tests: `steam_friends` (against a fake Steam), `pad_nav` (PadHold too), `ui_prompts`, `net_reconnect` (four players:
  two guests lost at once come back into their own slots), `net_lobby` (a full lobby of four), `net_latency` (flush,
  fast retransmit, lanes, priorities, session clock, message age, playout under jitter / loss / drift),
  `net_loopback` lanes under loss, `test_input` open panels.

## v0.10.0 — 2026-10-17

Sync traces for comparing two games after a test, and remote motion fixes from the first real two-PC test (Mafia DE
0.13.0: car copies never rolled, sprinting copies fell up to 3 m behind). New features, so a minor release. No breaking
changes; the wire protocol version stays 7.

- `core/trace.h`: a per-session trace file next to the main log (`logs\trace\<log name>-<date>-<time>.trace`, the
  newest 20 kept, at most 1.5 GiB each). Line based, "`<t> <TYPE> <fields>`" with `t` the QueryPerformanceCounter
  clock in seconds; buffered (written twice a second), thread-safe. While a trace is open every main log line is
  copied into it, so one file holds the whole session. A game layer writes its own records (states sent and
  received, controller decisions, readbacks from the game).
- `core/log.h`: the last five runs' logs are kept (`.1` newest .. `.5`), not just one; `log::Dir()` /
  `log::BaseName()`.
- `LocomotionController`:
  - When even the fastest multiplier (now 1.8, was 1.5) cannot reach the wanted speed, the shortfall is carried as
    a small nudge along the walk, so a copy whose AI sprint is slower than a player's (Mafia DE: ~3.3 m/s against
    ~6 m/s) keeps up instead of falling behind and snapping. What the copy reaches is the calibrated speed or, if
    less, what it was last measured doing; not right after a gait change (that is acceleration, not a shortfall).
  - A gait whose measured speed is far (> 30 %) off the calibration learns faster.
  - The command reports the wanted speed, the last measured speed and the carried part (traces, overlay).
- `VehicleController`:
  - Every command carries a wanted velocity for the copy (the target's plus a bounded share of the error,
    1.5/s up to 4 m/s): a game whose position setter stops the body gives it back right after a correction, so the
    copy keeps rolling under its own physics (a new test simulates exactly that and is never pinned).
  - A pinned copy gets its physics back after 5 s, then 10, 20, 40 s after each new pin (what kept it from moving may
    be gone). Why it was pinned (too slow / ran off / stays off), pin and retry counts, target and copy speed and the
    rotation error are reported.
- `net_motion` test: slow AI sprint, a zeroing teleport with the velocity given back, pin retries.

## v0.9.0 — 2026-10-17

Remote motion controllers for games that show other players as locally simulated copies, and launch-time names
that stay launch-time. New features, so a minor release. No breaking changes; the wire protocol version stays 7.

- `net/remote_motion.h` (pure, no game calls):
  - `LocomotionController`: from the remote target (position, velocity, heading, sprint / in-air flags) and the
    copy's reported position, a gait (stand / walk / run / sprint, with hysteresis), a speed multiplier for the
    game's own locomotion (the walk/run/sprint speed at multiplier 1 is calibrated at run time from the copy's
    measured speed, so a game's nominal speeds need not be known), a walk point ahead along the wanted velocity (or
    the spot itself when the remote player stands) and when to re-aim it, a position fix that stays small (only the
    error beyond 0.9 m is nudged, 2/s; a snap beyond 3.5 m / 3 m height, or when the copy is blocked), and a
    rate-limited turn toward the remote heading while standing. Meant to replace "pull the copy toward the target
    every tick", which slides its feet.
  - `VehicleController`: keeps a car copy on its target while its own physics carries it (blended position and
    rotation correction; nothing applied while it is on target, so the game's own animations on a standing car are
    not disturbed), switches to pinning it every tick when its physics does not keep up (the game's teleport zeroed
    its speed), runs off or stays more than 1.5 m off for 1.5 s, snaps on teleports / beyond 4 m, and governs the throttle (none while the copy is faster
    than its target).
  - `PackSnorm8` / `PackUnorm8` (and unpack) for control values on the wire; `Nlerp` for quaternions.
- `Config::GetFileString(key)`: the value from the settings file (or a saved `Set`), ignoring launch overrides.
- CO-OP SETTINGS > NAME: the box now shows (and SAVE writes) the name saved in the settings, not a `--name` given on
  the command line for one run (a test's `--name Host` was saved for good by pressing SAVE). The log says when the
  name comes from the command line, so the Steam name is not used.
- `core_version.h`: the minor number said 7 since v0.8.0; now 0.9.0 throughout.
- Tests: `net_motion` (quantisation round trips and clamping, nlerp sign fix; the locomotion controller against a
  simulated AI walker whose real speeds differ from the first guess: the gait follows stand / walk / run / sprint /
  turn / stop, no snaps, average error 0.14 m, no position nudges in steady motion, speeds calibrated to within 2 %, a
  blocked copy snapped after 2 s, a 180 degree standing turn in 0.43 s; the vehicle controller with a physics that
  carries the car (blended, 0.02 m) and one whose teleport zeroes its speed (pinned after 1.5 s), snap, overspeed,
  runaway, lag); `config_set` checks `GetFileString`.

## v0.8.0 — 2026-10-16

Faster detection of a game that was killed, without dropping a player whose game is only busy; a key press for the
game. New features, so a minor release. No breaking changes; the wire protocol version stays 7 (the new UDP packet
type is ignored by older peers, which only refresh their timeout on it).

- UDP keepalive thread (`UdpOptions::keepaliveThreadS`, default 0.25 s): when `Poll()` has not run for twice that
  long (the thread that polls is stalled, e.g. a game loading a world), a thread of the transport sends an ALIVE
  packet (type 9, header only) to every connected peer, so a short timeout does not drop a busy player. It gives up
  after `keepaliveMaxStallS` (60 s) of stall: a hung process is dropped like a killed one. `KeepalivesSent()`.
- The dll's timeouts are settings: `net_timeout_s` (UDP, default **4 s**, was 10 s) and `net_steam_timeout_s`
  (Steam P2P, default 6 s, pings every 0.5 s; the Steam transport has no keepalive thread), both 2..60. A killed game
  is now noticed after about 4.5 s over UDP. Measured in Mafia DE: the frame (and so `Poll()`) never stalled more than
  0.2 s, also during loading screens.
- `ui_input::PressGameKey(dik, holdMs)`: a key press the game itself sees (the keyboard's DirectInput state and
  buffered events), for prompts like a loading screen's "Enter: continue". Ignored while one of our panels captures
  input; a game reads DirectInput only while its window has focus.
- Tests: `net_loopback` "keepalive thread covers a stall" (5 s without `Poll()`, host timeout 2 s: still connected),
  "hung guest dropped after the stall cover" (1.5 s cover + 2 s timeout: dropped after ~3.3 s); the 10 s timeout
  test runs with the keepalive thread off (a killed process).

## v0.7.0 — 2026-10-16

Test input for the co-op panels (a new feature for tests, so a minor release). No breaking changes, no wire protocol
change, nothing changes for players (off unless `test_input = 1`).

- `dll/test_input.h`: with `test_input = 1` (ini) / `--test_input 1` the dll polls `<dll dir>\dev\<log tag>.input.txt`
  on the render thread (it keeps running while the game's scripts are paused in menus) and runs it line by line:
  `pad <buttons> [ms]` (the test pad, which it turns on), `key <vk>`, `move x y`, `click x y`, `wheel +1|-1`,
  `wait ms`, `open pause|lobby|settings|joinip`, `dump` (the open panel's rows, hit boxes and selection into the log),
  `log text`. The parser is header-only (`dll/test_input_script.h`).
- `ui_input::InjectEvent` (an event as if from the window, backbuffer coordinates), `CoopUi::Describe`,
  `CoopUi::OpenForTest`.
- Tests: `test_input` (every command, comments, CRLF, bad lines skipped and reported with their line number, button
  names).

## v0.6.0 — 2026-10-16

Traffic statistics and higher bandwidth limits (new features, so a minor release). No breaking changes: game repos
update without code changes; the new interface call is optional. No wire protocol change.

- Traffic statistics (docs/protocol.md "Traffic statistics"): `Session::Traffic()` counts messages and bytes per app
  message id (sent per destination, received, relayed, refused sends); `PeerStats::reliableQueued` (UDP) is the
  reliable backlog; `net::NetMonitor` turns them, the transport stats and the rate-limit drops into rates. The dll
  logs a report every `net_log_interval` s (default 5) and shows the last second in the debug status text (F7).
  Optional `GameModule::MessageName(id)` names the game's messages there.
- Higher limits: the host's relay limit per guest is 1 MB/s with a 2 MB burst (was 64 KB/s, 128 KB), set from
  `net_budget_kbps` (KiB/s, default 1024); `Coop::NetBudgetBytesPerS()` hands games the same number. The interest
  manager's default budget is 512 KB/s per peer with a 0.25 s burst (was 64 KB/s, 0.1 s).
- Tests: `net_loopback` "throughput" (a guest streaming ~450 KB/s through the host, unreliable states at 60 Hz plus a
  47 KB reliable message twice a second: nothing dropped by the relay limit, every reliable message delivered,
  counters and monitor agree); the rate-limit test pins the old small limits; `net_interest` "default budget" (a busy
  co-op scene, ~175 KB/s for one peer, nothing deferred).

## v0.5.0 — 2026-10-15

Pre-game lobby, co-op settings panel, HUD drawing for games, clean exit (new features, so a minor release). No
breaking changes: game repos update without code changes; every new interface call is optional. The wire protocol
changes (`kProtocolVersion` 7, the lobby messages), so players need the same build anyway.

- Lobby (`net/lobby.h`, docs/protocol.md "Lobby"): guests ready up, the host picks a mode from the game's list and
  starts it for everyone; a late joiner sees the game in progress and can follow it. The dll attaches it to every
  session and writes `mc_state` / `mc_mode` into the Steam lobby data (`SteamLobby::SetData`). New test `net_lobby`.
- Co-op UI: a LOBBY panel (players with character and READY / NOT READY / HOST, the game's character rows, READY or
  MODE + START, JOIN GAME while the host plays, INVITE FRIENDS, SETTINGS, LEAVE); it opens by itself after joining a
  host while not in the world (once `GameModule::LobbyCanOpen()`), closes on every screen when the game starts and
  when the session ends. A CO-OP SETTINGS panel (NAME with a text box, DEBUG OVERLAY, the game's rows; saved with
  `Config::Set`), also reachable from the pause CO-OP panel. New `MenuCommand::Lobby` / `Settings`.
- GameModule (all optional): `SettingsOptions` / `SettingsOptionStep`, `LobbyModes` / `StartMode`, `InWorld` /
  `CurrentMode` / `LobbyCanOpen`, `PlayerCharacter`, `DrawHud` (game geometry under the panels, e.g. name tags).
- Clean exit: `ExitProcess` and `TerminateProcess(self)` are hooked; the session is left cleanly before the process
  ends, so the others see "host left" / "<name> left the game" at once instead of after the 10 s timeout. Banners:
  **HOST LEFT** for guests, "<name> lost the connection" for a timeout. UDP sends its Disconnect 4 times (was 2).
- Overlay: holds no backbuffer reference between frames; releases everything on device removed/reset and at exit;
  logs the Present / ResizeBuffers threads. `Mod::DebugOverlay` / `SetDebugOverlay`, `Coop::SetName`.

## v0.4.0 — 2026-10-15

Game settings in the CO-OP panel, saving settings, a virtual gamepad for tests (new features, so a minor release). No
breaking changes: game repos update without code changes; the new interface calls are optional. No wire protocol
change.

- GameModule: optional `PanelOptions(rows)` / `PanelOptionStep(index, delta)`. The pause CO-OP panel shows each row as
  "LABEL  < VALUE >" in every session state (also without a session); Left/Right and the pad d-pad step it, Enter,
  pad A and a click step it forward, and the key hints show "Left/Right Change" on such a row. Mafia DE uses it for
  the player's CHARACTER and OUTFIT.
- Config: `Set(key, value)` changes a value for the process (it then wins over the file and the launcher's environment
  overrides) and saves it into the settings file: the first line for that key is replaced, everything else is kept,
  a missing key is appended, a missing file is created (written next to it and moved over it). Config is now
  thread-safe. New test `config_set`.
- ui_input: a test pad (`EnableTestPad`, `SetTestPad(state, holdMs)`): a virtual XInput gamepad on user 0 (also
  reported by `XInputGetCapabilities`, which is now hooked too), on top of a real pad's state, read by the game and by
  our panels. Off unless `test_pad = 1` / `--test_pad 1`. Synthetic mouse clicks never reach some games; a pad does.
- CO-OP panel fix: only a real mouse move selects the row under the cursor. A game that keeps the cursor at the
  window centre sends mouse moves to the same spot every frame, which pinned the selection to the row under the centre
  and undid every keyboard and pad step.

## v0.3.0 — 2026-10-15

Kick and kick-and-ban (new features, so a minor release). No breaking changes: game repos update without code
changes; everything new in the interfaces is additive. The wire protocol changes (`kProtocolVersion` 6), so players
need the same build anyway.

- Session: host-only `Kick(id, reason)` and `KickAndBan(id, reason)`. The kicked guest gets a reliable Kicked
  message (0x0C) with the reason and is dropped after the same 1 s linger as a Reject; its `onDisconnected` gets
  "You were kicked by the host[: reason]" (`WasKicked()` / `WasBanned()`), the other guests get PeerLeft "kicked by
  the host". Guests cannot kick (the API refuses, a Kicked sent to the host is ignored).
- Ban list for the hosted session (`BanList()`, `Unban(index)`, `ClearBans()`), keyed by the peer's address (UDP IP
  without the port) and its SteamID (new optional `Transport::PeerSteamId`, implemented by the Steam and multi
  transports). A banned Hello gets Reject 7 "You are banned from this session" (`RejectReason::Banned`). `Leave()`
  and every `Host()` clear the list.
- Co-op UI: in the pause CO-OP panel the host can select a guest in the player list (keyboard up/down, mouse,
  gamepad) and choose KICK or KICK AND BAN, confirmed with "Kick <name>? Enter = yes, Esc = no". Guests see the
  banner "You were kicked by the host" / "You are banned from this session" and go back to idle.
  `Coop::Kick`, `Coop::BanList`, `Coop::Unban`; `Coop::PlayerRow::id`. No new `MenuCommand`.
- Tests: new `net_kick` (reason delivered under loss, PeerLeft to the others, guests cannot kick, ban by address and
  by SteamID through a test transport, unban, ban list cleared by a new session, kicked-not-banned rejoin).

## v0.2.0 — 2026-10-15

Stability fixes plus a few small optional additions to the interface (so a minor release). No breaking changes: game repos update without code changes.

- Steam joins: an invite / "Join Game" request is only recorded in the Steam callback (which can run inside
  `Session::Poll`) and acted on at the start of the next `Coop::Tick`, so the session is never torn down while it is
  being polled (`src/dll/pending_join.h`).
- Crash dumps: only an exception that ends the process (unhandled filter, or heap corruption / stack buffer
  overrun) uses up the one-shot dump, and only once the dump was written; a caught first-chance stack overflow no
  longer prevents the dump of a later real crash. The dump is written from a writer thread started at install time;
  DllMain keeps 64 KB of stack back on every thread (>= 1 MB stacks) so an unhandled stack overflow still reaches
  the handler; on a thread without that reserve a stack overflow gives a `crash-<time>-firstchance.dmp` instead.
  The mod dll no longer calls `DisableThreadLibraryCalls`.
- Spam protection: the host rate-limits guest chat (5/s, burst 10) and the app messages it relays for a guest
  (64 KB/s, burst 128 KB); excess is dropped, counted (`Session::RateLimits()`) and logged at most once per second.
  New `SessionConfig` fields `chatPerS`, `chatBurst`, `relayBytesPerS`, `relayBurstBytes`. The UDP transport, when
  its incoming slots are full, evicts the oldest connection that has not completed the session handshake instead of
  refusing everyone; established players are never evicted (new optional `Transport::MarkEstablished`, called by
  the session; `UdpTransport::Evictions()`).
- Launcher: keys that need a value (`--game`, `--join`, `--join-lobby`, `--log`, `--model`, `--name`,
  `--password`, `--port`, `--transport`, `--window`, `--window-timeout-s`; table in `src/launcher/args.h`) refuse a
  missing, empty or flag-looking value (`--password --name x`) with an error (message box, or printed with
  `--check-only` / `--yes`) and exit 1 without starting the game. Bare flags stay bare.
- Tests: new `pending_join`, `crash_dump`, `launcher_args`; `net_loopback` (rate limits) and `net_fuzz` (eviction)
  extended. Wire protocol unchanged (`kProtocolVersion` 5).

## v0.1.0 — 2026-10-15

First release, split out of the Mafia: Definitive Edition co-op mod (JustAddMP/MafiaDE).

- Game descriptor (`include/mc/game_info.h`) and game-module interface (`include/mc/game_module.h`).
- Launcher: Steam game lookup, exe build check (CRC32, cached), injection, argument passing, `--install` /
  `--uninstall` of the optional start-from-Steam `version.dll` proxy.
- In-game: logging, crash handler with minidumps, MinHook wrapper, pattern scanning, Present hook, D3D11 overlay
  with own text renderer, co-op panels and banners, Scaleform (SWF/AVM2) patcher.
- Networking: UDP transport with reliability and fragmentation, Steam P2P transport, Steam lobbies and invites,
  host-authoritative session with version check, relay, ping, optional password (HMAC-SHA256 challenge), interest
  manager (range, rates, per-peer byte budget).
- Tools: `crashinfo.py` (symbolised crash dumps), `fontgen`, `embed_lua.ps1`.
- Tests: net_loopback, net_password, net_interest, net_fuzz, proxy_test, steam_smoke.
