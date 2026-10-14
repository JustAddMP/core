// Mod dll entry. DllMain only signals the launcher and spawns the init thread; all setup happens there. The game
// layer (mc/game_module.h) is initialised from the init thread.
#include "mc/core_version.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/names.h"
#include "dll/mod.h"
#include "dll/ui_input.h"
#include "dll/window_place.h"
#include "mc/game_module.h"
#include "render/present_hook.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace mc {
namespace {

HMODULE _module = nullptr;
WNDPROC _gameWndProc = nullptr;
bool _backgroundRun = true;

// Games commonly pause their simulation when they receive WM_ACTIVATEAPP(FALSE) (Mafia DE: its window procedure turns
// it into an engine "app inactive" event). In co-op a paused game stops sending its player and stops moving the
// puppets, and two instances on one PC could never both run. With background_run = 1 (default) the deactivation is
// swallowed so the world keeps running while unfocused. Nothing else changes: input still only goes to the focused
// window.
// Also the entry point for our UI's keyboard/mouse input: while a co-op panel is open, ui_input swallows the
// keyboard and mouse messages so the game's menus underneath do not react.
LRESULT CALLBACK ModWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (_backgroundRun && msg == WM_ACTIVATEAPP && wp == FALSE) return 0;
    if (ui_input::OnWindowMessage(msg, static_cast<uint64_t>(wp), static_cast<int64_t>(lp))) return 0;
    return CallWindowProcW(_gameWndProc, hwnd, msg, wp, lp);
}

void SubclassWindow(HWND hwnd, bool backgroundRun) {
    _backgroundRun = backgroundRun;
    ui_input::SetWindow(hwnd);
    // Messages can arrive on the window's thread between SetWindowLongPtr and the assignment below; ModWndProc must
    // already have somewhere to forward them.
    _gameWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    SetLastError(0);
    auto prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ModWndProc)));
    if (!prev && GetLastError() != 0) {
        MC_LOG_WARN("init: SetWindowLongPtr failed (%lu): no background_run, co-op panels cannot block keys",
                    GetLastError());
        return;
    }
    _gameWndProc = prev;
    if (backgroundRun) MC_LOG_INFO("init: background_run: window deactivation is ignored (game keeps running unfocused)");
}

std::wstring ModuleDir(HMODULE mod) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct WindowSearch {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp) {
    auto* search = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right - rc.left < 320 || rc.bottom - rc.top < 200) return TRUE;  // skip splash/tool windows
    search->found = hwnd;
    return FALSE;
}

HWND WaitForGameWindow(DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    WindowSearch search{GetCurrentProcessId(), nullptr};
    while (GetTickCount64() - start < timeoutMs) {
        EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&search));
        if (search.found) return search.found;
        Sleep(250);
    }
    return nullptr;
}

void LogEnvironment(const std::wstring& dir) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto& main = core::mem::MainModule();
    MC_LOG_INFO("%s %s (core %s), built " __DATE__ " " __TIME__, GetGameInfo().modName, GetGameInfo().modVersion,
                kCoreVersion);
    MC_LOG_INFO("pid %lu, exe %s", GetCurrentProcessId(), Narrow(exe).c_str());
    MC_LOG_INFO("main module base 0x%llx size 0x%zx timestamp 0x%08x, .text 0x%llx+0x%zx",
                static_cast<unsigned long long>(main.base), main.size, main.timeDateStamp,
                static_cast<unsigned long long>(main.text.start), main.text.size);
    MC_LOG_INFO("dll dir %s", Narrow(dir).c_str());
    char appId[32] = {};
    GetEnvironmentVariableA("SteamAppId", appId, sizeof(appId));
    MC_LOG_INFO("env SteamAppId=%s", appId[0] ? appId : "(unset)");
    // How the dll got here: the launcher (injection) or the optional version.dll proxy in the game folder (game
    // started from Steam; settings then come only from the ini next to this dll, src/proxy/proxy_shared.h).
    const GameInfo& gi = GetGameInfo();
    wchar_t proxy[MAX_PATH] = {};
    if (GetEnvironmentVariableW(core::EnvNameW("LAUNCHER").c_str(), nullptr, 0) > 0)
        MC_LOG_INFO("loaded by %s", Narrow(gi.launcherName).c_str());
    else if (GetEnvironmentVariableW(core::EnvNameW("PROXY").c_str(), proxy, MAX_PATH) > 0)
        MC_LOG_INFO("loaded by the version.dll proxy %s (game started without %s)", Narrow(proxy).c_str(),
                    Narrow(gi.launcherName).c_str());
    else
        MC_LOG_INFO("loaded by an unknown injector (no %sLAUNCHER / %sPROXY)", gi.envPrefix, gi.envPrefix);
}

// One log per game instance: --log <tag> -> <logPrefix>-<tag>.log. Without it the first instance writes
// <logPrefix>.log and any further concurrent instance <logPrefix>-<pid>.log (detected with a named mutex that the
// first instance keeps for its lifetime), so two games on one PC never rotate or interleave each other's log.
std::wstring LogFileName() {
    const std::wstring prefix = GetGameInfo().logPrefix;
    wchar_t tag[64] = {};
    DWORD n = GetEnvironmentVariableW(core::EnvNameW("LOG").c_str(), tag, 64);
    if (n > 0 && n < 64) return prefix + L"-" + tag + L".log";
    HANDLE m = CreateMutexW(nullptr, FALSE, core::KernelName(L"PrimaryLog").c_str());  // intentionally never closed
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return prefix + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    }
    return prefix + L".log";
}

DWORD WINAPI InitThread(LPVOID) {
    const std::wstring dir = ModuleDir(_module);
    core::log::Init(dir + L"\\logs", LogFileName().c_str());
    LogEnvironment(dir);

    auto& cfg = core::Config::Get();
    cfg.SetEnvPrefix(GetGameInfo().envPrefix);
    if (cfg.Load(dir + L"\\" + GetGameInfo().iniName)) {
        MC_LOG_INFO("config: loaded %s (%zu keys)", Narrow(cfg.Path()).c_str(), cfg.Values().size());
    } else {
        MC_LOG_INFO("config: %s not found, using defaults", Narrow(cfg.Path()).c_str());
    }
    for (const char* key : {"host", "join", "join_lobby", "transport", "name", "port", "log", "steam", "window"}) {
        if (cfg.Has(key)) MC_LOG_INFO("config: %s = %s", key, cfg.GetString(key).c_str());
    }

    core::crash::Install(dir + L"\\logs");
    if (!core::hook::Init()) return 1;
    // The game layer's hooks (script bridge, menus, ...): independent of the window/D3D. Failures only disable
    // features (reported to core::compat).
    GetGameModule().Init(dir, cfg.GetString("log"));

    const DWORD timeoutMs = static_cast<DWORD>(cfg.GetInt("window_timeout_s", 300)) * 1000;
    MC_LOG_INFO("init: waiting for the game window");
    HWND hwnd = WaitForGameWindow(timeoutMs);
    if (!hwnd) {
        MC_LOG_ERROR("init: game window did not appear within %lu s; not hooking", timeoutMs / 1000);
        return 1;
    }
    char title[128] = {}, cls[128] = {};
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    MC_LOG_INFO("init: game window %p class '%s' title '%s'", hwnd, cls, title);

    SubclassWindow(hwnd, cfg.GetBool("background_run", true));
    window_place::Init(hwnd);
    ui_input::Install();
    Mod::Init();
    if (!render::InstallPresentHook(&Mod::Tick, &Mod::OnResize, &Mod::OnBeforeResize)) {
        MC_LOG_ERROR("init: Present hook failed; mod inactive");
        return 1;
    }
    MC_LOG_INFO("init: done");
    return 0;
}

} // namespace
} // namespace mc

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        mc::_module = module;
        DisableThreadLibraryCalls(module);
        // Tell the launcher the dll is in (event created by the launcher; absent when injected otherwise).
        const std::wstring name = mc::core::KernelName(L"Injected-" + std::to_wstring(GetCurrentProcessId()));
        if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
            SetEvent(ev);
            CloseHandle(ev);
        }
        if (HANDLE t = CreateThread(nullptr, 0, mc::InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
