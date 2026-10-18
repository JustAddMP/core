#pragma once
// Input for our own UI panels, and blocking the game's input while one is open.
//
// The game reads the keyboard and mouse with DirectInput 8 (IDirectInputDevice8::GetDeviceState/GetDeviceData),
// the gamepad with XInput (xinput9_1_0.dll XInputGetState) and also gets window messages (its Scaleform menus).
// While capture is on we hand the game empty DirectInput/XInput state and swallow keyboard/mouse window messages;
// our panels read the swallowed messages and the real pad state instead. After capture ends, blocking continues
// until the confirm/back keys and pad buttons are released, so the key that closed our panel does not also act in
// the game's menu.
// Gamepads: every XInput user (0-3) counts, not only the first; a pad that is unplugged while a button is held no
// longer counts as holding it. A held state nobody has read for 100 ms (the game stopped polling XInput, e.g. a
// loading screen) is read again by the guard itself (dll/pad_hold.h), so a button released meanwhile does not keep
// the keyboard blocked. Each change between blocked and not blocked gives the game a new XInput packet
// number, so a game that skips unchanged packets still sees the empty (or the real) state. DirectInput joysticks /
// gamepads, if a game reads any, are blocked with centred POVs, released buttons and their axes frozen where they
// were (not zeroed: 0 is an axis pushed to its end).
#include <cstdint>

#include "dll/pad_nav.h"
#include "dll/ui_prompts.h"

namespace mc::ui_input {

// The input device the player used last (dll/ui_prompts.h DeviceTracker), for the panels' button prompts: followed
// all the time, whether a panel is open or not, from the window's key/mouse messages and every XInput read (the
// game's and ours, the test pad included), plus injected test events. Each switch is logged. Any thread.
prompts::Device PromptDevice();

bool Install();  // init thread, once; failures are logged and only disable blocking

void SetCapture(bool on);  // render thread
bool Blocking();           // capture on, or waiting for keys to be released

// Window procedure filter (window thread). True = swallow the message.
bool OnWindowMessage(unsigned msg, uint64_t wp, int64_t lp);
void SetWindow(void* hwnd);

struct Event {
    enum Type { Key, Char, MouseMove, MouseDown, Wheel } type;
    uint32_t code;  // Key: virtual key; Char: UTF-16 unit; MouseDown: button (0 left); Wheel: +1/-1
    float x, y;     // mouse events: client coordinates scaled to the backbuffer
};
bool PopEvent(Event* out);  // render thread
// Tests (dll/test_input.h): queue an event as if it came from the window; mouse coordinates in backbuffer pixels.
// Dropped unless a panel captures input, like the window's own events. Any thread.
void InjectEvent(const Event& e);

// Every connected gamepad combined (buttons OR-ed, the left stick of the pad pushed furthest, the larger
// triggers) for our panels (dll/pad_nav.h). Unplugged users are probed again once a second, not every frame
// (XInputGetState on an empty slot is slow). Connects and disconnects are logged. Render thread.
PadSnapshot ReadPads();
// ReadPads' buttons as XINPUT_GAMEPAD_* bits, left stick up/down reported as d-pad; 0 if no pad. Render thread.
uint16_t PadButtons();
// The largest left trigger of the connected pads (0..255), 0 if none. Render thread.
uint8_t PadLeftTrigger();

// Test pad, for automated tests (synthetic mouse clicks never reach some games; a pad does): a virtual gamepad on
// XInput user 0 that the game and our panels read like a real one. Off by default; the dll turns it on with
// `test_pad = 1` (ini) / `--test_pad 1` (launcher). SetTestPad holds the given state for holdMs milliseconds
// (0 = until the next call), then releases everything; false if the test pad is off. Any thread.
struct TestPadState {
    uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
void EnableTestPad(bool on);
bool SetTestPad(const TestPadState& state, uint32_t holdMs);

// A key press for the game itself (not our panels): the keyboard's DirectInput state shows DirectInput key `dik`
// (DIK_*) down for holdMs, and buffered reads get its press and release events, as if the player pressed it. For
// prompts a game shows that need a key (e.g. a loading screen's "Enter: continue"). Ignored while one of our panels
// captures input. Any thread.
void PressGameKey(uint8_t dik, uint32_t holdMs);

}  // namespace mc::ui_input
