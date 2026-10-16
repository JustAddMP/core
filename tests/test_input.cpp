// test_input: the test input script parser (src/dll/test_input_script.h) that drives virtual pad presses, mouse and
// keys into the co-op panels from a file while the game's scripts are paused. Exits non-zero on any failure.
#include <cstdio>
#include <string>
#include <vector>

#include "dll/test_input_script.h"

using mc::test_input::Command;
using mc::test_input::Parse;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    // a full script: every command, comments, blank lines, CRLF
    const std::string script =
        "# open the pause CO-OP panel first\r\n"
        "pad DOWN\r\n"
        "pad A+B 300\r\n"
        "pad 0x1000 50\n"
        "\n"
        "key 0x28   # down\n"
        "move 640 400\n"
        "click 641.5 420\n"
        "wheel -1\n"
        "wait 250\n"
        "dump\n"
        "log after the clicks\n"
        "open settings\n";
    std::vector<std::string> errors;
    const auto c = Parse(script, &errors);
    Check(errors.empty() && c.size() == 11, "script: 11 commands, no errors", std::to_string(c.size()));
    if (c.size() == 11) {
        Check(c[10].type == Command::Open && c[10].text == "settings", "open settings");
        Check(c[0].type == Command::Pad && c[0].buttons == 0x0002 && c[0].ms == 150, "pad DOWN: d-pad down, 150 ms");
        Check(c[1].type == Command::Pad && c[1].buttons == 0x3000 && c[1].ms == 300, "pad A+B 300");
        Check(c[2].type == Command::Pad && c[2].buttons == 0x1000 && c[2].ms == 50, "pad hex");
        Check(c[3].type == Command::Key && c[3].code == 0x28, "key 0x28 (comment after it)");
        Check(c[4].type == Command::Move && c[4].x == 640 && c[4].y == 400, "move");
        Check(c[5].type == Command::Click && c[5].x == 641.5f && c[5].y == 420, "click with a fraction");
        Check(c[6].type == Command::Wheel && c[6].code == 0xFFFFFFFFu, "wheel -1");
        Check(c[7].type == Command::Wait && c[7].ms == 250, "wait");
        Check(c[8].type == Command::Dump, "dump");
        Check(c[9].type == Command::Log && c[9].text == "after the clicks", "log text");
    }

    // bad lines are skipped with a reason; the good ones around them stay
    errors.clear();
    const auto b = Parse("pad Z\npad A 0\nkey\nkey 0x1FF\nmove 10\nclick -5 3\nwheel 2\nwait -1\njump 3\nopen menu\npad UP 100\n",
                         &errors);
    Check(b.size() == 1 && b[0].type == Command::Pad && b[0].buttons == 0x0001 && errors.size() == 10,
          "bad lines skipped and reported", std::to_string(errors.size()) + " errors");
    Check(!errors.empty() && errors[0].rfind("line 1:", 0) == 0, "errors name the line", errors.empty() ? "" : errors[0]);

    // every button name
    uint16_t all = 0;
    Check(mc::test_input::ParseButtons("UP+DOWN+LEFT+RIGHT+START+BACK+LS+RS+LB+RB+A+B+X+Y", &all) && all == 0xF3FF,
          "all button names", std::to_string(all));
    uint16_t none = 0;
    Check(!mc::test_input::ParseButtons("a", &none) && !mc::test_input::ParseButtons("", &none) &&
              !mc::test_input::ParseButtons("0x10000", &none),
          "lower case, empty and out-of-range rejected");

    std::printf(g_failures ? "FAILED: %d check(s)\n" : "ALL PASSED (0 failures)\n", g_failures);
    return g_failures ? 1 : 0;
}
