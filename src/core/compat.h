#pragma once
// Game-build compatibility, as seen from inside the game: game features whose patterns (or hooks) could not be found
// report here, and the launcher tells us when the player started an exe whose CRC is not in the game's supported
// builds (<envPrefix>UNSUPPORTED_BUILD=<crc>). The UI turns this into one clear banner instead of silent failures, and
// Coop refuses to start a session when a feature co-op cannot work without is missing.
// Thread-safe: features report from the init thread, the render thread reads.
#include <string>

namespace mc::core::compat {

// feature: short name for the banner ("Lua bridge", "menu entries"). required: co-op cannot work without it.
// Logs once per feature.
void Failed(const char* feature, bool required);

bool CoopDisabled();                // a required feature is missing
bool AnyFailed();
std::string FailedList();           // e.g. "Lua bridge, menu entries" (in report order); "" if none
std::string UnsupportedBuildCrc();  // from the launcher; "" for a supported build or when the check was skipped

}  // namespace mc::core::compat
