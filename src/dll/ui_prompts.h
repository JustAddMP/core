#pragma once
// Button prompts of our panels (the footer row "A Select  B Back"), the way the games' own menus show them (pure
// logic, no Windows: tests/ui_prompts.cpp drives it with made-up input).
//
// Game menus show their footer prompts for the input device the player used last: a gamepad's button glyphs after a
// pad button or stick, the keyboard key names after a key, a click or a mouse move, switching back and forth at once.
// The buttons do exactly what their prompts say. Our panels (dll/coop_ui.cpp) do the same:
//   - DeviceTracker follows the last device (fed by dll/ui_input.cpp from the window messages and every XInput read,
//     the game's and ours, whether a panel is open or not, so a panel opens with the device the game showed last);
//   - a panel describes its prompts once (Build: the stepper hint, accept, the panel's extra X / Y actions, back) and
//     both the footer drawing and the pad handling use that one list (PadAction), so a prompt never shows a button that
//     does something else, and a button without a prompt does nothing beyond A / B / the d-pad.
// With the keyboard only prompts with a key are shown (some extra actions are pad shortcuts for menu entries).
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "dll/pad_nav.h"

namespace mc::prompts {

enum class Device : uint8_t { KeyboardMouse, Pad };

inline const char* DeviceName(Device d) { return d == Device::Pad ? "gamepad" : "keyboard/mouse"; }

// The device the prompts are shown for: the one used last. Only changes count: a new button press, a stick or
// trigger crossing its threshold, a key, a click, the wheel, a mouse move of more than kMouseSlackPx (games that keep
// the cursor centred post moves to the same spot every frame; those are not a mouse being used). A stick that stays
// pushed or a button that stays held does not take the prompts back from the keyboard. Not thread-safe (ui_input
// holds a lock around it).
class DeviceTracker {
public:
    static constexpr float kMouseSlackPx = 8.0f;
    static constexpr int kStick = 16000;     // as PadNav::kStickPress
    static constexpr uint8_t kTrigger = 128;
    static constexpr unsigned kUsers = 4;

    Device Current() const { return dev_; }

    // A key down, a character, a mouse button or the wheel. True when the device changed.
    bool Keyboard() { return Set(Device::KeyboardMouse); }

    // The cursor at (x, y). The first move only records the spot.
    bool MouseMove(float x, float y) {
        if (!mouseSeen_) {
            mouseSeen_ = true;
            mx_ = x;
            my_ = y;
            return false;
        }
        if (std::fabs(x - mx_) <= kMouseSlackPx && std::fabs(y - my_) <= kMouseSlackPx) return false;
        mx_ = x;
        my_ = y;
        return Set(Device::KeyboardMouse);
    }

    // One pad's state (XInput user 0-3). True when the device changed.
    bool Pad(unsigned user, uint16_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t rt) {
        if (user >= kUsers) return false;
        uint8_t pushed = 0;
        if (Beyond(lx) || Beyond(ly)) pushed |= 1;
        if (Beyond(rx) || Beyond(ry)) pushed |= 2;
        if (lt > kTrigger) pushed |= 4;
        if (rt > kTrigger) pushed |= 8;
        const bool edge = (buttons & ~prevButtons_[user]) || (pushed & ~prevPushed_[user]);
        prevButtons_[user] = buttons;
        prevPushed_[user] = pushed;
        return edge && Set(Device::Pad);
    }

    // The pad of `user` is gone (read failed): what it held counts as released.
    void PadGone(unsigned user) {
        if (user >= kUsers) return;
        prevButtons_[user] = 0;
        prevPushed_[user] = 0;
    }

private:
    static bool Beyond(int16_t v) { return v > kStick || v < -kStick; }
    bool Set(Device d) {
        if (d == dev_) return false;
        dev_ = d;
        return true;
    }

    Device dev_ = Device::KeyboardMouse;
    uint16_t prevButtons_[kUsers] = {};
    uint8_t prevPushed_[kUsers] = {};
    bool mouseSeen_ = false;
    float mx_ = 0, my_ = 0;
};

// The pad side of a prompt: the glyph that is drawn and the button that runs it.
enum class Button : uint8_t { A, B, X, Y, LB, RB, DpadLeftRight };

inline const char* ButtonName(Button b) {
    switch (b) {
    case Button::A: return "A";
    case Button::B: return "B";
    case Button::X: return "X";
    case Button::Y: return "Y";
    case Button::LB: return "LB";
    case Button::RB: return "RB";
    case Button::DpadLeftRight: return "D-pad";
    }
    return "?";
}

// What a prompt does (dll/coop_ui.cpp maps these onto the panel's actions).
enum class Do : uint8_t {
    Accept,   // A / Enter: the selected row (the text box's submit)
    Back,     // B / Esc
    Change,   // d-pad / Left-Right: step the selected "< value >" row (handled as movement, not as a press)
    Delete,   // text box: remove the last character
    Paste,    // text box: paste the clipboard
    Refresh,  // JOIN FRIEND: scan the friends again
    Ready,    // lobby (guest): toggle ready
    Start,    // lobby (host): start the game
    JoinGame, // lobby (guest): join the host's running game
    Invite    // host: Steam invite dialog
};

inline const char* DoName(Do d) {
    switch (d) {
    case Do::Accept: return "accept";
    case Do::Back: return "back";
    case Do::Change: return "change";
    case Do::Delete: return "delete";
    case Do::Paste: return "paste";
    case Do::Refresh: return "refresh";
    case Do::Ready: return "ready";
    case Do::Start: return "start";
    case Do::JoinGame: return "join game";
    case Do::Invite: return "invite";
    }
    return "?";
}

struct Prompt {
    Button pad;
    std::string key;    // the keyboard key shown with keyboard/mouse; empty = a pad shortcut, not shown then
    std::string label;  // what it does, as shown ("Select", "Back", ...)
    Do action;
};

// What a panel offers in its footer.
struct Context {
    bool onStepper = false;  // the selected row is a "< value >" row (Left/Right change it)
    std::string changeLabel = "Change";
    std::string accept = "Select";  // empty: no accept prompt (nothing selectable)
    std::string back = "Back";
    bool textBox = false;           // JOIN BY IP / PASSWORD / NAME: X deletes, Y pastes
    bool refresh = false;           // JOIN FRIEND: Y refreshes
    // lobby / CO-OP panel shortcuts for entries of the list (pad only)
    std::string xLabel;  // empty = none
    Do xAction = Do::Start;
    std::string yLabel;  // empty = none
    Do yAction = Do::Invite;
};

// The footer, in the games' order: change, accept, the extras, back.
inline std::vector<Prompt> Build(const Context& c) {
    std::vector<Prompt> p;
    if (c.onStepper) p.push_back({Button::DpadLeftRight, "Left/Right", c.changeLabel, Do::Change});
    if (!c.accept.empty()) p.push_back({Button::A, "Enter", c.accept, Do::Accept});
    if (c.textBox) {
        p.push_back({Button::X, "Backspace", "Delete", Do::Delete});
        p.push_back({Button::Y, "Ctrl+V", "Paste", Do::Paste});
    } else {
        if (!c.xLabel.empty()) p.push_back({Button::X, "", c.xLabel, c.xAction});
        if (c.refresh)
            p.push_back({Button::Y, "F5", "Refresh", Do::Refresh});
        else if (!c.yLabel.empty())
            p.push_back({Button::Y, "", c.yLabel, c.yAction});
    }
    if (!c.back.empty()) p.push_back({Button::B, "Esc", c.back, Do::Back});
    return p;
}

// The prompts shown for a device (keyboard: only those with a key).
inline std::vector<Prompt> Shown(const std::vector<Prompt>& all, Device d) {
    if (d == Device::Pad) return all;
    std::vector<Prompt> out;
    for (const Prompt& p : all)
        if (!p.key.empty()) out.push_back(p);
    return out;
}

// The pad button behind a PadNav press (kX -> X, ...); false for movement and for accept/back (handled directly).
inline bool ButtonFromNav(uint32_t navBit, Button* out) {
    switch (navBit) {
    case PadNav::kX: *out = Button::X; return true;
    case PadNav::kY: *out = Button::Y; return true;
    case PadNav::kLB: *out = Button::LB; return true;
    case PadNav::kRB: *out = Button::RB; return true;
    default: return false;
    }
}

// What pressing `b` does on a panel with these prompts; false = nothing (no prompt shows that button).
inline bool PadAction(const std::vector<Prompt>& all, Button b, Do* out) {
    for (const Prompt& p : all)
        if (p.pad == b) {
            *out = p.action;
            return true;
        }
    return false;
}

// One line for logs and test dumps: "gamepad: [D-pad] Change  [A] Select  [B] Back".
inline std::string Describe(const std::vector<Prompt>& all, Device d) {
    std::string s = DeviceName(d);
    s += ":";
    for (const Prompt& p : Shown(all, d)) {
        s += "  [";
        s += d == Device::Pad ? ButtonName(p.pad) : p.key;
        s += "] " + p.label;
    }
    return s;
}

}  // namespace mc::prompts
