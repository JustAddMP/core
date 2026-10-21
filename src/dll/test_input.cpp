#include "dll/test_input.h"

#include "core/config.h"
#include "core/log.h"
#include "dll/coop_ui.h"
#include "dll/test_input_script.h"
#include "dll/ui_input.h"

#include <windows.h>

#include <deque>
#include <fstream>
#include <sstream>
#include <string>

namespace mc {
namespace {

constexpr double kPollS = 0.2;

struct State {
    bool enabled = false;
    std::wstring file;
    double poll = 0;
    double waitLeft = 0;
    std::deque<test_input::Command> queue;
};

State& S() {
    static State* s = new State;
    return *s;
}

void Run(const test_input::Command& c) {
    State& s = S();
    using C = test_input::Command;
    switch (c.type) {
    case C::Pad: {
        ui_input::TestPadState p;
        p.buttons = c.buttons;
        const bool ok = ui_input::SetTestPad(p, c.ms);
        MC_LOG_INFO("test_input: pad 0x%04x for %u ms%s", c.buttons, c.ms, ok ? "" : " (test pad off)");
        s.waitLeft = c.ms / 1000.0 + 0.1;  // released before the next command
        break;
    }
    case C::Key:
        ui_input::InjectEvent({ui_input::Event::Key, c.code, 0, 0});
        MC_LOG_INFO("test_input: key 0x%02x", c.code);
        break;
    case C::Move:
        ui_input::InjectEvent({ui_input::Event::MouseMove, 0, c.x, c.y});
        MC_LOG_INFO("test_input: mouse move to %.0f, %.0f", c.x, c.y);
        break;
    case C::Click:
        ui_input::InjectEvent({ui_input::Event::MouseMove, 0, c.x, c.y});
        ui_input::InjectEvent({ui_input::Event::MouseDown, 0, c.x, c.y});
        MC_LOG_INFO("test_input: click at %.0f, %.0f", c.x, c.y);
        break;
    case C::Wheel:
        ui_input::InjectEvent({ui_input::Event::Wheel, c.code, 0, 0});
        MC_LOG_INFO("test_input: wheel %s", c.code == 1 ? "up" : "down");
        break;
    case C::Wait:
        s.waitLeft = c.ms / 1000.0;
        break;
    case C::Text:
        for (char ch : c.text) ui_input::InjectEvent({ui_input::Event::Char, static_cast<uint32_t>(ch), 0, 0});
        MC_LOG_INFO("test_input: text '%s'", c.text.c_str());
        break;
    case C::Dump: {
        std::stringstream ss(CoopUi::Describe());
        std::string line;
        while (std::getline(ss, line)) MC_LOG_INFO("test_input: %s", line.c_str());
        break;
    }
    case C::Log:
        MC_LOG_INFO("test_input: %s", c.text.c_str());
        break;
    case C::Open:
        MC_LOG_INFO("test_input: open %s: %s", c.text.c_str(), CoopUi::OpenForTest(c.text) ? "ok" : "unknown");
        s.waitLeft = 0.3;  // let it open (and build its rows) before the next command
        break;
    }
}

void Poll() {
    State& s = S();
    std::ifstream f(s.file, std::ios::binary);
    if (!f) return;
    std::ostringstream text;
    text << f.rdbuf();
    f.close();
    DeleteFileW(s.file.c_str());
    std::vector<std::string> errors;
    const auto cmds = test_input::Parse(text.str(), &errors);
    for (const auto& e : errors) MC_LOG_WARN("test_input: %s", e.c_str());
    MC_LOG_INFO("test_input: script with %zu command(s)", cmds.size());
    for (const auto& c : cmds) s.queue.push_back(c);
}

}  // namespace

void TestInput::Init(const std::wstring& dllDir, const std::string& tag) {
    State& s = S();
    s.enabled = core::Config::Get().GetBool("test_input", false);
    if (!s.enabled) return;
    std::wstring wtag(tag.begin(), tag.end());
    if (wtag.empty()) wtag = L"default";
    CreateDirectoryW((dllDir + L"\\dev").c_str(), nullptr);
    s.file = dllDir + L"\\dev\\" + wtag + L".input.txt";
    ui_input::EnableTestPad(true);
    MC_LOG_INFO("test_input: on (scripts from dev\\%s.input.txt; the test pad is on)", tag.empty() ? "default" : tag.c_str());
}

bool TestInput::Enabled() { return S().enabled; }

void TestInput::Tick(float dt) {
    State& s = S();
    if (!s.enabled) return;
    s.poll -= dt;
    if (s.poll <= 0) {
        s.poll = kPollS;
        Poll();
    }
    if (s.waitLeft > 0) {
        s.waitLeft -= dt;
        return;
    }
    while (!s.queue.empty() && s.waitLeft <= 0) {
        const test_input::Command c = s.queue.front();
        s.queue.pop_front();
        Run(c);
    }
}

}  // namespace mc
