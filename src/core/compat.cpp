#include "core/compat.h"

#include "core/log.h"

#include <windows.h>
#include <mutex>

namespace mc::core::compat {
namespace {

std::mutex _mutex;
bool _failed[2] = {};

const char* Name(Feature f) {
    switch (f) {
    case Feature::LuaBridge: return "Lua bridge";
    case Feature::NativeMenu: return "menu entries";
    }
    return "?";
}

}  // namespace

void Failed(Feature feature) {
    std::lock_guard lock(_mutex);
    bool& flag = _failed[static_cast<int>(feature)];
    if (flag) return;
    flag = true;
    MC_LOG_ERROR("compat: %s unavailable on this game build%s", Name(feature),
                 feature == Feature::LuaBridge ? "; co-op disabled" : "");
}

bool CoopDisabled() {
    std::lock_guard lock(_mutex);
    return _failed[static_cast<int>(Feature::LuaBridge)];
}

bool AnyFailed() {
    std::lock_guard lock(_mutex);
    return _failed[0] || _failed[1];
}

std::string FailedList() {
    std::lock_guard lock(_mutex);
    std::string s;
    for (Feature f : {Feature::LuaBridge, Feature::NativeMenu}) {
        if (!_failed[static_cast<int>(f)]) continue;
        if (!s.empty()) s += ", ";
        s += Name(f);
    }
    return s;
}

std::string UnsupportedBuildCrc() {
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA("MAFIACOOP_UNSUPPORTED_BUILD", buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

}  // namespace mc::core::compat
