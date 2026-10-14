// version.dll proxy for the optional "start from Steam" mode (see proxy_shared.h).
//
// In every process: loads the real version.dll from the system directory and points the export stubs
// (version_stubs.asm) at it, so anything that imports version.dll keeps working.
// Only in mafiadefinitiveedition.exe, and only when MafiaCoop.exe did not start the game (it injects the dll itself):
// queues an APC on the loading thread that loads mafiacoop.dll from the path in mafiacoop_path.txt next to this file
// (or mafiacoop.dll next to this file). The game imports version.dll statically, so DllMain runs on the main thread
// during process initialization and the APC is delivered when the loader finishes, before the game's entry point:
// the same moment as the launcher's QueueUserAPC injection. Nothing is written anywhere; problems go to
// OutputDebugString (visible in DebugView) and the game then simply runs without co-op.
#include "proxy/proxy_shared.h"

#include <windows.h>

#include <string>

extern "C" void* g_real[];  // defined below, read by the asm stubs

namespace {

DWORD WINAPI NotFound() {  // stands in for an export the real dll lacks: fail like a missing function would
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

constexpr const char* kExports[] = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",     "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",  "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",     "VerFindFileA",               "VerFindFileW",              "VerInstallFileA",
    "VerInstallFileW",         "VerLanguageNameA",           "VerLanguageNameW",          "VerQueryValueA",
    "VerQueryValueW",
};
constexpr size_t kExportCount = sizeof(kExports) / sizeof(kExports[0]);

HMODULE _self = nullptr;
HMODULE _real = nullptr;
std::wstring _selfDir;

void Debug(const std::wstring& msg) {
    OutputDebugStringW((L"[MafiaCoop proxy] " + msg + L"\n").c_str());
}

std::wstring ModulePath(HMODULE mod) {
    std::wstring s(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(mod, s.data(), static_cast<DWORD>(s.size()));
        if (n == 0) return {};
        if (n < s.size()) {
            s.resize(n);
            return s;
        }
        s.resize(s.size() * 2);
    }
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void LoadReal() {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Debug(L"GetSystemDirectory failed");
        return;
    }
    // A full path: the loader compares full names, so this is System32's file and not this proxy again.
    const std::wstring path = std::wstring(sys) + L"\\version.dll";
    _real = LoadLibraryExW(path.c_str(), nullptr, 0);
    if (!_real) {
        Debug(L"cannot load " + path + L" (error " + std::to_wstring(GetLastError()) + L")");
        return;
    }
    for (size_t i = 0; i < kExportCount; ++i) {
        if (FARPROC p = GetProcAddress(_real, kExports[i])) g_real[i] = reinterpret_cast<void*>(p);
    }
}

std::wstring Trim(const std::wstring& s) {
    const size_t b = s.find_first_not_of(L" \t\r\n﻿");
    if (b == std::wstring::npos) return {};
    return s.substr(b, s.find_last_not_of(L" \t\r\n") - b + 1);
}

// The first line of mafiacoop_path.txt that is neither empty nor a '#' comment (UTF-8).
std::wstring ReadPointer(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[8192];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return {};
    const int wn = MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(got), nullptr, 0);
    std::wstring text(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(got), text.data(), wn);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        const std::wstring line = Trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == L'#') continue;
        return (line.size() >= 2 && line.front() == L'"' && line.back() == L'"') ? line.substr(1, line.size() - 2)
                                                                                  : line;
    }
    return {};
}

void CALLBACK LoadModApc(ULONG_PTR) {
    if (GetModuleHandleW(L"mafiacoop.dll")) {  // already in (e.g. an older launcher injected it first)
        Debug(L"mafiacoop.dll is already loaded; not loading it again");
        return;
    }
    std::wstring dll = ReadPointer(_selfDir + L"\\" + mc::proxy::kPointerName);
    if (dll.empty()) dll = _selfDir + L"\\mafiacoop.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Debug(L"mafiacoop.dll not found (" + dll + L"); the game runs without co-op. Run MafiaCoop.exe --install "
              L"again if the MafiaCoop folder was moved.");
        return;
    }
    SetEnvironmentVariableW(mc::proxy::kEnvProxy, ModulePath(_self).c_str());  // the dll logs how it was loaded
    if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        Debug(L"LoadLibrary(" + dll + L") failed, error " + std::to_wstring(GetLastError()));
}

bool IsGame() {
    return _wcsicmp(FileName(ModulePath(nullptr)).c_str(), mc::proxy::kGameExe) == 0;
}

bool StartedByLauncher() {
    return GetEnvironmentVariableW(mc::proxy::kEnvLauncher, nullptr, 0) > 0;
}

} // namespace

// Read by the asm stubs. Every slot starts at NotFound so a stub is never a jump to null.
extern "C" void* g_real[kExportCount] = {
    &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound,
    &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound, &NotFound,
};

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        _self = module;
        DisableThreadLibraryCalls(module);
        const std::wstring self = ModulePath(module);
        _selfDir = self.substr(0, self.find_last_of(L"\\/"));
        OutputDebugStringA(mc::proxy::kMarker);  // also keeps the marker bytes the launcher looks for in the file
        Debug(L"loaded: " + self);
        LoadReal();
        if (IsGame() && !StartedByLauncher()) {
            if (!QueueUserAPC(&LoadModApc, GetCurrentThread(), 0))
                Debug(L"QueueUserAPC failed, error " + std::to_wstring(GetLastError()));
        }
    }
    return TRUE;  // the real version.dll stays loaded until the process ends
}
