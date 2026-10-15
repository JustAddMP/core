// The launcher (e.g. MafiaCoop.exe): finds the game described by mc/game_info.h through Steam, starts it suspended
// with SteamAppId set, injects the mod dll (from this exe's directory) and resumes it.
//
// Usage: <launcher> [--host] [--join <ip[:port]>] [--join-lobby <id>] [--name <name>] [--log <tag>]
//                   [--<key> [value]] [--game <exe path>] [+connect_lobby <id>]
//                   [--skip-version-check] [--check-only]
//        <launcher> --install | --uninstall [--game <exe or folder>] [--yes]
// Parsing is in args.h; keys that need a value (args.h kValueKeys) refuse to start without one.
// Every --<key> [value] (except --game and the launcher-only flags) is passed to the dll as environment variable
// <envPrefix><KEY>=<value> ("1" for a bare flag); core::Config gives these precedence over the ini.
// Before starting the game the CRC32 of its exe is checked against GameInfo::builds (cached in version-cache.txt);
// --check-only does only that, prints the result and exits with 0 (supported), 2 (unsupported) or 1 (error).
// --install / --uninstall add or remove the optional "start from Steam" files in the game folder (install.cpp);
// --yes skips their confirmation dialog and prints the result instead (scripts, tests).
#include "mc/core_version.h"
#include "core/names.h"
#include "launcher/args.h"
#include "launcher/install.h"
#include "mc/game_info.h"
#include "proxy/proxy_shared.h"

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mc::launcher {
namespace {

const GameInfo& Gi() { return GetGameInfo(); }
std::wstring ModName() { return core::AsciiW(Gi().modName); }
std::wstring AppId() { return std::to_wstring(Gi().steamAppId); }

FILE* _log = nullptr;
bool _console = false;  // --check-only: print instead of showing message boxes

// Writes a line to redirected stdout or the parent's console (--check-only; the launcher is a GUI exe).
void Print(const std::wstring& line) {
    static HANDLE out = [] {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return h;
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return INVALID_HANDLE_VALUE;
        return CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    }();
    if (out == INVALID_HANDLE_VALUE) return;
    const std::wstring w = line + L"\r\n";
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
        return;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
    WriteFile(out, u.data(), static_cast<DWORD>(u.size()), &written, nullptr);
}

void Log(const wchar_t* fmt, ...) {
    if (!_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(_log, L"%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(_log, fmt, args);
    va_end(args);
    fputwc(L'\n', _log);
    fflush(_log);
}

int Fail(const std::wstring& msg) {
    Log(L"ERROR: %s", msg.c_str());
    if (_console)
        Print(L"ERROR: " + msg);
    else
        MessageBoxW(nullptr, msg.c_str(), ModName().c_str(), MB_ICONERROR | MB_OK);
    return 1;
}

std::wstring LastErrorText(DWORD err = GetLastError()) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return L"error " + std::to_wstring(err) + (s.empty() ? L"" : L": " + s);
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    return s.substr(b, s.find_last_not_of(L" \t\r\n") - b + 1);
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring ReadTextFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return Widen(ss.str());
}

std::wstring NormalizePath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

// --- the ini (only the launcher's keys) --------------------------------------------------

std::wstring IniValue(const std::wstring& iniPath, const std::wstring& key) {
    std::wstring text = ReadTextFile(iniPath);
    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
        std::wstring t = Trim(line);
        if (t.empty() || t[0] == L'#' || t[0] == L';') continue;
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (Lower(Trim(t.substr(0, eq))) != key) continue;
        std::wstring v = Trim(t.substr(eq + 1));
        if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

// --- Steam library discovery -------------------------------------------------------------------

std::wstring RegString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// All "key" "value" pairs with the given key in a Valve KeyValues text file (values unescaped).
std::vector<std::wstring> VdfValues(const std::wstring& text, const std::wstring& key) {
    std::vector<std::wstring> out;
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = 0;
    std::wstring lowerText = Lower(text);
    while ((pos = lowerText.find(Lower(needle), pos)) != std::wstring::npos) {
        pos += needle.size();
        size_t open = text.find(L'"', pos);
        size_t nl = text.find(L'\n', pos);
        if (open == std::wstring::npos || (nl != std::wstring::npos && open > nl)) continue;  // it was a block key
        std::wstring value;
        size_t i = open + 1;
        for (; i < text.size() && text[i] != L'"'; ++i) {
            if (text[i] == L'\\' && i + 1 < text.size()) ++i;
            value += text[i];
        }
        out.push_back(value);
        pos = i;
    }
    return out;
}

std::wstring FindGameViaSteam() {
    std::vector<std::wstring> steamRoots;
    for (auto [root, sub, val] : {std::tuple{HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
                                  std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
                                  std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"}}) {
        std::wstring p = RegString(root, sub, val);
        if (!p.empty()) steamRoots.push_back(NormalizePath(p));
    }
    if (steamRoots.empty()) {
        Log(L"steam: no Steam install found in the registry");
        return {};
    }
    std::vector<std::wstring> libraries;
    for (const auto& root : steamRoots) {
        Log(L"steam: root %s", root.c_str());
        libraries.push_back(root);
        std::wstring vdf = ReadTextFile(root + L"\\steamapps\\libraryfolders.vdf");
        for (auto& lib : VdfValues(vdf, L"path")) libraries.push_back(NormalizePath(lib));
    }
    for (const auto& lib : libraries) {
        std::wstring manifest = lib + L"\\steamapps\\appmanifest_" + AppId() + L".acf";
        std::wstring acf = ReadTextFile(manifest);
        if (acf.empty()) continue;
        auto dirs = VdfValues(acf, L"installdir");
        std::wstring installDir = dirs.empty() ? std::wstring(Gi().steamInstallDir) : dirs.front();
        std::wstring exe = lib + L"\\steamapps\\common\\" + installDir + L"\\" + Gi().gameExe;
        Log(L"steam: manifest %s -> %s", manifest.c_str(), exe.c_str());
        if (FileExists(exe)) return exe;
    }
    Log(L"steam: app %s not found in %zu libraries", AppId().c_str(), libraries.size());
    return {};
}

// --- game version check (CRC32 of the exe) ------------------------------------------------------

// Standard CRC-32 (reflected polynomial 0xEDB88320, init and final xor 0xFFFFFFFF; the same value as zlib's
// crc32), slicing-by-8: eight input bytes per step through eight 256-entry tables.
struct Crc32Tables {
    uint32_t t[8][256];
    Crc32Tables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};

uint32_t Crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
    static const Crc32Tables tables;
    const auto& t = tables.t;
    crc = ~crc;
    while (n >= 8) {
        uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= crc;
        crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) crc = (crc >> 8) ^ t[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}

// CRC32 of a whole file, streamed in 4 MB blocks. False on any open/read error (GetLastError is kept).
bool FileCrc32(const std::wstring& path, uint32_t& crc) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> buf(4u << 20);
    crc = 0;
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        crc = Crc32Update(crc, buf.data(), got);
    }
    const DWORD err = GetLastError();
    CloseHandle(f);
    SetLastError(err);
    return ok;
}

struct FileStamp {
    unsigned long long size = 0, writeTime = 0;
};

bool GetFileStamp(const std::wstring& path, FileStamp& st) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    st.size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    st.writeTime = (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32) |
                   a.ftLastWriteTime.dwLowDateTime;
    return true;
}

// version-cache.txt next to the launcher: one "<size> <last write time> <crc32 hex> <exe path>" line per exe, so
// a repeat launch does not read the ~110 MB exe again. A different size or write time recomputes.
struct CacheLine {
    FileStamp stamp;
    uint32_t crc = 0;
    std::wstring path, text;
};

std::vector<CacheLine> ReadCache(const std::wstring& cachePath) {
    std::vector<CacheLine> lines;
    std::wistringstream in(ReadTextFile(cachePath));
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);  // BOM written by ccs=UTF-8
        line = Trim(line);
        CacheLine c;
        unsigned crc = 0;
        int pathAt = 0;
        if (swscanf_s(line.c_str(), L"%llu %llu %x %n", &c.stamp.size, &c.stamp.writeTime, &crc, &pathAt) != 3 ||
            pathAt <= 0 || static_cast<size_t>(pathAt) >= line.size())
            continue;
        c.crc = crc;
        c.path = line.substr(static_cast<size_t>(pathAt));
        c.text = line;
        lines.push_back(std::move(c));
    }
    return lines;
}

bool CachedCrc(const std::wstring& cachePath, const std::wstring& exe, const FileStamp& st, uint32_t& crc) {
    for (const auto& c : ReadCache(cachePath)) {
        if (_wcsicmp(c.path.c_str(), exe.c_str()) != 0) continue;
        if (c.stamp.size != st.size || c.stamp.writeTime != st.writeTime) return false;
        crc = c.crc;
        return true;
    }
    return false;
}

void StoreCrc(const std::wstring& cachePath, const std::wstring& exe, const FileStamp& st, uint32_t crc) {
    std::vector<std::wstring> keep;
    for (const auto& c : ReadCache(cachePath))
        if (_wcsicmp(c.path.c_str(), exe.c_str()) != 0) keep.push_back(c.text);
    if (keep.size() > 7) keep.erase(keep.begin(), keep.end() - 7);
    wchar_t head[80];
    swprintf_s(head, L"%llu %llu %08X ", st.size, st.writeTime, crc);
    keep.push_back(head + exe);
    FILE* f = nullptr;
    if (_wfopen_s(&f, cachePath.c_str(), L"w, ccs=UTF-8") != 0 || !f) {
        Log(L"version: cannot write %s", cachePath.c_str());
        return;
    }
    for (const auto& l : keep) fwprintf(f, L"%s\n", l.c_str());
    fclose(f);
}

const GameBuild* FindSupportedBuild(uint32_t crc) {
    for (size_t i = 0; i < Gi().buildCount; ++i)
        if (Gi().builds[i].crc32 == crc) return &Gi().builds[i];
    return nullptr;
}

std::wstring SupportedBuildsText() {
    std::wstring s;
    for (size_t i = 0; i < Gi().buildCount; ++i) {
        const GameBuild& b = Gi().builds[i];
        wchar_t line[160];
        swprintf_s(line, L"    %s (CRC %08X)\n", Widen(b.label).c_str(), b.crc32);
        s += line;
    }
    return s;
}

// --- process helpers ---------------------------------------------------------------------------

std::vector<DWORD> FindProcesses(const wchar_t* exeName) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) pids.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return pids;
}

// Current environment minus Steam/our (<envPrefix>*) vars, plus ours; sorted, double-NUL terminated.
std::vector<wchar_t> BuildEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<std::wstring> entries;
    const std::wstring ourPrefix = Lower(core::AsciiW(Gi().envPrefix));
    wchar_t* env = GetEnvironmentStringsW();
    for (const wchar_t* p = env; p && *p; p += wcslen(p) + 1) {
        std::wstring e = p;
        std::wstring name = Lower(e.substr(0, e.find(L'=', 1)));
        if (name == L"steamappid" || name == L"steamgameid" || name.rfind(ourPrefix, 0) == 0) continue;
        entries.push_back(e);
    }
    if (env) FreeEnvironmentStringsW(env);
    // A name given twice (e.g. "--name a --name b") must not end up twice in the block: which copy the game sees
    // would depend on the sort. The last one wins, as for the dll's own argument handling.
    for (size_t i = 0; i < extra.size(); ++i) {
        bool later = false;
        for (size_t j = i + 1; j < extra.size() && !later; ++j) later = _wcsicmp(extra[i].first.c_str(), extra[j].first.c_str()) == 0;
        if (!later) entries.push_back(extra[i].first + L"=" + extra[i].second);
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.substr(0, a.find(L'=', 1)).c_str(), b.substr(0, b.find(L'=', 1)).c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// One command-line argument, quoted for CommandLineToArgvW / the CRT if it needs it (spaces, tabs, quotes).
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');  // escape the backslashes before a quote, and the quote
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');  // backslashes before the closing quote
    q += L'"';
    return q;
}

// Loads dllPath into a process via LoadLibraryW on a new remote thread. Returns true if LoadLibraryW succeeded.
bool InjectRemoteThread(HANDLE process, void* remotePath, DWORD& err) {
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!t) {
        err = GetLastError();
        return false;
    }
    DWORD wait = WaitForSingleObject(t, 15000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    err = wait == WAIT_OBJECT_0 ? (code ? 0 : ERROR_MOD_NOT_FOUND) : WAIT_TIMEOUT;
    return wait == WAIT_OBJECT_0 && code != 0;  // exit code = low 32 bits of the HMODULE
}

} // namespace

int Run() {
    std::wstring selfDir(MAX_PATH, L'\0');
    for (;;) {  // long paths: MAX_PATH would silently truncate and point at a missing dll
        const DWORD n = GetModuleFileNameW(nullptr, selfDir.data(), static_cast<DWORD>(selfDir.size()));
        if (n == 0) return Fail(L"GetModuleFileName failed: " + LastErrorText());
        if (n < selfDir.size()) {
            selfDir.resize(n);
            break;
        }
        selfDir.resize(selfDir.size() * 2);
    }
    selfDir = selfDir.substr(0, selfDir.find_last_of(L'\\'));
    const std::wstring iniPath = selfDir + L"\\" + Gi().iniName;
    const std::wstring dllPath = selfDir + L"\\" + Gi().dllName;
    const std::wstring modName = ModName(), launcherName = Gi().launcherName, dllName = Gi().dllName;
    const std::wstring gameName = core::AsciiW(Gi().gameName), envPrefix = core::AsciiW(Gi().envPrefix);

    // ---- arguments (args.h) ----
    // --key [value] -> <envPrefix>KEY env var for the dll; +word args (e.g. "+connect_lobby <id>", which Steam
    // passes for "Join Game") go to the game's command line unchanged. Keys that need a value refuse to start without.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    LaunchArgs args = ParseLaunchArgs(argv ? argc : 0, argv, envPrefix, launcherName, QuoteArg);
    if (argv) LocalFree(argv);
    if (!args.error.empty()) {
        _console = args.NonInteractive();
        return Fail(args.error);
    }
    std::vector<std::pair<std::wstring, std::wstring>>& passEnv = args.passEnv;
    const std::wstring &gameOverride = args.gameOverride, &logTag = args.logTag, &gameArgs = args.gameArgs;
    const bool checkOnly = args.checkOnly, install = args.install, uninstall = args.uninstall,
               assumeYes = args.assumeYes;
    bool skipVersionCheck = args.skipVersionCheck;  // the ini can turn it on too
    _console = checkOnly || ((install || uninstall) && assumeYes);

    // ---- log: one per launch when several games run (launcher-<tag>.log / launcher-<pid>.log) ----
    const std::vector<DWORD> running = FindProcesses(Gi().gameExe);
    std::wstring logName = L"launcher.log";
    if (!logTag.empty())
        logName = L"launcher-" + logTag + L".log";
    else if (checkOnly)
        logName = L"launcher-check.log";
    else if (install || uninstall)
        logName = L"launcher-install.log";
    else if (!running.empty())
        logName = L"launcher-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    CreateDirectoryW((selfDir + L"\\logs").c_str(), nullptr);
    _wfopen_s(&_log, (selfDir + L"\\logs\\" + logName).c_str(), L"w, ccs=UTF-8");
    Log(L"%s %s launcher (core %s), built " __DATE__ " " __TIME__, modName.c_str(), core::AsciiW(Gi().modVersion).c_str(),
        core::AsciiW(kCoreVersion).c_str());
    for (auto& [k, v] : passEnv)  // the session password is never written to a log
        Log(L"arg: %s=%s", k.c_str(), k == envPrefix + L"PASSWORD" ? L"(hidden)" : v.c_str());
    if (!gameArgs.empty()) Log(L"arg: game command line extra:%s", gameArgs.c_str());

    // ---- find the game ----
    std::wstring gameExe;
    if (!gameOverride.empty()) {
        gameExe = NormalizePath(gameOverride);
        Log(L"game: from --game: %s", gameExe.c_str());
    } else {
        gameExe = FindGameViaSteam();
        if (gameExe.empty()) {
            std::wstring p = IniValue(iniPath, L"game_path");
            if (!p.empty()) {
                p = NormalizePath(p);
                gameExe = (Lower(p).size() > 4 && Lower(p).ends_with(L".exe")) ? p : p + L"\\" + Gi().gameExe;
                Log(L"game: from %s game_path: %s", Gi().iniName, gameExe.c_str());
            }
        }
    }
    if (gameExe.empty()) {
        return Fail(L"Could not find " + gameName + L" (Steam app " + AppId() + L").\n\nAdd a line\n"
                    L"    game_path = <folder containing " + Gi().gameExe + L">\nto " + iniPath);
    }
    if (install || uninstall) {  // --game may name the folder here
        const InstallIo io{[](const std::wstring& s) { Log(L"%s", s.c_str()); }, &Print, _console};
        return install ? InstallProxy(gameExe, selfDir, io) : UninstallProxy(gameExe, io);
    }
    if (!FileExists(gameExe)) return Fail(L"Game executable not found:\n" + gameExe);

    // ---- game version: CRC32 of the exe against the builds we support (GameInfo::builds) ----
    if (!skipVersionCheck) {
        const std::wstring v = Lower(IniValue(iniPath, L"skip_version_check"));
        skipVersionCheck = v == L"1" || v == L"true" || v == L"yes" || v == L"on";
    }
    if (skipVersionCheck && !checkOnly) {  // --check-only always checks
        Log(L"version: check skipped (--skip-version-check / skip_version_check = 1)");
    } else {
        const auto t0 = std::chrono::steady_clock::now();
        const std::wstring cachePath = selfDir + L"\\version-cache.txt";
        FileStamp st;
        uint32_t crc = 0;
        bool cached = false;
        if (!GetFileStamp(gameExe, st))
            return Fail(L"Cannot read the game executable:\n" + gameExe + L"\n\n" + LastErrorText());
        if (CachedCrc(cachePath, gameExe, st, crc)) {
            cached = true;
        } else {
            if (!FileCrc32(gameExe, crc))
                return Fail(L"Cannot read the game executable:\n" + gameExe + L"\n\n" + LastErrorText());
            StoreCrc(cachePath, gameExe, st, crc);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const GameBuild* build = FindSupportedBuild(crc);
        wchar_t crcText[16];
        swprintf_s(crcText, L"%08X", crc);
        const std::wstring verdict = build ? L"supported: " + Widen(build->label) : L"NOT SUPPORTED";
        Log(L"version: exe CRC32 %s (%u), %llu bytes, %s in %.1f ms -> %s", crcText, crc, st.size,
            cached ? L"cached" : L"computed", ms, verdict.c_str());
        const std::wstring ver = core::AsciiW(Gi().modVersion);
        if (checkOnly) {
            wchar_t line[200];
            swprintf_s(line, L"CRC32 %s (%u), %llu bytes, %s in %.1f ms", crcText, crc, st.size,
                       cached ? L"cached" : L"computed", ms);
            Print(modName + L" " + ver + L" version check: " + gameExe);
            Print(line);
            if (build) {
                Print(verdict);
                return 0;
            }
            Print(L"NOT SUPPORTED. Supported builds:\n" + SupportedBuildsText());
            return 2;
        }
        if (!build) {
            const std::wstring msg = L"This version of " + gameName + L" isn't supported yet by " + modName + L" " +
                                     ver + L".\n\nSupported:\n" + SupportedBuildsText() + L"Found: CRC " + crcText +
                                     L"\n" + gameExe + L"\n\nCheck for a newer " + modName +
                                     L", or start anyway (may crash).\n\nYes = start anyway, No = quit";
            if (MessageBoxW(nullptr, msg.c_str(), modName.c_str(), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
                Log(L"version: unsupported game build; quit");
                return 2;
            }
            Log(L"version: unsupported game build; starting anyway");
            passEnv.emplace_back(envPrefix + L"UNSUPPORTED_BUILD", crcText);  // the dll shows a banner
        }
    }
    if (!FileExists(dllPath)) return Fail(dllName + L" not found next to " + launcherName + L":\n" + dllPath);
    const std::wstring gameDir = gameExe.substr(0, gameExe.find_last_of(L'\\'));
    Log(L"game: %s", gameExe.c_str());

    // A second instance is allowed (two games on one PC for testing, joined over UDP 127.0.0.1); the game repo puts
    // what its exe needs for that into GameInfo::gameArgs (Mafia DE: /multiinst).
    if (!running.empty()) Log(L"note: %zu game instance(s) already running; starting another", running.size());

    // ---- start suspended ----
    passEnv.emplace_back(L"SteamAppId", AppId());
    passEnv.emplace_back(mc::proxy::EnvLauncher(), L"1");  // an installed version.dll proxy must not load a second copy
    passEnv.emplace_back(L"SteamGameId", AppId());
    std::vector<wchar_t> env = BuildEnvironment(passEnv);
    const std::wstring fixedArgs = Gi().gameArgs && *Gi().gameArgs ? L" " + std::wstring(Gi().gameArgs) : L"";
    std::wstring cmdLine = L"\"" + gameExe + L"\"" + fixedArgs + gameArgs;
    Log(L"command line: %s", cmdLine.c_str());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(gameExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(), gameDir.c_str(), &si, &pi)) {
        return Fail(L"Could not start the game:\n" + gameExe + L"\n\n" + LastErrorText());
    }
    Log(L"started pid %lu (suspended)", pi.dwProcessId);

    auto abortGame = [&](const std::wstring& why) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return Fail(why);
    };

    const std::wstring eventName = core::KernelName(L"Injected-" + std::to_wstring(pi.dwProcessId));
    HANDLE loaded = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!loaded) return abortGame(L"CreateEvent failed: " + LastErrorText());

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(pi.hProcess, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath || !WriteProcessMemory(pi.hProcess, remotePath, dllPath.c_str(), pathBytes, nullptr)) {
        return abortGame(L"Could not write into the game process: " + LastErrorText());
    }

    // ---- inject ----
    // Default: queue an APC to LoadLibraryW on the still-suspended main thread. The APC is delivered when the
    // loader finishes process initialization (after kernel32 and the exe's imports are loaded, before the exe's
    // entry point), so our hooks are in place before the game runs any code. kernel32 is mapped at the same
    // address in every process of a boot session, so our LoadLibraryW address is valid there.
    // Fallback (or inject_method=thread): CreateRemoteThread(LoadLibraryW) after resuming.
    std::wstring method = Lower(IniValue(iniPath, L"inject_method"));
    if (method.empty()) method = L"apc";
    Log(L"inject: method %s, dll %s", method.c_str(), dllPath.c_str());
    auto loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    bool apcQueued = false;
    if (method == L"apc") {
        if (!QueueUserAPC(reinterpret_cast<PAPCFUNC>(loadLibrary), pi.hThread, reinterpret_cast<ULONG_PTR>(remotePath))) {
            Log(L"inject: QueueUserAPC failed (%s), will use remote thread", LastErrorText().c_str());
            method = L"thread";
        } else {
            apcQueued = true;
        }
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) return abortGame(L"ResumeThread failed: " + LastErrorText());
    Log(L"resumed");

    HANDLE waits[2] = {loaded, pi.hProcess};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, method == L"apc" ? 15000 : 0);
    if (w == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        return Fail(L"The game exited during startup (exit code " + std::to_wstring(code) +
                    L") before " + dllName + L" was loaded.");
    }
    if (w != WAIT_OBJECT_0) {
        if (method == L"apc") Log(L"inject: APC did not load the dll within 15 s, falling back to remote thread");
        DWORD err = 0;
        if (!InjectRemoteThread(pi.hProcess, remotePath, err) ||
            WaitForSingleObject(loaded, 5000) != WAIT_OBJECT_0) {
            CloseHandle(loaded);
            return abortGame(L"Could not load " + dllName + L" into the game: " + LastErrorText(err));
        }
        method = L"thread";
    } else {
        apcQueued = false;  // the APC ran (it signalled): the path buffer is no longer needed
    }
    Log(L"inject: dll loaded (via %s)", method.c_str());
    CloseHandle(loaded);

    // ---- watch the first seconds: a quick exit usually means Steam restarted the game without us ----
    if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        Sleep(2000);
        bool relaunched = false;  // a game process that was not running before we started ours
        for (DWORD p : FindProcesses(Gi().gameExe))
            if (std::find(running.begin(), running.end(), p) == running.end()) relaunched = true;
        Log(L"game exited early with code %lu (relaunched by Steam: %s)", code, relaunched ? L"yes" : L"no");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return Fail(relaunched ? L"The game was restarted through Steam, so the co-op mod is not loaded. "
                                 L"Make sure Steam is running and try again."
                               : L"The game exited right after starting (exit code " + std::to_wstring(code) +
                                     L"). See logs\\" + Gi().logPrefix + L".log.");
    }
    Log(L"game running (pid %lu); launcher done", pi.dwProcessId);
    // An APC that has not run yet (remote-thread fallback) still points at the path: freeing it would make that
    // late LoadLibraryW read freed memory, so it stays (a few hundred bytes).
    if (!apcQueued) VirtualFreeEx(pi.hProcess, remotePath, 0, MEM_RELEASE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

} // namespace mc::launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int rc = mc::launcher::Run();
    if (mc::launcher::_log) fclose(mc::launcher::_log);
    return rc;
}
