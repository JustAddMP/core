# Changelog

All notable changes to JustAddMP core. Versions follow MAJOR.MINOR.PATCH (see README.md "Versions"); each release
is a git tag `vX.Y.Z`. Breaking changes for game repos are marked **Breaking**.

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
