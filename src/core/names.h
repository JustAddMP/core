#pragma once
// Names derived from the game descriptor (mc/game_info.h): environment variables and kernel object names. Header-only
// so the launcher and the proxy can use it without linking anything else.
#include "mc/game_info.h"

#include <string>

namespace mc::core {

// ASCII -> UTF-16 (descriptor strings are ASCII).
inline std::wstring AsciiW(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
    return w;
}

// <envPrefix><key>, e.g. EnvName("LAUNCHER") -> "MAFIACOOP_LAUNCHER".
inline std::string EnvName(const char* key) { return std::string(GetGameInfo().envPrefix) + key; }
inline std::wstring EnvNameW(const char* key) { return AsciiW(EnvName(key).c_str()); }

// Local\<modName><suffix>, e.g. KernelName("Injected-1234") -> "Local\MafiaCoopInjected-1234".
inline std::wstring KernelName(const std::wstring& suffix) {
    return L"Local\\" + AsciiW(GetGameInfo().modName) + suffix;
}

}  // namespace mc::core
