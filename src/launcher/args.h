#pragma once
// Launcher command-line parsing (launcher.cpp), kept apart so tests\launcher_args.cpp can call it directly.
//
//   --<key> <value>   keys in kValueKeys must have a value; a missing one, an empty one or one that looks like
//                     another flag ("--password --name x") is an error and the game is not started
//   --<key> [value]   any other key: the next argument is its value unless it starts with "--"; bare = "1"
//   +<word> [value]   passed to the game's command line unchanged (e.g. "+connect_lobby <id>" from Steam)
// Every key except --game and the launcher-only flags (check-only, skip-version-check, install, uninstall, yes) is
// passed to the dll as <envPrefix><KEY> ('-' and '.' become '_', upper case).
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace mc::launcher {

// Keys that take a value, as the dll and launcher read them ('-' written as '_'). Matched case-insensitively and
// with '-' or '_' (--join-lobby = --join_lobby).
inline constexpr const wchar_t* kValueKeys[] = {
    L"game",              // launcher: game exe (or folder for --install/--uninstall)
    L"join",              // ip[:port], or a lobby id with --transport steam
    L"join_lobby",        // Steam lobby id
    L"log",               // log tag
    L"model",             // character the player appears as
    L"name",              // player name
    L"password",          // session password
    L"port",              // UDP port
    L"transport",         // udp | steam
    L"window",            // left | right
    L"window_timeout_s",  // seconds to wait for the game window
};

struct LaunchArgs {
    std::vector<std::pair<std::wstring, std::wstring>> passEnv;  // <envPrefix><KEY> -> value, for the dll
    std::wstring gameOverride, logTag, gameArgs;
    bool checkOnly = false, skipVersionCheck = false, install = false, uninstall = false, assumeYes = false;
    std::wstring error;  // non-empty: refuse to start; show this
    // Errors go to the console instead of a message box (scripts / tests).
    bool NonInteractive() const { return checkOnly || assumeYes; }
};

namespace args_detail {
inline std::wstring Normalize(const std::wstring& key) {
    std::wstring k;
    for (wchar_t c : key) k += (c == L'-' || c == L'.') ? L'_' : static_cast<wchar_t>(towlower(c));
    return k;
}
inline bool IsFlag(const std::wstring& a) { return a.size() >= 2 && a[0] == L'-' && a[1] == L'-'; }
}  // namespace args_detail

inline bool NeedsValue(const std::wstring& key) {
    const std::wstring k = args_detail::Normalize(key);
    for (const wchar_t* v : kValueKeys)
        if (k == v) return true;
    return false;
}

// argv[0] is the program. quoteArg quotes one argument for the game's command line. The first error is kept;
// parsing goes on so --check-only / --yes later on the line still choose how it is shown.
template <class QuoteFn>
LaunchArgs ParseLaunchArgs(int argc, const wchar_t* const* argv, const std::wstring& envPrefix,
                           const std::wstring& launcherName, QuoteFn quoteArg) {
    using args_detail::IsFlag;
    LaunchArgs out;
    auto error = [&](const std::wstring& e) {
        if (out.error.empty()) out.error = e;
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a.size() > 1 && a[0] == L'+') {
            out.gameArgs += L" " + quoteArg(a);
            if (i + 1 < argc && argv[i + 1][0] != L'-' && argv[i + 1][0] != L'+') out.gameArgs += L" " + quoteArg(argv[++i]);
            continue;
        }
        if (!IsFlag(a) || a.size() < 3) {
            error(L"Unexpected argument: " + a + L"\n\nUsage: " + launcherName +
                  L" [--host] [--join <ip>] [--join-lobby <id>] [--name <name>] [--<key> [value]] [--game <exe>]");
            continue;
        }
        const std::wstring key = a.substr(2), norm = args_detail::Normalize(key);
        // launcher-only flags (not passed to the dll)
        if (norm == L"check_only") {
            out.checkOnly = true;
            continue;
        }
        if (norm == L"skip_version_check") {
            out.skipVersionCheck = true;
            continue;
        }
        if (norm == L"install" || norm == L"uninstall") {
            (norm == L"install" ? out.install : out.uninstall) = true;
            continue;
        }
        if (norm == L"yes") {
            out.assumeYes = true;
            continue;
        }
        std::wstring value = L"1";
        if (NeedsValue(key)) {
            const bool have = i + 1 < argc && !IsFlag(argv[i + 1]);
            if (!have || argv[i + 1][0] == L'\0') {
                const std::wstring next = i + 1 < argc ? std::wstring(argv[i + 1]) : std::wstring();
                error(L"--" + key + L" needs a value" +
                      (have ? L" (it is empty)." : next.empty() ? L"." : L", but the next argument is " + next + L".") +
                      L"\n\nExample: " + launcherName + L" --" + key + L" <value>");
                if (have) ++i;  // skip the empty value
                continue;
            }
            value = argv[++i];
        } else if (i + 1 < argc && !IsFlag(argv[i + 1])) {
            value = argv[++i];
        }
        if (norm == L"game") {
            out.gameOverride = value;
            continue;
        }
        if (norm == L"log") out.logTag = value;
        std::wstring envName = envPrefix;
        for (wchar_t c : key) envName += (c == L'-' || c == L'.') ? L'_' : static_cast<wchar_t>(towupper(c));
        out.passEnv.emplace_back(envName, value);
    }
    if (out.install && out.uninstall) error(L"--install and --uninstall cannot be combined.");
    return out;
}

}  // namespace mc::launcher
