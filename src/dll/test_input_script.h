#pragma once
// Test input script (dll/test_input.h): the parser, header-only so tests/test_input.cpp can check it without a game.
//
// One command per line; '#' starts a comment; blank lines are skipped. Coordinates are backbuffer pixels (what the
// overlay draws in); times are milliseconds.
//   pad <buttons> [ms]   hold virtual pad buttons (XInput) for ms (default 150), then release
//                        buttons: names joined with '+' (A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS) or hex
//   key <vk>             a key press for our panels (VK_* in hex, e.g. 0x28 = Down, 0x0D = Enter, 0x1B = Esc)
//   move <x> <y>         mouse move to (x, y)
//   click <x> <y>        left click at (x, y) (a move there first)
//   wheel <+1|-1>        mouse wheel
//   wait <ms>            pause before the next command
//   open <panel>         open one of our panels as its menu entry does: pause, lobby, settings, joinip
//   dump                 log the open panel: its rows, their hit boxes and the selected row
//   log <text>           a line in the log
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace mc::test_input {

struct Command {
    enum Type { Pad, Key, Move, Click, Wheel, Wait, Dump, Log, Open } type = Log;
    uint16_t buttons = 0;  // Pad
    uint32_t ms = 0;       // Pad: hold time; Wait: pause
    uint32_t code = 0;     // Key: virtual key; Wheel: 1 = up, 0xFFFFFFFF = down
    float x = 0, y = 0;    // Move, Click
    std::string text;      // Log: the text; Open: the panel
};

inline bool ParseButtons(const std::string& s, uint16_t* out) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s.c_str() + 2, &end, 16);
        if (!end || *end || v > 0xFFFF) return false;
        *out = static_cast<uint16_t>(v);
        return true;
    }
    struct Name {
        const char* name;
        uint16_t bit;
    };
    static const Name kNames[] = {{"UP", 0x0001},    {"DOWN", 0x0002},  {"LEFT", 0x0004}, {"RIGHT", 0x0008},
                                  {"START", 0x0010}, {"BACK", 0x0020},  {"LS", 0x0040},   {"RS", 0x0080},
                                  {"LB", 0x0100},    {"RB", 0x0200},    {"A", 0x1000},    {"B", 0x2000},
                                  {"X", 0x4000},     {"Y", 0x8000}};
    uint16_t bits = 0;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, '+')) {
        bool found = false;
        for (const Name& n : kNames) {
            if (part == n.name) {
                bits |= n.bit;
                found = true;
            }
        }
        if (!found) return false;
    }
    if (!bits) return false;
    *out = bits;
    return true;
}

// Parses a script. Bad lines are skipped and reported in `errors` ("line N: ...").
inline std::vector<Command> Parse(const std::string& text, std::vector<std::string>* errors = nullptr) {
    std::vector<Command> out;
    std::stringstream in(text);
    std::string line;
    int n = 0;
    auto bad = [&](const std::string& why) {
        if (errors) errors->push_back("line " + std::to_string(n) + ": " + why);
    };
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t hash = line.find('#');
        std::string body = line.substr(0, hash);
        std::stringstream ls(body);
        std::string cmd;
        if (!(ls >> cmd)) continue;
        Command c;
        if (cmd == "pad") {
            std::string b;
            c.type = Command::Pad;
            c.ms = 150;
            if (!(ls >> b) || !ParseButtons(b, &c.buttons)) {
                bad("pad: unknown buttons");
                continue;
            }
            long ms = 0;
            if (ls >> ms) {
                if (ms <= 0 || ms > 60000) {
                    bad("pad: bad time");
                    continue;
                }
                c.ms = static_cast<uint32_t>(ms);
            }
        } else if (cmd == "key") {
            std::string v;
            c.type = Command::Key;
            if (!(ls >> v)) {
                bad("key: missing code");
                continue;
            }
            char* end = nullptr;
            const unsigned long k = std::strtoul(v.c_str(), &end, 0);
            if (!end || *end || k == 0 || k > 0xFF) {
                bad("key: bad code");
                continue;
            }
            c.code = static_cast<uint32_t>(k);
        } else if (cmd == "move" || cmd == "click") {
            c.type = cmd == "move" ? Command::Move : Command::Click;
            if (!(ls >> c.x >> c.y) || c.x < 0 || c.y < 0 || c.x > 20000 || c.y > 20000) {
                bad(cmd + ": bad coordinates");
                continue;
            }
        } else if (cmd == "wheel") {
            int d = 0;
            c.type = Command::Wheel;
            if (!(ls >> d) || (d != 1 && d != -1)) {
                bad("wheel: +1 or -1");
                continue;
            }
            c.code = d > 0 ? 1u : 0xFFFFFFFFu;
        } else if (cmd == "wait") {
            long ms = 0;
            c.type = Command::Wait;
            if (!(ls >> ms) || ms < 0 || ms > 600000) {
                bad("wait: bad time");
                continue;
            }
            c.ms = static_cast<uint32_t>(ms);
        } else if (cmd == "open") {
            c.type = Command::Open;
            if (!(ls >> c.text) || (c.text != "pause" && c.text != "lobby" && c.text != "settings" && c.text != "joinip")) {
                bad("open: pause, lobby, settings or joinip");
                continue;
            }
        } else if (cmd == "dump") {
            c.type = Command::Dump;
        } else if (cmd == "log") {
            c.type = Command::Log;
            std::getline(ls, c.text);
            while (!c.text.empty() && c.text.front() == ' ') c.text.erase(0, 1);
        } else {
            bad("unknown command '" + cmd + "'");
            continue;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace mc::test_input
