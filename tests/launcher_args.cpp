// launcher_args: the launcher's command-line parsing (src/launcher/args.h). Keys that need a value refuse a missing,
// empty or flag-looking value; bare flags stay bare. Also runs out\mc_test_launcher.exe --check-only with a missing
// value and checks it prints the error and exits 1 without going further. Exits non-zero on any failure.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "launcher/args.h"

using namespace mc::launcher;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

std::wstring Quote(const std::wstring& a) { return a.find(L' ') == std::wstring::npos ? a : L"\"" + a + L"\""; }

LaunchArgs Parse(std::vector<std::wstring> words) {
    std::vector<const wchar_t*> argv{L"launcher.exe"};
    for (auto& w : words) argv.push_back(w.c_str());
    return ParseLaunchArgs(static_cast<int>(argv.size()), argv.data(), L"MCTEST_", L"launcher.exe", Quote);
}

std::wstring Env(const LaunchArgs& a, const std::wstring& name) {
    std::wstring v = L"<unset>";
    for (auto& [k, val] : a.passEnv)
        if (k == name) v = val;
    return v;
}

// Must fail, and the message must name the key.
void Refused(std::vector<std::wstring> words, const wchar_t* key, const char* name) {
    const LaunchArgs a = Parse(words);
    Check(!a.error.empty() && a.error.find(key) != std::wstring::npos, name,
          "\"" + Narrow(a.error.substr(0, a.error.find(L'\n'))) + "\"");
}

void ParserChecks() {
    Refused({L"--password", L"--name", L"x"}, L"--password", "--password --name x: refused");
    Refused({L"--host", L"--password"}, L"--password", "--password at the end: refused");
    Refused({L"--name", L""}, L"--name", "--name \"\": refused (empty)");
    Refused({L"--join", L"--host"}, L"--join", "--join --host: refused");
    Refused({L"--join-lobby"}, L"--join-lobby", "--join-lobby without id: refused");
    Refused({L"--JOIN_LOBBY", L"--x"}, L"--JOIN_LOBBY", "--JOIN_LOBBY (case, underscore): refused");
    Refused({L"--port", L"--log", L"a"}, L"--port", "--port --log a: refused");
    Refused({L"--log"}, L"--log", "--log without tag: refused");
    Refused({L"--game", L"--check-only"}, L"--game", "--game --check-only: refused");
    Refused({L"--window"}, L"--window", "--window without side: refused");
    Refused({L"--transport", L"--host"}, L"--transport", "--transport without name: refused");
    Refused({L"--model"}, L"--model", "--model without name: refused");
    Refused({L"--window-timeout-s"}, L"--window-timeout-s", "--window-timeout-s without seconds: refused");
    Refused({L"stray"}, L"Unexpected argument", "stray word: refused");
    Refused({L"--install", L"--uninstall"}, L"cannot be combined", "--install --uninstall: refused");

    // every key in the table refuses a missing value
    int tableOk = 0, tableN = 0;
    for (const wchar_t* k : kValueKeys) {
        ++tableN;
        std::wstring flag = L"--" + std::wstring(k);
        tableOk += !Parse({flag}).error.empty() && !Parse({flag, L"--host"}).error.empty() &&
                   Parse({flag, L"v"}).error.empty();
    }
    Check(tableOk == tableN, "every kValueKeys entry needs a value", std::to_string(tableOk) + "/" + std::to_string(tableN));

    // values and bare flags
    {
        const LaunchArgs a = Parse({L"--host", L"--name", L"Alice Smith", L"--password", L"-starts-with-dash",
                                    L"--debug-overlay", L"--port", L"27961", L"--join_lobby", L"123", L"--log", L"h"});
        const bool ok = a.error.empty() && Env(a, L"MCTEST_HOST") == L"1" && Env(a, L"MCTEST_NAME") == L"Alice Smith" &&
                        Env(a, L"MCTEST_PASSWORD") == L"-starts-with-dash" && Env(a, L"MCTEST_DEBUG_OVERLAY") == L"1" &&
                        Env(a, L"MCTEST_PORT") == L"27961" && Env(a, L"MCTEST_JOIN_LOBBY") == L"123" && a.logTag == L"h";
        Check(ok, "values and bare flags parsed", Narrow(a.error));
    }
    {
        const LaunchArgs a = Parse({L"--check-only", L"--skip-version-check", L"--yes", L"--install", L"--game",
                                    L"C:\\Games\\X", L"--friendly-fire", L"0", L"--foo", L"--bar"});
        const bool ok = a.error.empty() && a.checkOnly && a.skipVersionCheck && a.assumeYes && a.install &&
                        a.gameOverride == L"C:\\Games\\X" && Env(a, L"MCTEST_FRIENDLY_FIRE") == L"0" &&
                        Env(a, L"MCTEST_FOO") == L"1" && Env(a, L"MCTEST_BAR") == L"1" &&
                        Env(a, L"MCTEST_GAME") == L"<unset>" && Env(a, L"MCTEST_CHECK_ONLY") == L"<unset>";
        Check(ok, "launcher flags, --game, other keys stay optional", Narrow(a.error));
    }
    {
        const LaunchArgs a = Parse({L"+connect_lobby", L"109775241234567890", L"--host"});
        Check(a.error.empty() && a.gameArgs == L" +connect_lobby 109775241234567890" && Env(a, L"MCTEST_HOST") == L"1",
              "+connect_lobby goes to the game", Narrow(a.gameArgs));
    }
    {
        const LaunchArgs a = Parse({L"--password", L"--check-only"});
        const LaunchArgs b = Parse({L"--password"});
        Check(!a.error.empty() && a.NonInteractive() && !b.NonInteractive(),
              "--check-only after the error still makes it printed");
    }
}

// The real launcher (built for the made-up test game) refuses before doing anything and prints the error.
void LauncherExe() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    const std::wstring exe = dir + L"\\mc_test_launcher.exe";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        Check(false, "launcher exe: pipe");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" --check-only --password --name x";
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        Check(false, "launcher exe: start", "CreateProcess failed");
        return;
    }
    std::string out;
    char buf[512];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    const bool exited = WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (!exited) TerminateProcess(pi.hProcess, 99);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::string first = out.substr(0, out.find_first_of("\r\n"));
    Check(exited && code == 1 && out.find("--password needs a value") != std::string::npos &&
              out.find("CRC") == std::string::npos,
          "mc_test_launcher --check-only --password --name x", "exit " + std::to_string(code) + ", \"" + first + "\"");
}

}  // namespace

int main() {
    ParserChecks();
    LauncherExe();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
