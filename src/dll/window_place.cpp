#include "dll/window_place.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>
#include <cctype>
#include <string>

namespace mc::window_place {
namespace {

constexpr float kCheckEvery = 0.5f;  // s

enum class Side { None, Left, Right };

HWND _hwnd = nullptr;
Side _side = Side::None;
float _now = 0, _nextCheck = 0;
int _applies = 0;
int _offChecks = 0;  // consecutive checks that found the window off target

bool Target(RECT* out) {
    HMONITOR mon = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const RECT& w = mi.rcWork;
    const LONG mid = w.left + (w.right - w.left) / 2;
    *out = w;
    if (_side == Side::Left) out->right = mid;
    else out->left = mid;
    return true;
}

void Check() {
    if (!_hwnd || !IsWindow(_hwnd) || IsIconic(_hwnd)) return;
    RECT want{}, have{};
    if (!Target(&want) || !GetWindowRect(_hwnd, &have)) return;
    if (EqualRect(&want, &have)) {
        _offChecks = 0;
        return;
    }
    // The first time at once; later only when it stays off for a second (not in the middle of a drag).
    if (_applies > 0 && ++_offChecks < 2) return;
    _offChecks = 0;
    ++_applies;
    if (IsZoomed(_hwnd)) ShowWindowAsync(_hwnd, SW_RESTORE);
    // Async: the window belongs to the game's window thread; a synchronous SetWindowPos from the render thread could
    // wait on a thread that waits on us.
    SetWindowPos(_hwnd, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    MC_LOG_INFO("window: placed on the %s half: %ld,%ld %ldx%ld (was %ld,%ld %ldx%ld, #%d)",
                _side == Side::Left ? "left" : "right", want.left, want.top, want.right - want.left,
                want.bottom - want.top, have.left, have.top, have.right - have.left, have.bottom - have.top, _applies);
}

}  // namespace

void Init(void* hwnd) {
    std::string v = core::Config::Get().GetString("window");
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "left") _side = Side::Left;
    else if (v == "right") _side = Side::Right;
    else if (!v.empty()) MC_LOG_WARN("window: unknown value '%s' (left|right); window left alone", v.c_str());
    _hwnd = static_cast<HWND>(hwnd);
    if (_side != Side::None) MC_LOG_INFO("window: keeping the game window on the %s half of its monitor", v.c_str());
}

void Tick(float dt) {
    if (_side == Side::None) return;
    _now += dt;
    if (_now < _nextCheck) return;
    _nextCheck = _now + kCheckEvery;
    Check();
}

void OnResize() {
    if (_side == Side::None) return;
    _nextCheck = _now + 0.2f;
}

}  // namespace mc::window_place
