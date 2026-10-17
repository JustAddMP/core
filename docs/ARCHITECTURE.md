# JustAddMP core: architecture

The game-agnostic part of the JustAddMP co-op mods: a launcher that starts a Steam game with a mod dll injected,
an optional version.dll proxy for "start from Steam", and inside the game the hooks, networking, overlay and co-op UI
that every game mod needs. Each game lives in its own repo (e.g. `JustAddMP/MafiaDE`) with core as a git submodule
at `external/core`, and adds only what is specific to that game: a descriptor (`GetGameInfo`) and a game layer
(`GetGameModule`).

## Clean-room rule (binding)

Do NOT copy or adapt code from other multiplayer mods. Allowed sources: the games' own binaries and data, public
documentation (Microsoft, Steamworks, Lua manual, Adobe SWF/AVM2 specs), and code we write here. Game addresses,
patterns and struct offsets never go into core: they belong to the game repo, which records how each was found.

## Dependencies

- Windows SDK only (Win32, Winsock, D3D11/DXGI, DbgHelp for crash dumps, BCrypt for the session password).
- `third_party/minhook` (BSD-2, vendored source, `third_party/minhook/LICENSE.txt`; the only third-party code).
- At runtime: the game and Steam. The dll `LoadLibrary`s the `steam_api64.dll` from the game folder itself, calls
  its `SteamAPI_Init` and pumps its `SteamAPI_RunCallbacks` (flat API via `GetProcAddress`; no Steamworks SDK
  headers; see docs/protocol.md "Steam transport").
- Static CRT (`/MT`) so no VC++ redistributable is needed. C++20, MSVC, `/W4` warning-free.

## Layout

```
build.cmd                 core libraries + test binaries + tests (see README.md); tools\msvc.cmd: toolchain + flags
include/mc/
  game_info.h             GameInfo: the game descriptor (names, Steam app id, supported exe builds, proxy names)
  game_module.h           GameModule: the dll-side game interface; MenuCommand/MenuState for native menu entries,
                          PanelOption for game settings in the CO-OP panel
src/
  core/                   log, crash (minidump + log on unhandled exception), hook (MinHook wrapper), config (ini +
                          <envPrefix>KEY overrides; Set() saves a value back into the ini), mem (module info, pattern scan, RIP-relative resolve), compat
                          (features missing on this game build -> banner), names.h (env/kernel names from GameInfo)
  net/                    transport.h (interface), udp_transport (own reliability), steam_transport (ISteamNetworking
                          P2P, flat API), multi_transport, steam_flat (bindings), steam_lobby, session (host-
                          authoritative star, relay, password, kick/ban), wire (byte writer/reader), auth (BCrypt HMAC),
                          interest (interest management, docs/interest.md), lobby (pre-game lobby: ready, mode,
                          start, in-game state; docs/protocol.md "Lobby"), remote_motion (core 0.9: controllers that
                          drive a locally simulated copy of a remote character with the game's own locomotion
                          (gait, speed multiplier calibrated at run time, walk point, small nudges / snaps) and keep a
                          car copy on target (blended correction or pinned, power governor); 8-bit control packing)
  render/                 present_hook (IDXGISwapChain::Present/ResizeBuffers -> per-frame tick on the render thread),
                          overlay + overlay.hlsl (D3D11 text/rect renderer), ui_font (GDI glyph atlas), font8x16.h
  swf/                    swf_patch: SWF/GFX + AVM2 (ABC) patcher (constant pool append, method prologues, assembler)
                          for games whose menus are Scaleform movies
  dll/                    dllmain (init thread), mod (per-frame tick; process-exit hooks: leave the session cleanly),
                          coop (sessions: host / join / Steam lobby / invites / password / kick / the pre-game lobby),
                          coop_ui (pause co-op panel with kick / kick and ban, the LOBBY panel, CO-OP SETTINGS, join-by-IP,
                          password and name boxes, banners),
                          ui_input (panel input; blocks the game's DirectInput/XInput/window input while a panel is
                          open; test pad: a virtual XInput gamepad for automated tests, test_pad = 1), test_input (core 0.7: scripted pad
                          presses, mouse moves/clicks, keys, panel open and dump from dev\<tag>.input.txt on the
                          render thread, so tests reach our panels while the game's scripts are paused; test_input = 1),
                          window_place
                          (--window left|right)
  launcher/               launcher (find the game via Steam, CRC check against GameInfo::builds, start suspended,
                          inject, pass --key value as <envPrefix>KEY), args.h (argument parsing; keys that need a
                          value, e.g. --password, refuse to start without one), install (--install / --uninstall)
  proxy/                  version.dll proxy: forwards the 17 exports to System32's version.dll, loads the mod dll in
                          the game exe only ("start from Steam")
tests/                    net_loopback, net_password, net_kick, net_lobby, net_interest, net_motion, net_fuzz, proxy_test,
                          pending_join, crash_dump, launcher_args, config_set, test_input, steam_smoke (manual)
  support/                test_game_info.cpp (a made-up game), test_module.cpp (a game module that does nothing)
tools/                    msvc.cmd, embed_lua.ps1 (embed a .lua into a header), fontgen.cpp, crashinfo.py
docs/                     ARCHITECTURE.md (this), protocol.md (sessions + transports), interest.md
```

Namespaces: `mc` (GameInfo, GameModule, Mod, Coop, CoopUi), `mc::core`, `mc::net`, `mc::render`, `mc::swf`,
`mc::ui_input`, `mc::window_place`, `mc::launcher`, `mc::proxy`. Game repos put their code in `mc::game`.

## The game-module interface

`include/mc/game_info.h` and `include/mc/game_module.h` are the whole contract. A game repo defines:

- `const mc::GameInfo& mc::GetGameInfo()` in a .cpp of its own that depends on nothing else. It is linked into the
  launcher, the proxy and the dll, so all three agree on every name. Core reads from it: the mod name/version (titles,
  banners, the session build number, Steam lobby data), launcher/dll/ini/log names, the environment prefix for
  launcher arguments, the default UDP port, the Steam rich-presence text, the game's name, exe, Steam app id and
  install folder, extra game arguments, the supported exe builds (CRC32) and the proxy's file names and marker.
- `mc::GameModule& mc::GetGameModule()` (dll only), called by core at these points:

| Call | Thread | When |
|---|---|---|
| `Init(dllDir, logTag)` | init thread | once, after MinHook is up, before core waits for the game window: install game hooks |
| `RenderTick(dt)` | render | every frame, before the co-op tick |
| `OnSessionStart(session)` | render | a session was created (host or join): register app message handlers |
| `SessionTick(dt)` | render | every frame while a session exists, after `Session::Poll` |
| `OnPlayerLeft(id)` | render | a player left the session |
| `OnSessionEnd()` | render | the session is being torn down (leave, disconnect, invite to another game) |
| `PlayerHealth(id)` | render | players panel (optional; -1 = no column) |
| `AppendStatus(lines)` | render | debug status text, F7 (optional) |
| `SetMenuState(state)` / `PopMenuCommand(&cmd)` | render | every frame: the game's native menu entries (optional) |
| `PanelOptions(rows)` / `PanelOptionStep(index, delta)` | render | every frame while the CO-OP panel or the lobby is open: game settings shown as "LABEL  < VALUE >" rows (e.g. the player's character); Left/Right, pad d-pad, Enter/A/click step them (optional) |
| `SettingsOptions(rows)` / `SettingsOptionStep(index, delta)` | render | the game's rows in CO-OP SETTINGS (core adds NAME and DEBUG OVERLAY), e.g. SHOW NAME TAGS (optional, 0.5) |
| `LobbyModes(modes)` / `StartMode(mode)` | render | the modes the host can pick in the lobby (name, available, note); load one when the lobby starts or a late joiner follows the host (optional, 0.5) |
| `InWorld()` / `CurrentMode()` / `LobbyCanOpen()` | render | the local player is in a loaded world, which mode he plays; whether the lobby may open by itself now (not over a title screen) (optional, 0.5) |
| `PlayerCharacter(id)` | render | a player's character for the lobby's CHARACTER column (optional, 0.5) |
| `DrawHud(list, w, h)` | render | every frame: game geometry drawn under core's panels and banners, e.g. name tags (optional, 0.5) |

Features that cannot work on the running exe report to `core::compat::Failed(name, required)`; a required one
disables co-op with a banner, an optional one only shows the banner. The game's own threads (e.g. a script-VM tick it
hooks itself) are the game layer's business: core only calls the points above.

## Runtime model

- `DllMain` signals the launcher (`Local\<modName>Injected-<pid>`) and spawns the init thread. The init thread opens
  the log (`<dll dir>\logs\<logPrefix>[-<tag>].log`), loads the ini (`GameInfo::iniName`), installs the crash handler
  and MinHook, calls `GameModule::Init`, waits until the game window exists, subclasses it (background_run: swallow
  `WM_ACTIVATEAPP(FALSE)` so an unfocused game keeps running; panel input), and hooks Present.
- The Present hook calls `Mod::Tick(dt)` once per frame on the game's render thread: crash-handler re-assert,
  heartbeat, `GameModule::RenderTick`, `Coop::Tick` (Steam callbacks, session, `GameModule::SessionTick`), the
  co-op UI and the overlay.
- Hand-over between the render thread and any game thread the game layer uses is the game layer's job (one mutex per
  subsystem, held only for copies, is the pattern).
- Process exit: `ExitProcess` / `TerminateProcess(self)` are hooked; the hook waits (at most 500 ms) for the current
  frame's Tick to finish, leaves the session cleanly (the others are told at once, docs/protocol.md "Leaving cleanly"),
  releases the overlay's D3D objects and stops further ticks.
- Overlay: no backbuffer reference survives a frame (a view is made and released every frame), so the game's
  ResizeBuffers / swapchain release never trips over us; a Present that reports device removed/reset drops every
  overlay resource (rebuilt on the next frame with a device). Present and ResizeBuffers thread ids are logged.
- Crash dumps: `<dll dir>\logs\crash-<time>.dmp`, one per process, only for an exception that ends the process
  (unhandled, or heap corruption / stack buffer overrun); handled first-chance exceptions never use it up. DllMain
  keeps 64 KB of stack back on every thread (>= 1 MB stacks) so an unhandled stack overflow still reaches the
  handler; on a thread without that reserve a stack overflow gives `crash-<time>-firstchance.dmp` instead (it may
  still be caught). `tools\crashinfo.py` reads them.

## Networking

- `Transport`: `Host(port)`, `Connect(address)`, `Send(peer, bytes, Reliability)`, `Poll(handler)`,
  `Disconnect(peer)`, `Close()`. Peer ids are `uint64_t` (SteamID64 for Steam, synthetic for UDP).
- Two backends: Steam P2P (invites via overlay, NAT traversal) and direct UDP (fallback, and for two game instances
  on one PC, which share a SteamID). The host listens on both at once (`MultiTransport`).
- Session: the host is the authority (no separate server process). Guests talk only to the host; the host relays.
  Version handshake first; mismatched versions are rejected with a readable reason. Details in docs/protocol.md.

## How a game repo consumes core

```
<game>/
  external/core/          this repo (git submodule)
  build.cmd               call external\core\tools\msvc.cmd, call external\core\build.cmd <out>\obj\core,
                          compile the game layer with /I external\core\src /I external\core\include, link:
                            mod dll  = <cb>\entry\dllmain.obj + game objects + <cb>\mc_core.lib + MC_SYSLIBS
                            launcher = <cb>\launcher\*.obj + game_info.obj         (/SUBSYSTEM:WINDOWS)
                            proxy    = <cb>\proxy\*.obj + game_info.obj            (/DLL /DEF:src\proxy\version.def)
  src/game/game_info.cpp  GetGameInfo()
  src/game/module.cpp     GetGameModule() -> the game layer
```

The file names the build produces must match the descriptor (`launcherName`, `dllName`, `proxyBuiltName`).
Core's `tests\proxy_test.cpp` can be built by the game against its own descriptor to test its launcher and proxy
without the game. Generic fixes go to core first (its own tests must pass), then the game repo bumps the submodule.
