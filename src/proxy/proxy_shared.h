#pragma once
// Shared between the proxy dll (src/proxy) and the launcher's --install / --uninstall (src/launcher/install.cpp).
//
// Optional "start from Steam" mode: the launcher copies out\mafiacoop_proxy.dll into the game folder as version.dll
// (the game imports VERSION.dll by name and it is not a KnownDLL, so the copy in the exe's folder is loaded instead
// of System32's) plus a pointer file holding the full path of mafiacoop.dll. The proxy forwards every version.dll
// export to the real System32 file and, only inside mafiadefinitiveedition.exe, loads mafiacoop.dll.

namespace mc::proxy {

// Bytes that identify our proxy inside a version.dll found in the game folder (install refuses to overwrite, and
// uninstall refuses to delete, a version.dll without them: it belongs to someone else).
inline constexpr char kMarker[] = "MafiaCoop-proxy-dll:v1 (version.dll forwarder that loads mafiacoop.dll)";

inline constexpr wchar_t kBuiltName[] = L"mafiacoop_proxy.dll";  // name in out\ (never next to an exe there)
inline constexpr wchar_t kInstalledName[] = L"version.dll";       // name in the game folder
inline constexpr wchar_t kPointerName[] = L"mafiacoop_path.txt";  // next to it: path of mafiacoop.dll
inline constexpr char kPointerHeader[] = "# MafiaCoop";           // first line of our pointer file starts with this
inline constexpr wchar_t kGameExe[] = L"mafiadefinitiveedition.exe";

// Environment variables: the launcher sets MAFIACOOP_LAUNCHER=1 in the game it starts (it injects the dll itself,
// so the proxy must not load a second copy); the proxy sets MAFIACOOP_PROXY=<its path> before loading the dll.
inline constexpr wchar_t kEnvLauncher[] = L"MAFIACOOP_LAUNCHER";
inline constexpr wchar_t kEnvProxy[] = L"MAFIACOOP_PROXY";

} // namespace mc::proxy
