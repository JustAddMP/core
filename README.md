# JustAddMP core

The shared, game-agnostic part of the JustAddMP co-op mods: launcher (finds a Steam game, checks its exe build,
starts it with the mod dll injected), optional "start from Steam" version.dll proxy, and inside the game: crash
handler, logging, MinHook hooks, Present hook + D3D11 overlay, Steam P2P / UDP networking with a host-authoritative
session, Steam lobbies and invites, the co-op panels and banners, and a Scaleform (SWF/AVM2) patcher for native menu
entries.

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
out\net_interest.exe         interest management
out\net_fuzz.exe             malformed packets against session and UDP transport
out\proxy_test.exe           version.dll proxy + launcher --install/--uninstall against a fake game folder
out\steam_smoke.exe <copy of steam_api64.dll>    manual: needs Steam running (app 480, Spacewar)
```

Every test exits 0 on success. The test binaries (`mc_test_launcher.exe`, `mc_test.dll`, `mc_test_proxy.dll`) are
built for a made-up game (`tests\support\test_game_info.cpp`), so nothing touches a real game install.
Flags: C++20, `/MT`, `/O2 /Zi` with full PDBs, `/W4 /permissive-` (warning-free); MinHook as C at `/W3`.
