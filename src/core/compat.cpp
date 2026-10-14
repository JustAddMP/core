#include "core/compat.h"

#include "core/log.h"
#include "core/names.h"

#include <windows.h>
#include <mutex>
#include <vector>

namespace mc::core::compat {
namespace {

struct Entry {
    std::string name;
    bool required;
};

std::mutex _mutex;
std::vector<Entry> _failed;

}  // namespace

void Failed(const char* feature, bool required) {
    std::lock_guard lock(_mutex);
    for (const Entry& e : _failed)
        if (e.name == feature) return;
    _failed.push_back({feature, required});
    MC_LOG_ERROR("compat: %s unavailable on this game build%s", feature, required ? "; co-op disabled" : "");
}

bool CoopDisabled() {
    std::lock_guard lock(_mutex);
    for (const Entry& e : _failed)
        if (e.required) return true;
    return false;
}

bool AnyFailed() {
    std::lock_guard lock(_mutex);
    return !_failed.empty();
}

std::string FailedList() {
    std::lock_guard lock(_mutex);
    std::string s;
    for (const Entry& e : _failed) {
        if (!s.empty()) s += ", ";
        s += e.name;
    }
    return s;
}

std::string UnsupportedBuildCrc() {
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA(EnvName("UNSUPPORTED_BUILD").c_str(), buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

}  // namespace mc::core::compat
