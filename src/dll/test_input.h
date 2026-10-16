#pragma once
// Test input for automated tests of our panels (core 0.7). Scripts in the game run on its script thread, which stops
// while the pause menu is open, so a test cannot press the pad from Lua inside our pause-menu panels. With
// `test_input = 1` (ini) / `--test_input 1` the dll polls `<dll dir>\dev\<log tag>.input.txt` (tag "default" without
// one) on the render thread, which keeps running in menus; a file there is read, deleted and run line by line
// (commands in dll/test_input_script.h): virtual pad presses (the test pad, dll/ui_input.h), mouse moves and clicks
// and keys handed straight to our panels' input queue, waits, and a dump of the open panel into the log. It turns
// the test pad on too. Off by default; never touches the game's own input except through the test pad.
#include <string>

namespace mc {

class TestInput {
public:
    static void Init(const std::wstring& dllDir, const std::string& tag);  // reads the test_input setting
    static void Tick(float dt);                                             // render thread, every frame
    static bool Enabled();
};

}  // namespace mc
