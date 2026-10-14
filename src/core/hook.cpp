#include "core/hook.h"

#include "core/log.h"

#include <MinHook.h>
#include <windows.h>
#include <map>
#include <mutex>

namespace mc::core::hook {
namespace {

std::mutex _mutex;
std::map<std::string, void*> _hooks;  // name -> target
bool _initialized = false;

void* TargetOf(const char* name) {
    auto it = _hooks.find(name);
    return it == _hooks.end() ? nullptr : it->second;
}

const char* ModuleOf(void* addr, char* buf, size_t cap) {
    HMODULE mod = nullptr;
    buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod)) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = path;
        for (DWORD i = 0; i < n; ++i) if (path[i] == '\\') base = path + i + 1;
        _snprintf_s(buf, cap, _TRUNCATE, "%s+0x%llx", base,
                    static_cast<unsigned long long>(static_cast<char*>(addr) - reinterpret_cast<char*>(mod)));
    }
    return buf;
}

} // namespace

bool Init() {
    std::lock_guard lock(_mutex);
    if (_initialized) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        MC_LOG_ERROR("hook: MH_Initialize failed: %s", MH_StatusToString(s));
        return false;
    }
    _initialized = true;
    MC_LOG_INFO("hook: MinHook initialized");
    return true;
}

void Shutdown() {
    std::lock_guard lock(_mutex);
    if (!_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    for (auto& [name, target] : _hooks) MH_RemoveHook(target);
    _hooks.clear();
    MH_Uninitialize();
    _initialized = false;
    MC_LOG_INFO("hook: all hooks removed");
}

bool Create(const char* name, void* target, void* detour, void** original) {
    std::lock_guard lock(_mutex);
    char where[MAX_PATH + 32];
    ModuleOf(target, where, sizeof(where));
    if (_hooks.count(name)) {
        MC_LOG_ERROR("hook: '%s' already exists", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        MC_LOG_ERROR("hook: create '%s' at %p (%s) failed: %s", name, target, where, MH_StatusToString(s));
        return false;
    }
    _hooks[name] = target;
    MC_LOG_INFO("hook: created '%s' at %p (%s)", name, target, where);
    return true;
}

bool Enable(const char* name) {
    std::lock_guard lock(_mutex);
    void* target = TargetOf(name);
    MH_STATUS s = target ? MH_EnableHook(target) : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        MC_LOG_ERROR("hook: enable '%s' failed: %s", name, MH_StatusToString(s));
        return false;
    }
    MC_LOG_INFO("hook: enabled '%s'", name);
    return true;
}

bool Disable(const char* name) {
    std::lock_guard lock(_mutex);
    void* target = TargetOf(name);
    MH_STATUS s = target ? MH_DisableHook(target) : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        MC_LOG_ERROR("hook: disable '%s' failed: %s", name, MH_StatusToString(s));
        return false;
    }
    MC_LOG_INFO("hook: disabled '%s'", name);
    return true;
}

bool Remove(const char* name) {
    std::lock_guard lock(_mutex);
    void* target = TargetOf(name);
    MH_STATUS s = target ? MH_RemoveHook(target) : MH_ERROR_NOT_CREATED;
    if (s != MH_OK) {
        MC_LOG_ERROR("hook: remove '%s' failed: %s", name, MH_StatusToString(s));
        return false;
    }
    _hooks.erase(name);
    MC_LOG_INFO("hook: removed '%s'", name);
    return true;
}

bool Install(const char* name, void* target, void* detour, void** original) {
    if (!Create(name, target, detour, original)) return false;
    if (!Enable(name)) {
        Remove(name);
        return false;
    }
    return true;
}

} // namespace mc::core::hook
