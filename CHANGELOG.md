# Changelog

All notable changes to JustAddMP core. Versions follow MAJOR.MINOR.PATCH (see README.md "Versions"); each release
is a git tag `vX.Y.Z`. Breaking changes for game repos are marked **Breaking**.

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
