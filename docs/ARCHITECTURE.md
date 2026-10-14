# MafiaCoop native — architecture

A from-scratch, self-contained co-op mod for Mafia: Definitive Edition (Steam build, exe CRC32 3168979183).
All code is our own. Goal: co-op that feels like a built-in game feature.

## Clean-room rule (binding)

Do NOT copy or adapt code from other multiplayer mods. Allowed sources: the game's own binaries and data, the ScriptHook (MPlus) Lua scripts in the
game folder as API reference, public documentation (Microsoft, Steamworks, Lua 5.x manual), and code we write here.
Every game address/struct offset is derived by us from `mafiadefinitiveedition.exe` and recorded in `docs/re-notes.md`
with how it was found.

## Dependencies

- Windows SDK only (Win32, Winsock, D3D11/DXGI, DbgHelp for crash dumps).
- `third_party/minhook` (BSD-2, vendored source, the only third-party code).
- At runtime: the game and Steam. The game memory-loads an embedded old steam_api, so the dll `LoadLibrary`s the
  `steam_api64.dll` from the game folder itself, calls its `SteamAPI_Init` and pumps its `SteamAPI_RunCallbacks`
  (flat API via `GetProcAddress`; no Steamworks SDK headers checked in; see docs/protocol.md "Steam transport").
- Static CRT (`/MT`) so no VC++ redistributable is needed.

## Layout

```
native/
  build.cmd              builds everything with cl.exe (finds VS via vswhere), output in out\
  docs/                  ARCHITECTURE.md (this), re-notes.md (game reverse engineering), protocol.md
  third_party/minhook/
  src/
    launcher/            MafiaCoop.exe: finds the game (Steam registry + libraryfolders.vdf), checks its exe CRC32
                         against core/version.h, starts it
                         suspended with SteamAppId=1030840 in the environment, injects mafiacoop.dll, resumes;
                         install.{h,cpp}: --install/--uninstall of the optional proxy (src/proxy)
    core/                log.{h,cpp}, crash.{h,cpp} (minidump + log on unhandled exception), hook.{h,cpp}
                         (thin MinHook wrapper), config.{h,cpp} (MafiaCoop.ini next to the dll), mem.{h,cpp}
                         (module info, pattern scan with ?? wildcards, RIP-relative resolve), version.h (mod version,
                         supported game builds), compat.{h,cpp} (features missing on this game build -> banner)
    render/              present_hook.{h,cpp}: hooks IDXGISwapChain::Present -> per-frame Tick on the game's
                         render thread; overlay.{h,cpp} + overlay.hlsl (status panel, co-op panels, banners);
                         ui_font.{h,cpp} (glyph atlas rasterised with GDI from Bahnschrift)
    game/                lua.{h,cpp} (bridge into the game's Lua VM, dev console), player_sync.{h,cpp} + player_sync.lua
                         (phase 3: local player sampling, puppets, copy cars; the .lua is embedded at build time),
                         menu.{h,cpp} (phase 5: CO-OP entries in the game's Scaleform main/pause menus) on top of
                         swf_patch.{h,cpp} (SWF/ABC patcher: constant pool append, method prologues, assembler)
    net/                 transport.h (interface), udp_transport.{h,cpp} (own reliability), steam_transport
                         .{h,cpp} (ISteamNetworkingMessages, flat API), steam_lobby.{h,cpp}, session.{h,cpp}
                         (host-authoritative star; host relays), wire.{h,cpp} (byte writer/reader), auth.{h,cpp}
                         (session password: BCrypt HMAC-SHA256 challenge/response)
    dll/                 dllmain.cpp: init thread -> wait for game ready -> install hooks; mod.cpp (per-frame tick);
                         coop.{h,cpp} (sessions, startable from config or at runtime); coop_ui.{h,cpp} (menu commands,
                         pause co-op panel, join-by-IP box, banners); window_place.{h,cpp} (--window left|right); ui_input.{h,cpp} (panel input, blocks the
                         game's DirectInput/XInput/window input while a panel is open)
    proxy/               optional version.dll proxy (MafiaCoop.exe --install puts it in the game folder): forwards the
                         17 exports to System32's version.dll, loads mafiacoop.dll in the game ("start from Steam")
  tests/                 net_loopback.cpp (two sessions over UDP in one process), steam_smoke.cpp
  tools/                 fontgen.cpp, embed_lua.ps1 (build step), devlua.ps1 (run Lua in a running game)
  out/                   build output (gitignored): MafiaCoop.exe, mafiacoop.dll, *.pdb, tests
```

## Runtime model

- `DllMain` only spawns the init thread. The init thread logs, installs the crash handler, waits until the game
  window/D3D device exists, then installs hooks.
- Two threads (confirmed at runtime, re-notes 1.5):
  - **Render thread:** the Present hook calls `Mod::Tick(dt)` once per frame: networking (Session/Steam), the overlay,
    sending the 20 Hz player state, receiving snapshots.
  - **GameMainThread:** the `C_ScriptMachine::Tick` hook drains the Lua bridge's job queue and runs the per-tick
    callbacks (player sync: sample the local player, drive puppets and copy cars). **Every game/Lua call happens
    here.**
  - Hand-over: one mutex per subsystem (bridge job/result queues; player sync snapshots + local sample), held only for
    copies. The game window procedure is subclassed (background_run) on the window's own thread.
- Logs: `<dll dir>\logs\mafiacoop.log`; crash dumps `<dll dir>\logs\crash-<time>.dmp`.

## Networking

- `Transport`: `Host(port)`, `Connect(address)`, `Send(peer, bytes, Reliability)`, `Poll(handler)`,
  `Disconnect(peer)`, `Close()`. Peer ids are `uint64_t` (SteamID64 for Steam, synthetic for UDP).
  Reliability: `Unreliable` (latest-state snapshots) and `Reliable` (ordered).
- Two backends: Steam P2P (default; invites via overlay, NAT traversal, no port forwarding) and direct UDP (fallback,
  and for two game instances on one PC, which share a SteamID and so cannot talk over Steam).
- Session: the host is the authority (no separate server process). Guests talk only to the host; the host relays.
  Message framing and ids in `docs/protocol.md`. Version handshake first; mismatched versions are rejected with a
  readable reason.

## Phases and gates

1. Skeleton — injected dll logs, Present hook ticks, crash handler writes a dump. Gate: game runs normally for 10 min.
2. Connection — Steam lobby + invite + direct-IP fallback, handshake, ping display. Gate: two instances/PCs connect.
3. Second player — remote player ped spawned and synced (position, heading, anim state, weapon, vehicle seat).
4. Shared world — host's NPCs/cars/weather/time/mission state mirrored to guests.
5. Native UI — co-op entries in the game's menus (main menu CO-OP page, pause menu CO-OP), invites, notifications
   (done: entries patched into the game's own AS3 menus; the pause panel, IP box and banners are our overlay in the
   menu style because the game's HUD banners show nothing from script, docs/game-api.md 9).
6. Melee/damage sync, guest-side objectives, NPC appearance.
