# JustAddMP core

The shared, game-agnostic part of the JustAddMP co-op mods: launcher (finds a Steam game, checks its exe build,
starts it with the mod dll injected), optional "start from Steam" version.dll proxy, and inside the game: crash
handler, logging (plus a per-session sync trace for comparing two games), MinHook hooks, Present hook + D3D11 overlay, Steam P2P / UDP networking with a host-authoritative
session, Steam lobbies and invites, the co-op panels and banners, and a Scaleform (SWF/AVM2) patcher for native menu
entries.

It is not tied to one game or series: it works for any 64-bit Windows game that renders with Direct3D 11
(Steam optional: without it players join by IP). Games on Direct3D 12 or Vulkan need another Present hook, and
32-bit games a 32-bit build.

Everything game-specific lives in a game repo that includes this one as a git submodule, e.g.
[JustAddMP/MafiaDE](https://github.com/JustAddMP/MafiaDE). A game provides:

- a **descriptor**, `mc::GetGameInfo()` (`include/mc/game_info.h`): names, Steam app id, supported exe builds, ...
- a **game layer**, `mc::GetGameModule()` (`include/mc/game_module.h`): its hooks, player sync, menu entries.

Design, the interface and the runtime model: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Network protocol:
[docs/protocol.md](docs/protocol.md). Interest management: [docs/interest.md](docs/interest.md).

## Using it from a game repo

```
git submodule add https://github.com/JustAddMP/core.git external/core
```

The game's `build.cmd` calls `external\core\tools\msvc.cmd` (toolchain + common flags), then
`external\core\build.cmd <game out>\obj\core` (core libraries only), compiles its own sources with
`/I external\core\src /I external\core\include` and links the mod dll, the launcher and the proxy as described in
docs/ARCHITECTURE.md "How a game repo consumes core". Fix generic problems here first, then bump the submodule in
the game repo.

## Build and test

Needs Windows 10/11 x64 and Visual Studio 2022 or newer with the x64 C++ toolset (found through vswhere if `cl.exe`
is not on PATH). No CMake.

```
build.cmd                    core libraries, test binaries and tests -> out\
out\net_loopback.exe         two sessions over UDP in one process, with loss/latency/duplicates
out\net_password.exe         session password (right / wrong / missing / attempt limit)
out\net_kick.exe             kick and kick-and-ban (reason delivered, PeerLeft, guests cannot kick, bans by
                             address and SteamID, ban list cleared by a new session)
out\net_reconnect.exe        reconnect after a lost connection (slot held, same id back, others see drop + return,
                             held slot counts as taken, hold / window expiry, no reconnect after a clean leave, host
                             leave or kick, a new connection replacing a stale one, a Hello on the live connection,
                             forged tokens, the token skips the password, kicking a held player)
out\net_lobby.exe            pre-game lobby (ready, mode, start, late joiner, guests cannot start) and clean leaving
                             (host left / guest left reported at once)
out\net_interest.exe         interest management
out\net_motion.exe           remote motion controllers (locomotion against a simulated AI walker, car copies) and
                             8-bit control packing (also a slow AI sprint, a teleport that stops the body, pin
                             retries)
out\core_trace.exe           sync trace (file next to the log, header, log copy, whole lines from 4 threads, only
                             the newest kept) and the five previous logs kept
out\net_fuzz.exe             malformed packets against session and UDP transport
out\pending_join.exe         Steam join requests are handed to the next tick, never acted on mid-poll
out\steam_friends.exe        JOIN FRIEND list against a fake Steam (hosting / guest / not hosting / version / full)
out\pad_nav.exe              gamepad navigation of the panels (presses only, stick hysteresis, repeat, B = back)
out\ui_prompts.exe           panel footer prompts: last input device (pad presses / sticks / triggers vs keys,
                             clicks, real mouse moves), the prompt list per panel kind, X / Y act only through
                             a prompt that shows them
out\crash_dump.exe           crash handler: one minidump for the real crash, none for handled exceptions
out\launcher_args.exe        launcher argument parsing: keys that need a value refuse to start without one
out\config_set.exe           Config::Set saves a value into the ini (only that line changes; wins over env overrides);
                             GetFileString ignores launch overrides
out\proxy_test.exe           version.dll proxy + launcher --install/--uninstall against a fake game folder
out\steam_smoke.exe <copy of steam_api64.dll>    manual: needs Steam running (app 480, Spacewar)
```

Every test exits 0 on success. The test binaries (`mc_test_launcher.exe`, `mc_test.dll`, `mc_test_proxy.dll`) are
built for a made-up game (`tests\support\test_game_info.cpp`), so nothing touches a real game install.
Flags: C++20, `/MT`, `/O2 /Zi` with full PDBs, `/W4 /permissive-` (warning-free); MinHook as C at `/W3`.

## Versions

Core is released as git tags `vMAJOR.MINOR.PATCH` (current version in `include/mc/core_version.h`, history in
[CHANGELOG.md](CHANGELOG.md)). Each game repo pins its `external/core` submodule to one tag, so a core change never
reaches a game until that game is updated on purpose, and a game can always go back to its last working tag.

- **Patch** (0.1.0 → 0.1.1): bug fixes only, no interface change. Always safe to update.
- **Minor** (0.1 → 0.2): new features; existing games keep building and working without changes.
- **Major** (→ 1.0, 1.x → 2.0): changes that need game repos to update their code (game interface, descriptor,
  build layout). Listed as **Breaking** in the changelog.
- While core is 0.x, a minor release may still contain breaking changes; the changelog marks them.
- The network protocol version is separate (`net::kProtocolVersion`); players must run the same game mod version
  anyway, which pins one core version.

Making a release: update `include/mc/core_version.h` and `CHANGELOG.md`, build and run all tests, commit, then
`git tag -a vX.Y.Z -m "core vX.Y.Z"` and push the tag. Game repos then move to it with their `tools\update_core.cmd`.
