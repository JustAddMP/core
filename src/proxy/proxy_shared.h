#pragma once
// Shared between the proxy dll (src/proxy) and the launcher's --install / --uninstall (src/launcher/install.cpp).
//
// Optional "start from Steam" mode: the launcher copies its proxy dll (GameInfo::proxyBuiltName) into the game folder
// as version.dll (a game that imports VERSION.dll by name loads the copy in the exe's folder instead of System32's,
// because version.dll is not a KnownDLL) plus a pointer file (GameInfo::proxyPointerName) holding the full path of the
// mod dll. The proxy forwards every version.dll export to the real System32 file and, only inside the game exe, loads
// the mod dll. All names come from the game descriptor (mc/game_info.h), compiled into both binaries.
#include "core/names.h"
#include "mc/game_info.h"

#include <string>

namespace mc::proxy {

// Bytes that identify our proxy inside a version.dll found in the game folder (install refuses to overwrite, and
// uninstall refuses to delete, a version.dll without them: it belongs to someone else).
inline const char* Marker() { return GetGameInfo().proxyMarker; }

inline const wchar_t* BuiltName() { return GetGameInfo().proxyBuiltName; }  // name in out\ (never next to an exe there)
inline constexpr wchar_t kInstalledName[] = L"version.dll";                  // name in the game folder
inline const wchar_t* PointerName() { return GetGameInfo().proxyPointerName; }  // next to it: path of the mod dll
inline std::string PointerHeader() { return std::string("# ") + GetGameInfo().modName; }  // first line of our pointer file
inline const wchar_t* GameExe() { return GetGameInfo().gameExe; }

// Environment variables: the launcher sets <envPrefix>LAUNCHER=1 in the game it starts (it injects the dll itself, so
// the proxy must not load a second copy); the proxy sets <envPrefix>PROXY=<its path> before loading the dll.
inline std::wstring EnvLauncher() { return core::EnvNameW("LAUNCHER"); }
inline std::wstring EnvProxy() { return core::EnvNameW("PROXY"); }

} // namespace mc::proxy
