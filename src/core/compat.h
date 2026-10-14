#pragma once
// Game-build compatibility, as seen from inside the game: features whose patterns (or hooks) could not be found
// report here, and the launcher tells us when the player started an exe whose CRC is not in core/version.h
// (MAFIACOOP_UNSUPPORTED_BUILD=<crc>). The UI turns this into one clear banner instead of silent failures, and
// Coop refuses to start a session when a feature co-op cannot work without is missing.
// Thread-safe: features report from the init thread, the render thread reads.
#include <string>

namespace mc::core::compat {

enum class Feature {
    LuaBridge,   // script tick + Lua API: required for co-op (player sync runs on it)
    NativeMenu,  // CO-OP entries in the game's menus: optional (launcher arguments still work)
};

void Failed(Feature feature);  // logs once per feature

bool CoopDisabled();                // a feature co-op needs is missing
bool AnyFailed();
std::string FailedList();           // e.g. "Lua bridge, menu entries"; "" if none
std::string UnsupportedBuildCrc();  // from the launcher; "" for a supported build or when the check was skipped

}  // namespace mc::core::compat
