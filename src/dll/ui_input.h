#pragma once
// Input for our own UI panels, and blocking the game's input while one is open.
//
// The game reads the keyboard and mouse with DirectInput 8 (IDirectInputDevice8::GetDeviceState/GetDeviceData),
// the gamepad with XInput (xinput9_1_0.dll XInputGetState) and also gets window messages (its Scaleform menus).
// While capture is on we hand the game empty DirectInput/XInput state and swallow keyboard/mouse window messages;
// our panels read the swallowed messages and the real pad state instead. After capture ends, blocking continues
// until the confirm/back keys and pad buttons are released, so the key that closed our panel does not also act in
// the game's menu.
#include <cstdint>

namespace mc::ui_input {

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

// First gamepad (XInput user 0), buttons as XINPUT_GAMEPAD_* bits (left stick up/down reported as d-pad); 0 if
// none. Render thread.
uint16_t PadButtons();
// First gamepad's left trigger (0..255), 0 if none. Render thread.
uint8_t PadLeftTrigger();

}  // namespace mc::ui_input
