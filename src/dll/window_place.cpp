#include "dll/window_place.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
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

// ---- window_area (test tool) ---------------------------------------------------------------------------------
Area _area;
using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExWFn _createW = nullptr;
CreateWindowExAFn _createA = nullptr;
volatile LONG _placedCreates = 0;

bool TopLevel(DWORD style, HWND parent) { return !(style & WS_CHILD) && (parent == nullptr || parent == HWND_DESKTOP); }

void NoteCreate(int x0, int y0, int w0, int x, int y, int w) {
    const LONG n = InterlockedIncrement(&_placedCreates);
    if (n <= 12)
        MC_LOG_INFO("window: area: a top-level window made at %d,%d (%d wide) is made at %d,%d (%d wide) (#%ld)", x0, y0,
                    w0, x, y, w, n);
}

HWND WINAPI CreateWindowExWDetour(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h,
                                  HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    if (_area.On() && TopLevel(style, parent)) {
        const int x0 = x, y0 = y, w0 = w;
        FitCreate(_area, x, y, w);
        NoteCreate(x0, y0, w0, x, y, w);
    }
    return _createW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

HWND WINAPI CreateWindowExADetour(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                  HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    if (_area.On() && TopLevel(style, parent)) {
        const int x0 = x, y0 = y, w0 = w;
        FitCreate(_area, x, y, w);
        NoteCreate(x0, y0, w0, x, y, w);
    }
    return _createA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

// The game exe's import of user32!<name> pointed at `detour` (the old target into *original). An import table patch,
// not a MinHook detour: enabling a MinHook hook suspends every thread for ~0.3 s, and two of them on the init thread
// made the main menu's .gfx load before the menu hook existed (2026-10-21: no CO-OP page, no Free Ride start).
bool PatchImport(const char* name, void* detour, void** original) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by->Name), name) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) continue;
            if (!*original) *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            done = true;
        }
    }
    return done;
}

BOOL CALLBACK CountShown(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) ++*reinterpret_cast<int*>(lp);
    return TRUE;
}

}  // namespace

bool ParseArea(const char* text, Area& out) {
    out = Area{};
    if (!text || !*text) return true;
    int a = 0, b = 0, c = 0;
    char tail = 0;
    const int n = std::sscanf(text, "%d,%d,%d%c", &a, &b, &c, &tail);
    if (n == 2 || n == 3) {
        if (b <= a || b - a < 200) return false;
        out.x0 = a;
        out.x1 = b;
        out.y = n == 3 ? c : 0;
        out.haveY = n == 3;
        return true;
    }
    return false;
}

void FitCreate(const Area& a, int& x, int& y, int& w) {
    if (!a.On()) return;
    const int width = a.x1 - a.x0;
    if (w != CW_USEDEFAULT && w > width) w = width;
    x = a.x0;
    if (a.haveY || y == CW_USEDEFAULT) y = a.haveY ? a.y : 0;
}

void EarlyInit() {
    const std::string v = core::Config::Get().GetString("window_area");
    if (v.empty()) return;
    if (!ParseArea(v.c_str(), _area) || !_area.On()) {
        MC_LOG_WARN("window: window_area '%s' not understood (x0,x1[,y]): windows left alone", v.c_str());
        return;
    }
    int shown = 0;
    EnumWindows(CountShown, reinterpret_cast<LPARAM>(&shown));
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    // the real functions first (a call through the patched import goes to them)
    if (user32) {
        _createW = reinterpret_cast<CreateWindowExWFn>(GetProcAddress(user32, "CreateWindowExW"));
        _createA = reinterpret_cast<CreateWindowExAFn>(GetProcAddress(user32, "CreateWindowExA"));
    }
    void* origW = nullptr;
    void* origA = nullptr;
    const bool okW = _createW && PatchImport("CreateWindowExW", reinterpret_cast<void*>(&CreateWindowExWDetour), &origW);
    const bool okA = _createA && PatchImport("CreateWindowExA", reinterpret_cast<void*>(&CreateWindowExADetour), &origA);
    MC_LOG_INFO("window: area x %d..%d%s (a test tool): new top-level windows the game makes are made there (its "
                "imports CreateWindowExW %s, CreateWindowExA %s)%s",
                _area.x0, _area.x1, _area.haveY ? (" y " + std::to_string(_area.y)).c_str() : "",
                okW ? "patched" : "not imported", okA ? "patched" : "not imported",
                shown ? " - a window of this game was already shown before: it was made elsewhere" : "");
}

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

void* Window() { return _hwnd; }

}  // namespace mc::window_place
