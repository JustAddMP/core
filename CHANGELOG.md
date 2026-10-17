# Changelog

All notable changes to JustAddMP core. Versions follow MAJOR.MINOR.PATCH (see README.md "Versions"); each release
is a git tag `vX.Y.Z`. Breaking changes for game repos are marked **Breaking**.

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
