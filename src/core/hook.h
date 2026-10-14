#pragma once
// Thin MinHook wrapper. Every call logs its outcome; hooks are tracked by name.
#include <string>

namespace mc::core::hook {

bool Init();      // MH_Initialize
void Shutdown();  // disables + removes all hooks, MH_Uninitialize

// Creates (not enables) a hook on target; *original receives the trampoline.
bool Create(const char* name, void* target, void* detour, void** original);
bool Enable(const char* name);
bool Disable(const char* name);
bool Remove(const char* name);

// Create + Enable.
bool Install(const char* name, void* target, void* detour, void** original);

template <typename T>
bool Install(const char* name, void* target, T* detour, T** original) {
    return Install(name, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

} // namespace mc::core::hook
