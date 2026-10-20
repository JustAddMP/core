#pragma once
// The game descriptor: everything core needs to know about one game and the mod built for it. The game repo defines
// GetGameInfo() in a .cpp of its own that depends on nothing else: it is linked into the launcher, the version.dll
// proxy and the mod dll. See game_module.h for the dll-side interface.
#include <cstddef>
#include <cstdint>

namespace mc {

// One game exe build the mod was made for, identified by the CRC32 of the whole exe file.
struct GameBuild {
    uint32_t crc32;
    const char* label;  // e.g. "Steam build 16502642"
};

struct GameInfo {
    // ---- the mod ----
    const char* modName;           // "MafiaCoop": message box titles, banners, logs, kernel object names
    const char* modVersion;        // "0.2.0": shown to players; the session handshake build number is derived from it
    const wchar_t* launcherName;   // "MafiaCoop.exe": file name of the launcher the build produces
    const wchar_t* dllName;        // "mafiacoop.dll": the mod dll next to the launcher
    const wchar_t* iniName;        // "MafiaCoop.ini": settings next to the launcher/dll
    const wchar_t* logPrefix;      // "mafiacoop": logs\<prefix>.log, logs\<prefix>-<tag>.log
    const char* envPrefix;         // "MAFIACOOP_": launcher --key value -> <prefix>KEY=value for the dll
    uint16_t defaultPort;          // UDP port for hosting and for "join <ip>" without a port
    const char* steamStatus;       // Steam rich presence "status" while in a co-op lobby

    // ---- the game ----
    const char* gameName;          // "Mafia: Definitive Edition" (messages)
    const wchar_t* gameExe;        // "mafiadefinitiveedition.exe"
    uint32_t steamAppId;           // 1030840
    const wchar_t* steamInstallDir;  // default folder under steamapps\common when the app manifest has none
    const wchar_t* gameArgs;       // appended to the game's command line by the launcher ("" = none)
    const GameBuild* builds;       // supported exe builds (launcher CRC check)
    size_t buildCount;

    // ---- optional "start from Steam" proxy (version.dll in the game folder) ----
    const wchar_t* proxyBuiltName;   // "mafiacoop_proxy.dll": file name in the build output
    const wchar_t* proxyPointerName; // "mafiacoop_path.txt": next to the installed proxy, holds the dll's path
    const char* proxyMarker;         // bytes that identify our proxy file (install/uninstall never touch others)

    // ---- update check (core 0.13; core/update_check.h) ----
    // The mod's id at https://justaddmp.com/api/version/<id>, e.g. "mafia-de". Once per game start the dll asks it
    // for the newest release and shows a notice at the main menu when it is newer than modVersion. nullptr: no check.
    const char* updateId = nullptr;
};

const GameInfo& GetGameInfo();  // defined by the game repo

}  // namespace mc
