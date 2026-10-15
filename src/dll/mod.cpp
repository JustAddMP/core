#include "dll/mod.h"

#include "core/config.h"
#include "core/crash.h"
#include "core/hook.h"
#include "core/log.h"
#include "dll/coop.h"
#include "dll/coop_ui.h"
#include "dll/window_place.h"
#include "mc/game_module.h"
#include "render/overlay.h"
#include "render/present_hook.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace mc {
namespace {

constexpr double kHeartbeatSeconds = 10.0;

struct State {
    bool crashTest = false;
    double crashTestDelay = 20.0;
    double uptime = 0.0;          // seconds since first Tick
    double sinceHeartbeat = 0.0;
    unsigned long long frames = 0;
    unsigned framesSinceHeartbeat = 0;
    bool overlayVisible = false;  // the debug status text (top-left); banners and panels are drawn regardless
    bool toggleKeyDown = false;
    std::vector<render::OverlayLine> status;
    float minDt = 1e9f;
    float maxDt = 0.0f;
};

State _state;

// Held for every frame's Tick. The process-exit hook takes it so it never shuts the session down while a frame is
// using it, and no frame runs after the shutdown (_exiting).
std::recursive_timed_mutex _tickMutex;
std::atomic<bool> _exiting{false};

using ExitProcessFn = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
ExitProcessFn _origExitProcess = nullptr;
TerminateProcessFn _origTerminateProcess = nullptr;

// The game is ending (its quit menu, the window's close button, a crash handler's exit, ...): leave the co-op
// session cleanly so the others are told at once instead of after the 10 s timeout, and release our D3D objects.
// A kill from outside (Task Manager, taskkill /F) cannot be caught; the others then time out.
void OnProcessExit(const char* via) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    MC_LOG_INFO("mod: process exit (%s): leaving co-op cleanly", via);
    std::unique_lock<std::recursive_timed_mutex> lock(_tickMutex, std::defer_lock);
    const bool locked = lock.try_lock_for(std::chrono::milliseconds(500));
    _exiting = true;
    Coop::Shutdown();
    if (locked) render::Overlay::ReleaseAll();  // only without a frame drawing at the same time
    MC_LOG_INFO("mod: exit handling done%s", locked ? "" : " (render thread busy: overlay left to the OS)");
}

void WINAPI ExitProcessDetour(UINT code) {
    OnProcessExit("ExitProcess");
    _origExitProcess(code);
}

BOOL WINAPI TerminateProcessDetour(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        OnProcessExit("TerminateProcess");
    return _origTerminateProcess(process, code);
}

void InstallExitHooks() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void* exitProcess = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess")) : nullptr;
    void* terminateProcess = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "TerminateProcess")) : nullptr;
    const bool a = exitProcess && core::hook::Install("ExitProcess", exitProcess, reinterpret_cast<void*>(&ExitProcessDetour),
                                                      reinterpret_cast<void**>(&_origExitProcess));
    const bool b = terminateProcess &&
                   core::hook::Install("TerminateProcess", terminateProcess,
                                       reinterpret_cast<void*>(&TerminateProcessDetour),
                                       reinterpret_cast<void**>(&_origTerminateProcess));
    if (!a || !b) MC_LOG_WARN("mod: exit hooks incomplete (ExitProcess %d, TerminateProcess %d)", a, b);
}

// F7 toggles the debug status text; only while one of our windows is in the foreground.
void PollToggleKey() {
    const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool wasDown = _state.toggleKeyDown;
    _state.toggleKeyDown = down;
    if (!down || wasDown) return;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    _state.overlayVisible = !_state.overlayVisible;
    MC_LOG_INFO("mod: debug status text %s (F7)", _state.overlayVisible ? "shown" : "hidden");
}

} // namespace

void Mod::Init() {
    const auto& cfg = core::Config::Get();
    _state.crashTest = cfg.GetBool("crash_test", false);
    _state.crashTestDelay = cfg.GetInt("crash_test_delay", 20);
    if (_state.crashTest) {
        MC_LOG_WARN("mod: crash_test=1 -> will raise an access violation in Tick after %.0f s", _state.crashTestDelay);
    }
    // Hidden by default for players; debug_overlay = 1 / --debug-overlay shows it from the start ("overlay" is the
    // older name of the same key).
    _state.overlayVisible = cfg.GetBool("debug_overlay", cfg.GetBool("overlay", false));
    MC_LOG_INFO("mod: debug status text %s at start (F7 toggles; debug_overlay = 1 shows it)",
                _state.overlayVisible ? "shown" : "hidden");
    Coop::Init();
    CoopUi::Init();
    InstallExitHooks();
    MC_LOG_INFO("mod: initialized");
}

void Mod::Tick(float dt) {
    std::lock_guard<std::recursive_timed_mutex> lock(_tickMutex);
    if (_exiting) return;
    if (_state.frames == 0) MC_LOG_INFO("mod: first Tick");
    ++_state.frames;
    ++_state.framesSinceHeartbeat;
    _state.uptime += dt;
    window_place::Tick(dt);
    _state.sinceHeartbeat += dt;
    if (_state.frames > 1) {
        if (dt < _state.minDt) _state.minDt = dt;
        if (dt > _state.maxDt) _state.maxDt = dt;
    }

    // The game installs its own unhandled-exception filter a few seconds after start; put ours back every frame at
    // first so an early crash still gets our log + dump, then with every heartbeat.
    if (_state.uptime < 60.0) core::crash::Reassert();

    if (_state.sinceHeartbeat >= kHeartbeatSeconds) {
        double fps = _state.framesSinceHeartbeat / _state.sinceHeartbeat;
        MC_LOG_INFO("heartbeat: uptime %.0f s, frames %llu, fps %.1f, frame time min %.1f ms max %.1f ms",
                    _state.uptime, _state.frames, fps, _state.minDt * 1000.0f, _state.maxDt * 1000.0f);
        _state.sinceHeartbeat = 0.0;
        _state.framesSinceHeartbeat = 0;
        _state.minDt = 1e9f;
        _state.maxDt = 0.0f;
        core::crash::Reassert();
        std::vector<render::OverlayLine> lines;
        Coop::GetStatus(lines);
        for (const auto& l : lines) MC_LOG_INFO("status: %s", l.text.c_str());
    }

    GetGameModule().RenderTick(dt);
    Coop::Tick(dt);
    render::Overlay::Track(render::GameSwapChain());
    CoopUi::Tick(dt);

    PollToggleKey();
    _state.status.clear();
    if (_state.overlayVisible) Coop::GetStatus(_state.status);
    render::Overlay::Draw(render::GameSwapChain(), _state.status, CoopUi::Draw());

    if (_state.crashTest && _state.uptime >= _state.crashTestDelay) {
        _state.crashTest = false;
        MC_LOG_WARN("mod: crash_test: raising access violation now");
        volatile int* bad = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10));
        *bad = 0xDEAD;
    }
}

void Mod::OnBeforeResize() { render::Overlay::ReleaseTargets(); }

void Mod::OnDeviceLost() { render::Overlay::ReleaseAll(); }

bool Mod::DebugOverlay() { return _state.overlayVisible; }

void Mod::SetDebugOverlay(bool on, bool save) {
    _state.overlayVisible = on;
    MC_LOG_INFO("mod: debug status text %s", on ? "shown" : "hidden");
    if (save && !core::Config::Get().Set("debug_overlay", on ? "1" : "0"))
        MC_LOG_WARN("mod: could not save debug_overlay");
}

void Mod::OnResize(unsigned width, unsigned height) {
    window_place::OnResize();
    MC_LOG_INFO("mod: resized to %ux%u", width, height);
}

} // namespace mc
