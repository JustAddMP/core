#include "dll/ui_input.h"

#include "core/hook.h"
#include "core/log.h"
#include "render/overlay.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <vector>
#include <mutex>

namespace mc::ui_input {
namespace {

// Own GUID constants (dinput.h's are in dxguid.lib, which we do not link).
constexpr GUID kIID_DirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kIID_DirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

constexpr int kGetDeviceStateIndex = 9;
constexpr int kGetDeviceDataIndex = 10;

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, void* data);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD objSize, DIDEVICEOBJECTDATA* data,
                                                    DWORD* inOut, DWORD flags);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);
using XInputGetCapsFn = DWORD(WINAPI*)(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps);

GetDeviceStateFn _origStateA = nullptr, _origStateW = nullptr;
void* _targetStateW = nullptr;  // hooked addresses of the W flavour (the A vtable may point to the same code)
void* _targetDataW = nullptr;
GetDeviceDataFn _origDataA = nullptr, _origDataW = nullptr;
XInputGetStateFn _origXInput = nullptr;
XInputGetCapsFn _origXInputCaps = nullptr;

// Test pad (SetTestPad): a virtual gamepad on XInput user 0 for automated tests. Off unless enabled.
std::atomic<bool> _testPadEnabled{false};
std::mutex _testPadMutex;
XINPUT_GAMEPAD _testPad{};
ULONGLONG _testPadUntil = 0;  // GetTickCount64 deadline of the injected state (0 = released)
DWORD _testPadPacket = 0;

std::atomic<bool> _capture{false};
std::atomic<bool> _guard{false};  // capture ended, keys still down
std::atomic<ULONGLONG> _guardUntil{0};  // keep blocking a little after the keys came up (buffered DirectInput)
constexpr ULONGLONG kGuardTailMs = 250;
std::atomic<HWND> _hwnd{nullptr};
std::atomic<uint16_t> _pad{0};

struct RawEvent {
    Event e;
    float clientW, clientH;
};
std::mutex _mutex;
std::deque<RawEvent> _events;

bool AnyHeld() {
    for (int vk : {VK_ESCAPE, VK_RETURN, VK_SPACE, VK_BACK, VK_LBUTTON, VK_UP, VK_DOWN})
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return (_pad.load() & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) != 0;
}

bool BlockNow() {
    if (_capture.load()) return true;
    if (_guard.load()) {
        const ULONGLONG now = GetTickCount64();
        if (AnyHeld()) {
            _guardUntil = now + kGuardTailMs;
            return true;
        }
        if (now < _guardUntil.load()) return true;
        _guard = false;
    }
    return false;
}

// Device type (DI8DEVTYPE_*) per device object, from GetCapabilities (vtable slot 3), cached.
std::mutex _devMutex;
struct DevType {
    void* dev;
    BYTE type;
};
std::vector<DevType> _devTypes;
BYTE DeviceType(void* dev) {
    std::lock_guard lock(_devMutex);
    for (const auto& d : _devTypes)
        if (d.dev == dev) return d.type;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    using CapsFn = HRESULT(STDMETHODCALLTYPE*)(void*, DIDEVCAPS*);
    auto getCaps = reinterpret_cast<CapsFn>((*reinterpret_cast<void***>(dev))[3]);
    const BYTE t = SUCCEEDED(getCaps(dev, &caps)) ? static_cast<BYTE>(caps.dwDevType & 0xFF) : 0;
    if (_devTypes.size() < 64) _devTypes.push_back({dev, t});
    return t;
}

// PressGameKey: a key the game should see pressed (keyboard device only)
std::mutex _keyMutex;
uint8_t _keyDik = 0;
ULONGLONG _keyUntil = 0;   // held until then
bool _keyPressQueued = false, _keyReleaseQueued = false;  // buffered events still to hand out
DWORD _keySeq = 0x40000000;

HRESULT STDMETHODCALLTYPE StateDetour(GetDeviceStateFn orig, void* self, DWORD size, void* data) {
    const HRESULT hr = orig(self, size, data);
    if (SUCCEEDED(hr) && data && BlockNow()) std::memset(data, 0, size);
    if (SUCCEEDED(hr) && data && size >= 256 && !BlockNow()) {
        std::lock_guard lock(_keyMutex);
        if (_keyDik && GetTickCount64() < _keyUntil && DeviceType(self) == DI8DEVTYPE_KEYBOARD)
            static_cast<uint8_t*>(data)[_keyDik] = 0x80;
    }
    return hr;
}

// Appends the queued press/release of PressGameKey to a buffered keyboard read.
void AppendGameKey(void* self, DWORD objSize, DIDEVICEOBJECTDATA* data, DWORD* inOut, DWORD capacity) {
    std::lock_guard lock(_keyMutex);
    if (!_keyDik || !(_keyPressQueued || _keyReleaseQueued) || DeviceType(self) != DI8DEVTYPE_KEYBOARD) return;
    auto add = [&](bool down) {
        if (*inOut >= capacity) return false;
        auto* ev = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<uint8_t*>(data) +
                                                         static_cast<size_t>(*inOut) * objSize);
        std::memset(ev, 0, objSize);
        ev->dwOfs = _keyDik;
        ev->dwData = down ? 0x80 : 0;
        ev->dwTimeStamp = GetTickCount();
        ev->dwSequence = ++_keySeq;
        ++*inOut;
        return true;
    };
    if (_keyPressQueued && add(true)) _keyPressQueued = false;
    if (!_keyPressQueued && _keyReleaseQueued && GetTickCount64() >= _keyUntil && add(false)) _keyReleaseQueued = false;
}
HRESULT STDMETHODCALLTYPE StateDetourA(void* self, DWORD size, void* data) {
    return StateDetour(_origStateA, self, size, data);
}
HRESULT STDMETHODCALLTYPE StateDetourW(void* self, DWORD size, void* data) {
    return StateDetour(_origStateW, self, size, data);
}

HRESULT DataDetour(GetDeviceDataFn orig, void* self, DWORD objSize, DIDEVICEOBJECTDATA* data, DWORD* inOut,
                   DWORD flags) {
    const DWORD capacity = inOut ? *inOut : 0;
    const HRESULT hr = orig(self, objSize, data, inOut, flags);
    if (SUCCEEDED(hr) && inOut && data && objSize >= sizeof(DIDEVICEOBJECTDATA) && !(flags & DIGDD_PEEK) &&
        !BlockNow())
        AppendGameKey(self, objSize, data, inOut, capacity);
    if (FAILED(hr) || !inOut || !BlockNow()) return hr;
    if (!data || objSize < sizeof(DIDEVICEOBJECTDATA)) {
        *inOut = 0;
        return hr;
    }
    // Drop presses and mouse movement; keep key/button releases, so a key that was down when capture began is
    // not left "stuck" in the game's input state.
    const bool keyboard = DeviceType(self) == DI8DEVTYPE_KEYBOARD;
    DWORD kept = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(data);
    for (DWORD i = 0; i < *inOut; ++i) {
        auto* ev = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + static_cast<size_t>(i) * objSize);
        const bool isButton = keyboard || ev->dwOfs >= DIMOFS_BUTTON0;
        if (isButton && !(ev->dwData & 0x80)) {
            if (kept != i) std::memmove(bytes + static_cast<size_t>(kept) * objSize, ev, objSize);
            ++kept;
        }
    }
    *inOut = kept;
    return hr;
}
HRESULT STDMETHODCALLTYPE DataDetourA(void* self, DWORD objSize, DIDEVICEOBJECTDATA* data, DWORD* inOut,
                                      DWORD flags) {
    return DataDetour(_origDataA, self, objSize, data, inOut, flags);
}
HRESULT STDMETHODCALLTYPE DataDetourW(void* self, DWORD objSize, DIDEVICEOBJECTDATA* data, DWORD* inOut,
                                      DWORD flags) {
    return DataDetour(_origDataW, self, objSize, data, inOut, flags);
}

// The real pad state of user 0, or the test pad while it is enabled (its buttons on top of a real pad).
DWORD ReadPad(DWORD user, XINPUT_STATE* state) {
    DWORD r = _origXInput(user, state);
    if (user != 0 || !_testPadEnabled.load() || !state) return r;
    if (r != ERROR_SUCCESS) std::memset(state, 0, sizeof(*state));
    std::lock_guard lock(_testPadMutex);
    if (_testPadUntil && GetTickCount64() >= _testPadUntil) {
        _testPad = {};
        _testPadUntil = 0;
        ++_testPadPacket;
    }
    XINPUT_GAMEPAD& g = state->Gamepad;
    g.wButtons |= _testPad.wButtons;
    if (_testPad.bLeftTrigger) g.bLeftTrigger = _testPad.bLeftTrigger;
    if (_testPad.bRightTrigger) g.bRightTrigger = _testPad.bRightTrigger;
    if (_testPad.sThumbLX || _testPad.sThumbLY) {
        g.sThumbLX = _testPad.sThumbLX;
        g.sThumbLY = _testPad.sThumbLY;
    }
    if (_testPad.sThumbRX || _testPad.sThumbRY) {
        g.sThumbRX = _testPad.sThumbRX;
        g.sThumbRY = _testPad.sThumbRY;
    }
    state->dwPacketNumber += _testPadPacket;
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputCapsDetour(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    const DWORD r = _origXInputCaps(user, flags, caps);
    if (r == ERROR_SUCCESS || user != 0 || !_testPadEnabled.load() || !caps) return r;
    std::memset(caps, 0, sizeof(*caps));  // the test pad looks like a wired Xbox controller
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY =
        static_cast<SHORT>(0xFFC0);
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputDetour(DWORD user, XINPUT_STATE* state) {
    const DWORD r = ReadPad(user, state);
    if (r == ERROR_SUCCESS && state) {
        if (user == 0) _pad = state->Gamepad.wButtons;
        if (BlockNow()) std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    }
    return r;
}

// Gets the device vtable for one interface flavour and hooks its GetDeviceState/GetDeviceData.
bool HookDevice(HMODULE dinput, const GUID& iid, bool wide) {
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(dinput, "DirectInput8Create")));
    if (!create) return false;
    IUnknown* di = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid, reinterpret_cast<void**>(&di), nullptr)) ||
        !di)
        return false;
    // CreateDevice is vtable slot 3 for both IDirectInput8A and W (A takes the same GUID).
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
    auto createDevice = reinterpret_cast<CreateDeviceFn>((*reinterpret_cast<void***>(di))[3]);
    void* dev = nullptr;
    bool ok = false;
    if (SUCCEEDED(createDevice(di, kGUID_SysKeyboard, &dev, nullptr)) && dev) {
        void** vt = *reinterpret_cast<void***>(dev);
        void* state = vt[kGetDeviceStateIndex];
        void* data = vt[kGetDeviceDataIndex];
        if (wide) {
            _targetStateW = state;
            _targetDataW = data;
            ok = core::hook::Install("IDirectInputDevice8W::GetDeviceState", state,
                                     reinterpret_cast<void*>(&StateDetourW), reinterpret_cast<void**>(&_origStateW)) &&
                 core::hook::Install("IDirectInputDevice8W::GetDeviceData", data,
                                     reinterpret_cast<void*>(&DataDetourW), reinterpret_cast<void**>(&_origDataW));
        } else if (state == _targetStateW && data == _targetDataW) {
            ok = true;  // same code as the W flavour (already hooked)
        } else {
            ok = core::hook::Install("IDirectInputDevice8A::GetDeviceState", state,
                                     reinterpret_cast<void*>(&StateDetourA), reinterpret_cast<void**>(&_origStateA)) &&
                 core::hook::Install("IDirectInputDevice8A::GetDeviceData", data,
                                     reinterpret_cast<void*>(&DataDetourA), reinterpret_cast<void**>(&_origDataA));
        }
        reinterpret_cast<IUnknown*>(dev)->Release();
    }
    di->Release();
    return ok;
}

void Push(const Event& e) {
    RECT rc{};
    HWND h = _hwnd.load();
    if (h) GetClientRect(h, &rc);
    std::lock_guard lock(_mutex);
    if (_events.size() > 256) _events.pop_front();
    _events.push_back({e, static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top)});
}

}  // namespace

bool Install() {
    bool ok = true;
    HMODULE dinput = LoadLibraryW(L"dinput8.dll");  // the game imports it, so this is the same module
    if (!dinput || !HookDevice(dinput, kIID_DirectInput8W, true) || !HookDevice(dinput, kIID_DirectInput8A, false)) {
        MC_LOG_WARN("ui_input: DirectInput hooks failed; the game may still see keys while a co-op panel is open");
        ok = false;
    }
    HMODULE xinput = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!xinput) xinput = LoadLibraryW(L"xinput9_1_0.dll");
    void* xget = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    if (!xget || !core::hook::Install("XInputGetState", xget, reinterpret_cast<void*>(&XInputDetour),
                                      reinterpret_cast<void**>(&_origXInput))) {
        MC_LOG_WARN("ui_input: XInput hook failed; no gamepad in co-op panels");
        ok = false;
    }
    void* xcaps = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetCapabilities")) : nullptr;
    if (xcaps)
        core::hook::Install("XInputGetCapabilities", xcaps, reinterpret_cast<void*>(&XInputCapsDetour),
                            reinterpret_cast<void**>(&_origXInputCaps));
    MC_LOG_INFO("ui_input: installed (DirectInput %s, XInput %s)", _origStateW ? "ok" : "off",
                _origXInput ? "ok" : "off");
    return ok;
}

void SetWindow(void* hwnd) { _hwnd = static_cast<HWND>(hwnd); }

void SetCapture(bool on) {
    const bool was = _capture.exchange(on);
    if (was && !on) _guard = true;
    if (on != was) {
        std::lock_guard lock(_mutex);
        _events.clear();
    }
}

bool Blocking() { return BlockNow(); }

bool OnWindowMessage(unsigned msg, uint64_t wp, int64_t lp) {
    const bool capture = _capture.load();
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!capture) return BlockNow() && msg == WM_KEYDOWN;
        if (wp == VK_F4 && (GetAsyncKeyState(VK_MENU) & 0x8000)) return false;  // Alt+F4 still closes the game
        Push({Event::Key, static_cast<uint32_t>(wp), 0, 0});
        return true;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        // Releases always reach the game: its menus saw the press of the key that opened our panel and must see
        // that key come up, or they act on it later.
        return false;
    case WM_CHAR:
        if (!capture) return BlockNow();
        Push({Event::Char, static_cast<uint32_t>(wp), 0, 0});
        return true;
    case WM_MOUSEMOVE:
        if (!capture) return false;
        Push({Event::MouseMove, 0, static_cast<float>(static_cast<short>(LOWORD(lp))),
              static_cast<float>(static_cast<short>(HIWORD(lp)))});
        return true;
    case WM_LBUTTONDOWN:
        if (!capture) return false;
        Push({Event::MouseDown, 0, static_cast<float>(static_cast<short>(LOWORD(lp))),
              static_cast<float>(static_cast<short>(HIWORD(lp)))});
        return true;
    case WM_MOUSEWHEEL:
        if (!capture) return false;
        Push({Event::Wheel, static_cast<uint32_t>(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1), 0, 0});
        return true;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        return capture;
    default:
        return false;
    }
}

void InjectEvent(const Event& e) {
    if (!_capture.load()) return;
    unsigned bw = 0, bh = 0;
    render::Overlay::BackbufferSize(&bw, &bh);
    std::lock_guard lock(_mutex);
    if (_events.size() > 256) _events.pop_front();
    // client size = backbuffer size: PopEvent scales by 1
    _events.push_back({e, static_cast<float>(bw), static_cast<float>(bh)});
}

bool PopEvent(Event* out) {
    RawEvent r;
    {
        std::lock_guard lock(_mutex);
        if (_events.empty()) return false;
        r = _events.front();
        _events.pop_front();
    }
    *out = r.e;
    if (out->type == Event::MouseMove || out->type == Event::MouseDown) {
        unsigned bw = 0, bh = 0;
        render::Overlay::BackbufferSize(&bw, &bh);
        if (r.clientW > 0 && r.clientH > 0 && bw && bh) {
            out->x = out->x * static_cast<float>(bw) / r.clientW;
            out->y = out->y * static_cast<float>(bh) / r.clientH;
        }
    }
    return true;
}

uint8_t PadLeftTrigger() {
    if (!_origXInput) return 0;
    XINPUT_STATE st{};
    if (ReadPad(0, &st) != ERROR_SUCCESS) return 0;
    return st.Gamepad.bLeftTrigger;
}

uint16_t PadButtons() {
    if (!_origXInput) return 0;
    XINPUT_STATE st{};
    if (ReadPad(0, &st) != ERROR_SUCCESS) return 0;
    _pad = st.Gamepad.wButtons;
    uint16_t b = st.Gamepad.wButtons;
    constexpr SHORT kStick = 16000;  // left stick up/down counts as the d-pad
    if (st.Gamepad.sThumbLY > kStick) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -kStick) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    return b;
}

void PressGameKey(uint8_t dik, uint32_t holdMs) {
    std::lock_guard lock(_keyMutex);
    _keyDik = dik;
    _keyUntil = GetTickCount64() + (holdMs ? holdMs : 100);
    _keyPressQueued = _keyReleaseQueued = true;
}

void EnableTestPad(bool on) {
    if (_testPadEnabled.exchange(on) != on) MC_LOG_INFO("ui_input: test pad %s", on ? "enabled" : "disabled");
}

bool SetTestPad(const TestPadState& st, uint32_t holdMs) {
    if (!_testPadEnabled.load() || !_origXInput) return false;
    std::lock_guard lock(_testPadMutex);
    _testPad.wButtons = st.buttons;
    _testPad.bLeftTrigger = st.leftTrigger;
    _testPad.bRightTrigger = st.rightTrigger;
    _testPad.sThumbLX = st.lx;
    _testPad.sThumbLY = st.ly;
    _testPad.sThumbRX = st.rx;
    _testPad.sThumbRY = st.ry;
    _testPadUntil = holdMs ? GetTickCount64() + holdMs : 0;
    ++_testPadPacket;
    return true;
}

}  // namespace mc::ui_input
