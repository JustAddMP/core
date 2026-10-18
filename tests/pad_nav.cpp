// pad_nav: gamepad navigation of the co-op panels (src/dll/pad_nav.h) with made-up pad states: presses only (a held
// A from the menu does nothing), stick with hysteresis and dominant axis, repeat timing, B / START / BACK = back,
// several pads combined, a pad that disappears while held. Exits non-zero on any failure.
#include <cstdio>
#include <string>

#include "dll/pad_nav.h"

using mc::PadNav;
using mc::PadSnapshot;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-56s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

PadSnapshot Buttons(uint16_t b) {
    PadSnapshot s;
    s.buttons = b;
    s.connected = 1;
    return s;
}
PadSnapshot Stick(int lx, int ly) {
    PadSnapshot s;
    s.lx = static_cast<int16_t>(lx);
    s.ly = static_cast<int16_t>(ly);
    s.connected = 1;
    return s;
}

// Runs `frames` frames of `dt` with the same state; returns how many times `action` fired.
int Count(PadNav& n, const PadSnapshot& s, uint32_t action, int frames, float dt = 1.0f / 60) {
    int c = 0;
    for (int i = 0; i < frames; ++i)
        if (n.Update(s, dt) & action) ++c;
    return c;
}

}  // namespace

int main() {
    const PadSnapshot none{};
    {  // the A that opened the panel is still held: nothing until it is released and pressed again
        PadNav n;
        n.Reset(Buttons(PadNav::kA));
        Check(Count(n, Buttons(PadNav::kA), PadNav::kAccept, 30) == 0, "held A from the menu does not accept");
        Check(n.Update(none, 0.016f) == 0, "release: nothing");
        Check(n.Update(Buttons(PadNav::kA), 0.016f) == PadNav::kAccept, "new A press accepts");
        Check(Count(n, Buttons(PadNav::kA), PadNav::kAccept, 60) == 0, "A held: accepts once, no repeat");
    }
    {  // back buttons
        for (uint16_t b : {PadNav::kB, PadNav::kStart, PadNav::kBackButton}) {
            PadNav n;
            n.Reset(none);
            Check(n.Update(Buttons(b), 0.016f) == PadNav::kBack, "B / START / BACK = back", std::to_string(b));
        }
    }
    {  // d-pad: one step, then repeat after 0.4 s, every 0.15 s
        PadNav n;
        n.Reset(none);
        Check(n.Update(Buttons(PadNav::kDpadDown), 0.016f) == PadNav::kDown, "d-pad down: one step");
        // 0.4 s more held: the first repeat fires at 0.4 s
        const int first = Count(n, Buttons(PadNav::kDpadDown), PadNav::kDown, 24, 0.0166f);  // ~0.398 s
        Check(first == 0, "no repeat before 0.4 s", std::to_string(first));
        const int rep = Count(n, Buttons(PadNav::kDpadDown), PadNav::kDown, 60, 0.0166f);  // ~1 s
        Check(rep >= 6 && rep <= 8, "then about every 0.15 s", std::to_string(rep) + " in 1 s");
        // a long frame (game hitch) gives one step, not a burst
        Check(Count(n, Buttons(PadNav::kDpadDown), PadNav::kDown, 1, 2.0f) == 1, "a 2 s frame: one step");
        Check(Count(n, Buttons(PadNav::kDpadDown), PadNav::kDown, 1, 0.0166f) == 0, "... and no burst after it");
    }
    {  // stick: threshold, hysteresis, dominant axis
        PadNav n;
        n.Reset(none);
        Check(n.Update(Stick(0, 15000), 0.016f) == 0, "stick below the press threshold: nothing");
        Check(n.Update(Stick(0, 17000), 0.016f) == PadNav::kUp, "stick up beyond it: up");
        // wobbling between 12000 and 17000 does not press again
        int extra = 0;
        for (int i = 0; i < 20; ++i) extra += (n.Update(Stick(0, i % 2 ? 12000 : 17000), 0.016f) & PadNav::kUp) ? 1 : 0;
        Check(extra == 0, "wobble above the release threshold: no new press", std::to_string(extra));
        Check(n.Update(Stick(0, 9000), 0.016f) == 0, "below the release threshold: released");
        Check(n.Update(Stick(0, 17000), 0.016f) == PadNav::kUp, "pushed again: up again");
        n.Update(none, 0.016f);
        Check(n.Update(Stick(20000, 17000), 0.016f) == PadNav::kRight, "diagonal: the dominant axis only (right)");
        n.Update(none, 0.016f);
        Check(n.Update(Stick(0, -30000), 0.016f) == PadNav::kDown, "stick down: down");
        n.Update(none, 0.016f);
        Check(n.Update(Stick(-30000, 0), 0.016f) == PadNav::kLeft, "stick left: left");
    }
    {  // a stick still pushed when the panel opens does not move until it comes back
        PadNav n;
        n.Reset(Stick(0, -32000));
        Check(Count(n, Stick(0, -32000), PadNav::kDown, 60) == 0, "stick held at open: no movement");
        n.Update(none, 0.016f);
        Check(n.Update(Stick(0, -32000), 0.016f) == PadNav::kDown, "after centring: moves");
    }
    {  // accept wins over a direction in the same frame
        PadNav n;
        n.Reset(none);
        const uint32_t a = n.Update(Buttons(PadNav::kA | PadNav::kDpadDown), 0.016f);
        Check(a == PadNav::kAccept, "A + d-pad in one frame: accept only");
    }
    {  // two pads combined (ui_input ORs the buttons): pad 2's A while pad 1 holds down
        PadNav n;
        n.Reset(none);
        PadSnapshot s = Buttons(PadNav::kDpadDown);
        s.connected = 3;
        n.Update(s, 0.016f);
        s.buttons |= PadNav::kA;
        Check(n.Update(s, 0.016f) == PadNav::kAccept, "second pad's A while the first holds down");
    }
    {  // a pad unplugged while holding down: the combined state is empty, the repeat stops
        PadNav n;
        n.Reset(none);
        n.Update(Buttons(PadNav::kDpadDown), 0.016f);
        Check(Count(n, none, PadNav::kDown, 120) == 0, "unplugged while held: no more steps");
        Check(n.Update(Buttons(PadNav::kDpadDown), 0.016f) == PadNav::kDown, "plugged in and pressed: a step");
    }
    {  // switching direction while the old one is held: the new one repeats
        PadNav n;
        n.Reset(none);
        n.Update(Buttons(PadNav::kDpadDown), 0.016f);
        Check(n.Update(Buttons(PadNav::kDpadDown | PadNav::kDpadRight), 0.016f) == PadNav::kRight,
              "right while down is held: right");
        const int r = Count(n, Buttons(PadNav::kDpadDown | PadNav::kDpadRight), PadNav::kRight, 60, 0.0166f);
        Check(r >= 4, "right repeats", std::to_string(r));
    }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
