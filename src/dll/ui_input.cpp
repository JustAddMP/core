#include "dll/ui_input.h"

#include "core/hook.h"
#include "core/log.h"
#include "dll/pad_hold.h"
#include "render/overlay.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
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
// Buttons held per XInput user, from the game's reads and ours (0 when the read failed: unplugged). The release guard
// waits for all of them; a state too old to trust (the game stopped polling XInput) it reads again (dll/pad_hold.h).
constexpr DWORD kUsers = PadHold::kUsers;
PadHold _pads;
DWORD ReadPad(DWORD user, XINPUT_STATE* state);
// Changes whenever blocking starts or ends: added to the XInput packet number the game sees, so it never skips the
// switch to the empty state (or back) as "unchanged".
std::atomic<DWORD> _blockEpoch{0};

// The last input device, for the prompts (PromptDevice).
std::mutex _deviceMutex;
prompts::DeviceTracker _tracker;
std::atomic<prompts::Device> _device{prompts::Device::KeyboardMouse};

void DeviceChanged(const char* why, unsigned detail) {  // _deviceMutex held
    _device = _tracker.Current();
    MC_LOG_INFO("ui_input: prompts now for %s (%s 0x%X)", prompts::DeviceName(_tracker.Current()), why, detail);
}
void NoteKeyboard(const char* why, unsigned detail) {
    std::lock_guard lock(_deviceMutex);
    if (_tracker.Keyboard()) DeviceChanged(why, detail);
}
void NoteMouseMove(float x, float y) {
    std::lock_guard lock(_deviceMutex);
    if (_tracker.MouseMove(x, y)) DeviceChanged("mouse move", 0);
}
void NotePad(DWORD user, bool ok, const XINPUT_STATE& st) {
    std::lock_guard lock(_deviceMutex);
    if (!ok) {
        _tracker.PadGone(user);
        return;
    }
    const XINPUT_GAMEPAD& g = st.Gamepad;
    if (_tracker.Pad(user, g.wButtons, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY, g.bLeftTrigger,
                     g.bRightTrigger))
        DeviceChanged("pad buttons", g.wButtons);
}

struct RawEvent {
    Event e;
    float clientW, clientH;
};
std::mutex _mutex;
std::deque<RawEvent> _events;

bool AnyHeld() {
    for (int vk : {VK_ESCAPE, VK_RETURN, VK_SPACE, VK_BACK, VK_LBUTTON, VK_UP, VK_DOWN})
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    // every button a panel acts on (X / Y prompts close some panels: START, INVITE): the press must not reach the game
    constexpr WORD kPanelButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
                                   XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_SHOULDER |
                                   XINPUT_GAMEPAD_RIGHT_SHOULDER;
    return _pads.AnyHeld(kPanelButtons, GetTickCount64(), [](int user, uint16_t* buttons) {
        if (!_origXInput) return false;
        XINPUT_STATE st{};
        if (ReadPad(static_cast<DWORD>(user), &st) != ERROR_SUCCESS) return false;
        *buttons = st.Gamepad.wButtons;
        return true;
    });
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
        if (_guard.exchange(false)) ++_blockEpoch;
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

// Joysticks, gamepads, wheels, flight sticks, 1st-person controllers, supplemental devices (pedals...).
bool IsJoystick(BYTE type) {
    return (type >= DI8DEVTYPE_JOYSTICK && type <= DI8DEVTYPE_1STPERSON) || type == DI8DEVTYPE_SUPPLEMENTAL;
}

// DirectInput joysticks / gamepads (DIJOYSTATE / DIJOYSTATE2): the axes of the last unblocked read, per device, so
// a blocked read can hand them back unchanged (an axis of 0 is pushed to its end, not centred).
std::mutex _joyMutex;
struct JoyAxes {
    void* dev;
    LONG axes[8];  // lX lY lZ lRx lRy lRz rglSlider[2]
};
std::vector<JoyAxes> _joyAxes;

void BlockJoystickState(void* self, DWORD size, void* data) {
    auto* js = static_cast<DIJOYSTATE*>(data);
    {
        std::lock_guard lock(_joyMutex);
        for (const auto& j : _joyAxes)
            if (j.dev == self) std::memcpy(&js->lX, j.axes, sizeof(j.axes));
    }
    for (DWORD& pov : js->rgdwPOV) pov = 0xFFFFFFFF;  // centred
    std::memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
    if (size >= sizeof(DIJOYSTATE2)) {  // DIJOYSTATE2: 128 buttons, then velocities / accelerations / forces
        auto* js2 = static_cast<DIJOYSTATE2*>(data);
        std::memset(js2->rgbButtons, 0, sizeof(js2->rgbButtons));
        std::memset(&js2->lVX, 0, size - offsetof(DIJOYSTATE2, lVX));
    }
}

void RememberJoystickAxes(void* self, const void* data) {
    const auto* js = static_cast<const DIJOYSTATE*>(data);
    std::lock_guard lock(_joyMutex);
    for (auto& j : _joyAxes)
        if (j.dev == self) {
            std::memcpy(j.axes, &js->lX, sizeof(j.axes));
            return;
        }
    if (_joyAxes.size() >= 16) return;
    JoyAxes j{self, {}};
    std::memcpy(j.axes, &js->lX, sizeof(j.axes));
    _joyAxes.push_back(j);
}

HRESULT STDMETHODCALLTYPE StateDetour(GetDeviceStateFn orig, void* self, DWORD size, void* data) {
    const HRESULT hr = orig(self, size, data);
    if (SUCCEEDED(hr) && data) {
        const bool joyLayout = size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2);
        const bool joystick = joyLayout && IsJoystick(DeviceType(self));
        if (BlockNow()) {
            if (joystick)
                BlockJoystickState(self, size, data);
            else
                std::memset(data, 0, size);
        } else if (joystick) {
            RememberJoystickAxes(self, data);
        }
    }
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
    // Joysticks: only their buttons (DIJOFS_BUTTON0..127) count as buttons; their axes and POVs are dropped.
    const BYTE type = DeviceType(self);
    const bool keyboard = type == DI8DEVTYPE_KEYBOARD;
    const bool joystick = IsJoystick(type);
    DWORD kept = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(data);
    for (DWORD i = 0; i < *inOut; ++i) {
        auto* ev = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + static_cast<size_t>(i) * objSize);
        const bool isButton = keyboard || (joystick ? ev->dwOfs >= DIJOFS_BUTTON0 && ev->dwOfs < DIJOFS_BUTTON(128)
                                                    : ev->dwOfs >= DIMOFS_BUTTON0);
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
    if (user < kUsers)
        _pads.Set(static_cast<int>(user), r == ERROR_SUCCESS && state ? state->Gamepad.wButtons : 0, GetTickCount64());
    if (user < kUsers && state) NotePad(user, r == ERROR_SUCCESS, *state);  // before blocking: the real state
    if (r == ERROR_SUCCESS && state) {
        if (BlockNow()) std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
        state->dwPacketNumber += _blockEpoch.load() * 0x10000u;
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
    if (on && !was) ++_blockEpoch;  // blocking starts (when it ends, BlockNow bumps it after the release guard)
    if (on != was) {
        std::lock_guard lock(_mutex);
        _events.clear();
    }
}

bool Blocking() { return BlockNow(); }

prompts::Device PromptDevice() { return _device.load(); }

bool OnWindowMessage(unsigned msg, uint64_t wp, int64_t lp) {
    const bool capture = _capture.load();
    switch (msg) {  // which device the prompts show (all the time, not only under a panel)
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: NoteKeyboard("key", static_cast<unsigned>(wp)); break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_MOUSEWHEEL: NoteKeyboard("mouse button", msg); break;
    case WM_MOUSEMOVE:
        NoteMouseMove(static_cast<float>(static_cast<short>(LOWORD(lp))),
                      static_cast<float>(static_cast<short>(HIWORD(lp))));
        break;
    default: break;
    }
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
    if (e.type == Event::MouseMove)
        NoteMouseMove(e.x, e.y);
    else
        NoteKeyboard("test event", static_cast<unsigned>(e.type));
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

// Our own reads of every user: which are connected, when an empty slot is probed next.
struct PadSlots {
    bool connected[kUsers] = {};
    ULONGLONG nextProbe[kUsers] = {};
    ULONGLONG cachedAt = 0;  // ReadPads runs several times a frame: the pads are read once per tick (~16 ms)
    PadSnapshot cached;
};
PadSlots& Slots() {
    static PadSlots s;
    return s;
}
constexpr ULONGLONG kProbeEmptyMs = 1000;

PadSnapshot ReadPads() {
    PadSnapshot out;
    if (!_origXInput) return out;
    static std::mutex m;  // the game layer may ask from its own thread too
    std::lock_guard lock(m);
    PadSlots& sl = Slots();
    const ULONGLONG now = GetTickCount64();
    if (sl.cachedAt == now) return sl.cached;
    int best = -1;
    for (DWORD u = 0; u < kUsers; ++u) {
        // the test pad lives on user 0 and is always "connected" while enabled
        const bool test = u == 0 && _testPadEnabled.load();
        if (!sl.connected[u] && !test && now < sl.nextProbe[u]) continue;
        XINPUT_STATE st{};
        const bool ok = ReadPad(u, &st) == ERROR_SUCCESS;
        _pads.Set(static_cast<int>(u), ok ? st.Gamepad.wButtons : 0, now);
        NotePad(u, ok, st);
        if (ok != sl.connected[u]) {
            sl.connected[u] = ok;
            if (!test) MC_LOG_INFO("ui_input: gamepad %lu %s", u, ok ? "connected" : "disconnected");
        }
        if (!ok) {
            sl.nextProbe[u] = now + kProbeEmptyMs;
            continue;
        }
        const XINPUT_GAMEPAD& g = st.Gamepad;
        out.connected |= static_cast<uint8_t>(1u << u);
        out.buttons |= g.wButtons;
        out.leftTrigger = std::max(out.leftTrigger, g.bLeftTrigger);
        out.rightTrigger = std::max(out.rightTrigger, g.bRightTrigger);
        const int mag = std::max(std::abs(static_cast<int>(g.sThumbLX)), std::abs(static_cast<int>(g.sThumbLY)));
        if (mag > best) {
            best = mag;
            out.lx = g.sThumbLX;
            out.ly = g.sThumbLY;
        }
    }
    sl.cachedAt = now;
    sl.cached = out;
    return out;
}

uint8_t PadLeftTrigger() { return ReadPads().leftTrigger; }

uint16_t PadButtons() {
    const PadSnapshot p = ReadPads();
    uint16_t b = p.buttons;
    constexpr SHORT kStick = 16000;  // left stick up/down counts as the d-pad
    if (p.ly > kStick) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (p.ly < -kStick) b |= XINPUT_GAMEPAD_DPAD_DOWN;
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
