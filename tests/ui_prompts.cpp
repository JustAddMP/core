// ui_prompts: the co-op panels' footer prompts (src/dll/ui_prompts.h) with made-up input: the last-device tracker
// (pad presses / sticks / triggers vs keys, clicks and real mouse moves; held buttons and a cursor pinned to one spot
// do not switch), the prompt lists per panel kind, what is shown for each device, and that every pad button acts only
// through a prompt that shows it. Exits non-zero on any failure.
#include <cstdio>
#include <string>

#include "dll/ui_prompts.h"

using namespace mc::prompts;
using mc::PadNav;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

bool Pad(DeviceTracker& t, uint16_t buttons, int16_t lx = 0, int16_t ly = 0, uint8_t lt = 0, unsigned user = 0) {
    return t.Pad(user, buttons, lx, ly, 0, 0, lt, 0);
}

bool Has(const std::vector<Prompt>& v, Button b, Do d) {
    for (const Prompt& p : v)
        if (p.pad == b && p.action == d) return true;
    return false;
}

}  // namespace

int main() {
    {  // device tracker
        DeviceTracker t;
        Check(t.Current() == Device::KeyboardMouse, "starts on keyboard/mouse");
        Check(!Pad(t, 0), "idle pad: no switch");
        Check(Pad(t, PadNav::kA) && t.Current() == Device::Pad, "A press: gamepad");
        Check(!Pad(t, PadNav::kA), "A still held: no change");
        Check(t.Keyboard() && t.Current() == Device::KeyboardMouse, "a key: keyboard");
        Check(!Pad(t, PadNav::kA) && t.Current() == Device::KeyboardMouse, "A held through the key: stays keyboard");
        Pad(t, 0);
        Check(Pad(t, PadNav::kA) && t.Current() == Device::Pad, "A pressed again: gamepad");
        t.Keyboard();
        Check(Pad(t, 0, 0, 20000) && t.Current() == Device::Pad, "stick pushed: gamepad");
        t.Keyboard();
        Check(!Pad(t, 0, 0, 20000), "stick resting pushed: stays keyboard");
        Pad(t, 0, 0, 0);
        Check(!Pad(t, 0, 0, 9000), "stick within the dead zone: no switch");
        Check(Pad(t, 0, 0, 0, 200) && t.Current() == Device::Pad, "left trigger pulled: gamepad");
        Check(!t.MouseMove(100, 100), "first mouse move only records the spot");
        Check(!t.MouseMove(103, 98) && t.Current() == Device::Pad, "cursor jitter within the slack: no switch");
        for (int i = 0; i < 30; ++i) t.MouseMove(100, 100);  // a game re-centring the cursor every frame
        Check(t.Current() == Device::Pad, "cursor pinned to one spot: no switch");
        Check(t.MouseMove(140, 100) && t.Current() == Device::KeyboardMouse, "a real mouse move: keyboard/mouse");
        // pads are tracked per user: pad 2's press while pad 1 holds a button
        Pad(t, PadNav::kB, 0, 0, 0, 0);
        t.Keyboard();
        Check(Pad(t, PadNav::kA, 0, 0, 0, 1) && t.Current() == Device::Pad, "second pad's press: gamepad");
        // a pad unplugged while held and plugged in again holding it: a new press
        t.Keyboard();
        t.PadGone(1);
        Check(Pad(t, PadNav::kA, 0, 0, 0, 1), "re-plugged pad holding A: counts as a press");
        Check(!t.Pad(7, PadNav::kA, 0, 0, 0, 0, 0, 0), "user out of range: ignored");
    }
    {  // a plain list panel (CO-OP, player menu)
        Context c;
        const auto p = Build(c);
        Check(p.size() == 2 && p[0].pad == Button::A && p[1].pad == Button::B, "list: A Select, B Back");
        Check(p[0].key == "Enter" && p[1].key == "Esc", "list keys: Enter / Esc");
        Check(Describe(p, Device::Pad) == "gamepad:  [A] Select  [B] Back", "gamepad footer", Describe(p, Device::Pad));
        Check(Describe(p, Device::KeyboardMouse) == "keyboard/mouse:  [Enter] Select  [Esc] Back", "keyboard footer",
              Describe(p, Device::KeyboardMouse));
        Do d;
        Check(!PadAction(p, Button::X, &d) && !PadAction(p, Button::Y, &d), "list: X / Y do nothing (no prompt)");
    }
    {  // a "< value >" row selected: the change prompt first
        Context c;
        c.onStepper = true;
        c.accept = "Next";
        const auto p = Build(c);
        Check(p.size() == 3 && p[0].pad == Button::DpadLeftRight && p[0].key == "Left/Right" && p[1].label == "Next",
              "stepper: D-pad Change, A Next, B Back");
    }
    {  // host lobby: X Start, Y Invite (pad shortcuts, not shown for the keyboard)
        Context c;
        c.xLabel = "Start";
        c.xAction = Do::Start;
        c.yLabel = "Invite";
        c.yAction = Do::Invite;
        const auto p = Build(c);
        Check(Has(p, Button::X, Do::Start) && Has(p, Button::Y, Do::Invite), "host lobby: X Start, Y Invite");
        Check(p.back().pad == Button::B, "back is last");
        Check(Shown(p, Device::Pad).size() == 4, "gamepad shows all four");
        Check(Shown(p, Device::KeyboardMouse).size() == 2, "keyboard shows Enter / Esc only");
        Do d = Do::Back;
        Check(PadAction(p, Button::X, &d) && d == Do::Start, "X runs Start");
        Check(PadAction(p, Button::Y, &d) && d == Do::Invite, "Y runs Invite");
        Check(!PadAction(p, Button::LB, &d), "LB does nothing");
    }
    {  // text box: X Delete (Backspace), Y Paste (Ctrl+V), shown for both devices; the extras of a list are ignored
        Context c;
        c.textBox = true;
        c.accept = "Connect";
        c.back = "Cancel";
        c.xLabel = "Start";
        const auto p = Build(c);
        Check(Has(p, Button::X, Do::Delete) && Has(p, Button::Y, Do::Paste) && !Has(p, Button::X, Do::Start),
              "text box: X Delete, Y Paste");
        Check(Describe(p, Device::KeyboardMouse) ==
                  "keyboard/mouse:  [Enter] Connect  [Backspace] Delete  [Ctrl+V] Paste  [Esc] Cancel",
              "text box keyboard footer", Describe(p, Device::KeyboardMouse));
    }
    {  // JOIN FRIEND: Y Refresh (F5)
        Context c;
        c.accept = "Join";
        c.refresh = true;
        c.yLabel = "Invite";  // refresh wins the Y button
        const auto p = Build(c);
        Do d;
        Check(PadAction(p, Button::Y, &d) && d == Do::Refresh, "friends: Y refreshes");
        Check(Describe(p, Device::KeyboardMouse) == "keyboard/mouse:  [Enter] Join  [F5] Refresh  [Esc] Back",
              "friends keyboard footer", Describe(p, Device::KeyboardMouse));
    }
    {  // every prompt's button is unique within a panel (a button never means two things)
        Context c;
        c.onStepper = true;
        c.xLabel = "Ready";
        c.xAction = Do::Ready;
        c.yLabel = "Invite";
        const auto p = Build(c);
        bool unique = true;
        for (size_t i = 0; i < p.size(); ++i)
            for (size_t j = i + 1; j < p.size(); ++j)
                if (p[i].pad == p[j].pad) unique = false;
        Check(unique, "no button appears twice");
    }
    {  // PadNav reports X / Y / LB / RB presses (once, no repeat), never with movement
        PadNav n;
        n.Reset({});
        mc::PadSnapshot s;
        s.connected = 1;
        s.buttons = PadNav::kXButton;
        Check(n.Update(s, 0.016f) == PadNav::kX, "pad X press");
        int again = 0;
        for (int i = 0; i < 60; ++i) again += (n.Update(s, 0.016f) & PadNav::kX) ? 1 : 0;
        Check(again == 0, "X held: no repeat");
        s.buttons = PadNav::kYButton | PadNav::kDpadDown;
        Check(n.Update(s, 0.016f) == PadNav::kY, "Y with d-pad down: Y only");
        s.buttons = 0;
        n.Update(s, 0.016f);
        s.buttons = PadNav::kLBButton | PadNav::kRBButton;
        Check(n.Update(s, 0.016f) == (PadNav::kLB | PadNav::kRB), "LB + RB");
        Button b;
        Check(ButtonFromNav(PadNav::kX, &b) && b == Button::X && ButtonFromNav(PadNav::kRB, &b) && b == Button::RB &&
                  !ButtonFromNav(PadNav::kAccept, &b),
              "nav bits to buttons");
        PadNav h;  // a Y held when the panel opened (the button that opened it) does nothing until released
        s.buttons = PadNav::kYButton;
        h.Reset(s);
        Check(h.Update(s, 0.016f) == 0, "Y held from before: nothing");
    }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
