#pragma once
// Pad buttons held per XInput user, for ui_input's release guard (dll/ui_input.h): after one of our panels closes,
// the game's input stays blocked until the buttons that closed it are released. Header-only so tests/pad_nav.cpp can
// check it without a game.
//
// Each user's buttons are kept with the time they were read. The game's own XInput reads (and our panels') keep them
// fresh while somebody polls, but a game that stops polling the pad (a loading screen that only waits for Enter)
// would leave the button that closed the panel "held" for ever, and the guard would block the keyboard with it. So
// the guard reads a state older than kStaleMs again itself (only users that hold a button: reading an empty slot is
// slow, and it holds nothing), and a pad that cannot be read any more counts as released.
#include <atomic>
#include <cstdint>

namespace mc {

class PadHold {
public:
    static constexpr int kUsers = 4;           // XUSER_MAX_COUNT
    static constexpr uint64_t kStaleMs = 100;  // older than this: read the pad again before trusting it

    // A read of `user`'s pad at nowMs (0 buttons when the read failed: unplugged).
    void Set(int user, uint16_t buttons, uint64_t nowMs) {
        if (user < 0 || user >= kUsers) return;
        _buttons[user] = buttons;
        _at[user] = nowMs;
    }
    uint16_t Buttons(int user) const { return user >= 0 && user < kUsers ? _buttons[user].load() : uint16_t{0}; }

    // True if any user holds a button of `mask`. `read(int user, uint16_t* buttons) -> bool` reads that user's pad
    // now (false: no pad / the read failed, which counts as released); it is called only for a user that holds a
    // button of `mask` and whose state is older than kStaleMs. Any thread.
    template <class Read>
    bool AnyHeld(uint16_t mask, uint64_t nowMs, Read&& read) {
        bool held = false;
        for (int u = 0; u < kUsers; ++u) {
            uint16_t b = _buttons[u].load();
            if (!(b & mask)) continue;
            const uint64_t at = _at[u].load();
            if (nowMs > at && nowMs - at > kStaleMs) {
                uint16_t fresh = 0;
                if (!read(u, &fresh)) fresh = 0;
                Set(u, fresh, nowMs);
                b = fresh;
            }
            if (b & mask) held = true;
        }
        return held;
    }

private:
    std::atomic<uint16_t> _buttons[kUsers] = {};
    std::atomic<uint64_t> _at[kUsers] = {};
};

}  // namespace mc
