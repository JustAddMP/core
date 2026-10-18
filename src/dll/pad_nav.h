#pragma once
// Gamepad navigation for our panels (pure logic, no XInput: tests/pad_nav.cpp drives it with made-up states).
//
// Input: the combined state of every connected pad (ui_input::ReadPads): buttons OR-ed, the left stick of the pad
// pushed furthest. Output per frame: the navigation actions that fire this frame.
//   - d-pad and left stick both move; the stick counts in its dominant axis only (a diagonal is not up AND right),
//     with hysteresis (pressed beyond kStickPress, released below kStickRelease) so a stick resting near the
//     threshold does not chatter;
//   - a held direction repeats (kFirstRepeatS, then every kRepeatS);
//   - A = accept, B / START / BACK = back; X, Y, LB, RB are reported as presses (no repeat) for the panels' extra
//     prompts (dll/ui_prompts.h), like accept/back never together with movement;
//   - only presses act: whatever is already held when a panel opens (the A that chose the menu entry, the stick
//     still pushed) does nothing until it is released (Reset with the current state).
#include <cstdint>
#include <cstdlib>

namespace mc {

struct PadSnapshot {
    uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
    int16_t lx = 0, ly = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    uint8_t connected = 0;  // bit per XInput user
};

class PadNav {
public:
    enum Action : uint32_t {
        kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kAccept = 16, kBack = 32,
        kX = 64, kY = 128, kLB = 256, kRB = 512
    };
    static constexpr uint32_t kButtonActions = kAccept | kBack | kX | kY | kLB | kRB;
    static constexpr int kStickPress = 16000;
    static constexpr int kStickRelease = 11000;
    static constexpr float kFirstRepeatS = 0.4f;
    static constexpr float kRepeatS = 0.15f;

    // XINPUT_GAMEPAD_* values (kept here so the header needs no xinput.h)
    static constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
    static constexpr uint16_t kStart = 0x0010, kBackButton = 0x0020, kA = 0x1000, kB = 0x2000;
    static constexpr uint16_t kXButton = 0x4000, kYButton = 0x8000, kLBButton = 0x0100, kRBButton = 0x0200;

    // What is held now does not act until released.
    void Reset(const PadSnapshot& s) {
        stick_ = 0;
        held_ = 0;
        stale_ = Held(s);
        repeatDir_ = 0;
        repeatT_ = 0;
    }

    uint32_t Update(const PadSnapshot& s, float dt) {
        const uint32_t raw = Held(s);
        stale_ &= raw;                       // held since Reset: ignored until released
        const uint32_t now = raw & ~stale_;
        const uint32_t pressed = now & ~held_;
        held_ = now;
        uint32_t out = pressed & kButtonActions;
        if (out) {  // a button press ends the frame: no movement with it
            repeatDir_ = 0;
            return out;
        }
        const uint32_t dirs = now & (kUp | kDown | kLeft | kRight);
        const uint32_t newDirs = pressed & (kUp | kDown | kLeft | kRight);
        if (newDirs) {
            // a new direction: act once now, then repeat the most recent one while it stays held
            out |= newDirs;
            repeatDir_ = Lowest(newDirs);
            repeatT_ = kFirstRepeatS;
        } else if (repeatDir_ && (dirs & repeatDir_)) {
            repeatT_ -= dt;
            if (repeatT_ <= 0) {
                out |= repeatDir_;
                repeatT_ += kRepeatS;
                if (repeatT_ <= 0) repeatT_ = kRepeatS;  // a long frame: one step, not a burst
            }
        } else {
            repeatDir_ = Lowest(dirs);  // the repeating one was let go while another is still held
            repeatT_ = kFirstRepeatS;
        }
        return out;
    }

private:
    static uint32_t Lowest(uint32_t bits) { return bits & (~bits + 1); }

    // Buttons and d-pad as actions, plus the left stick's direction (dominant axis, hysteresis).
    uint32_t Held(const PadSnapshot& s) {
        uint32_t a = 0;
        if (s.buttons & kDpadUp) a |= kUp;
        if (s.buttons & kDpadDown) a |= kDown;
        if (s.buttons & kDpadLeft) a |= kLeft;
        if (s.buttons & kDpadRight) a |= kRight;
        if (s.buttons & kA) a |= kAccept;
        if (s.buttons & (kB | kStart | kBackButton)) a |= kBack;
        if (s.buttons & kXButton) a |= kX;
        if (s.buttons & kYButton) a |= kY;
        if (s.buttons & kLBButton) a |= kLB;
        if (s.buttons & kRBButton) a |= kRB;
        const int ax = std::abs(static_cast<int>(s.lx)), ay = std::abs(static_cast<int>(s.ly));
        uint32_t stick = 0;
        // keep the current stick direction while its axis stays beyond the release threshold
        if (stick_ & (kUp | kDown)) {
            if (ay > kStickRelease && (s.ly > 0 ? kUp : kDown) == stick_) stick = stick_;
        } else if (stick_ & (kLeft | kRight)) {
            if (ax > kStickRelease && (s.lx > 0 ? kRight : kLeft) == stick_) stick = stick_;
        }
        if (!stick && (ax > kStickPress || ay > kStickPress))
            stick = ay >= ax ? (s.ly > 0 ? kUp : kDown) : (s.lx > 0 ? kRight : kLeft);
        stick_ = stick;
        return a | stick;
    }

    uint32_t stick_ = 0;      // the stick's direction (one of kUp..kRight, or 0)
    uint32_t held_ = 0;       // actions held last frame (without stale_)
    uint32_t stale_ = 0;      // held when the panel opened, not released since
    uint32_t repeatDir_ = 0;  // the direction that repeats while held
    float repeatT_ = 0;
};

}  // namespace mc
