# Changelog

All notable changes to JustAddMP core. Versions follow MAJOR.MINOR.PATCH (see README.md "Versions"); each release
is a git tag `vX.Y.Z`. Breaking changes for game repos are marked **Breaking**.

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
