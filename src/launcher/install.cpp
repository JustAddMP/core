// Launcher --install / --uninstall (see install.h and src/proxy/proxy_shared.h).
#include "launcher/install.h"

#include "core/names.h"
#include "proxy/proxy_shared.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace mc::launcher {
namespace {

using namespace mc::proxy;

constexpr int kOk = 0, kError = 1, kRefused = 3;

// Descriptor strings for messages.
std::wstring Mod() { return core::AsciiW(GetGameInfo().modName); }
std::wstring Launcher() { return GetGameInfo().launcherName; }
std::wstring DllName() { return GetGameInfo().dllName; }

bool IsFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<char> ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::vector<char>(std::istreambuf_iterator<char>(in), {}) : std::vector<char>{};
}

bool Contains(const std::vector<char>& data, const char* needle) {
    const size_t n = std::strlen(needle);
    if (data.size() < n) return false;
    for (size_t i = 0; i + n <= data.size(); ++i)
        if (data[i] == needle[0] && std::memcmp(data.data() + i, needle, n) == 0) return true;
    return false;
}

bool IsOurProxy(const std::wstring& path) {
    return Contains(ReadAll(path), Marker());
}

bool IsOurPointer(const std::wstring& path) {
    const std::vector<char> d = ReadAll(path);
    const std::string header = PointerHeader();
    return d.size() >= header.size() && std::memcmp(d.data(), header.data(), header.size()) == 0;
}

std::wstring ErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return L"error " + std::to_wstring(err) + (s.empty() ? L"" : L": " + s);
}

std::wstring GameDir(const std::wstring& exeOrDir) {
    std::wstring p = exeOrDir;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    const size_t slash = p.find_last_of(L'\\');
    const std::wstring last = slash == std::wstring::npos ? p : p.substr(slash + 1);
    if (_wcsicmp(last.c_str(), GameExe()) == 0) p = slash == std::wstring::npos ? L"." : p.substr(0, slash);
    return p;
}

// A running game exe started from this folder (it would have version.dll loaded).
bool GameRunningFrom(const std::wstring& dir) {
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, GameExe()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[1024];
        DWORD n = 1024;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) found = _wcsicmp(GameDir(path).c_str(), dir.c_str()) == 0;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

struct Reporter {
    const InstallIo& io;
    void Line(const std::wstring& s) const {
        io.log(s);
        if (io.console) io.print(s);
    }
    int Error(const std::wstring& s, int code = kError) const {
        io.log(L"ERROR: " + s);
        if (io.console)
            io.print(L"ERROR: " + s);
        else
            MessageBoxW(nullptr, s.c_str(), Mod().c_str(), (code == kRefused ? MB_ICONWARNING : MB_ICONERROR) | MB_OK);
        return code;
    }
    void Done(const std::wstring& s) const {
        Line(s);
        if (!io.console) MessageBoxW(nullptr, s.c_str(), Mod().c_str(), MB_ICONINFORMATION | MB_OK);
    }
    bool Confirm(const std::wstring& s) const {
        io.log(L"confirm: " + s);
        if (io.console) return true;  // --yes
        return MessageBoxW(nullptr, s.c_str(), Mod().c_str(), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES;
    }
};

} // namespace

int InstallProxy(const std::wstring& gameExeOrDir, const std::wstring& selfDir, const InstallIo& io) {
    const Reporter r{io};
    const std::wstring dir = GameDir(gameExeOrDir);
    const std::wstring gameExe = dir + L"\\" + GameExe();
    const std::wstring proxySrc = selfDir + L"\\" + BuiltName();
    const std::wstring dll = selfDir + L"\\" + DllName();
    const std::wstring target = dir + L"\\" + kInstalledName;
    const std::wstring pointer = dir + L"\\" + PointerName();
    const std::wstring mod = Mod(), launcher = Launcher();
    r.Line(L"install: game folder " + dir);

    if (!IsFile(gameExe)) return r.Error(L"Not a game folder (no " + std::wstring(GameExe()) + L"):\n" + dir);
    if (!IsFile(proxySrc)) return r.Error(L"Missing " + proxySrc + L" (it ships next to " + launcher + L").");
    if (!IsFile(dll)) return r.Error(L"Missing " + dll + L".");

    const bool update = IsFile(target) && IsOurProxy(target);
    if (Exists(target) && !update) {
        return r.Error(L"The game folder already has a " + std::wstring(kInstalledName) +
                           L" that is not from " + mod + L" (probably another mod):\n" + target + L"\n\n" + mod +
                           L" did not change anything. Remove the other mod first, or keep starting the game with " +
                           launcher + L".",
                       kRefused);
    }
    if (Exists(pointer) && !(IsFile(pointer) && IsOurPointer(pointer))) {
        return r.Error(L"The game folder already has a " + std::wstring(PointerName()) + L" that is not from " + mod +
                           L":\n" + pointer + L"\n\n" + mod + L" did not change anything.",
                       kRefused);
    }
    if (GameRunningFrom(dir)) return r.Error(L"The game is running. Close it and try again.", kRefused);

    const std::wstring question =
        std::wstring(update ? L"Update" : L"Install") +
        L" \"start from Steam\" mode?\n\n"
        L"This writes two files into the game folder:\n\n    " + target +
        L"\n        a small loader. The game loads it at start; it passes everything on to Windows' own version.dll "
        L"and loads " + mod + L".\n\n    " + pointer + L"\n        one line: where " + mod + L" is:\n        " + dll +
        L"\n\nNo other game file is changed. Afterwards the game started from Steam (or its exe) has co-op; keep this " +
        mod + L" folder where it is (or run --install again after moving it). Settings come from " +
        GetGameInfo().iniName + L" next to " + DllName() + L".\n\nTo remove: run " + launcher +
        L" --uninstall, or delete those two files. Steam's \"Verify integrity of game files\" does not remove them."
        L"\n\nContinue?";
    if (!r.Confirm(question)) {
        r.Line(L"install: cancelled");
        return kRefused;
    }

    if (!CopyFileW(proxySrc.c_str(), target.c_str(), FALSE)) {
        const DWORD err = GetLastError();
        return r.Error(L"Could not write " + target + L":\n" + ErrorText(err) +
                       (err == ERROR_ACCESS_DENIED ? L"\n\nThe game folder may need administrator rights, or the game "
                                                     L"is still running."
                                                   : L""));
    }
    // UTF-8, header line first (it marks the file as ours), then the path.
    auto narrow = [](const std::wstring& w) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr,
                                            nullptr);
        std::string s(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    };
    std::string body = PointerHeader() + ": path of " + narrow(DllName()) +
                       ", loaded by version.dll in this folder. Remove both with " + narrow(launcher) +
                       " --uninstall.\r\n";
    body += narrow(dll) + "\r\n";
    HANDLE f = CreateFileW(pointer.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = f != INVALID_HANDLE_VALUE &&
                    WriteFile(f, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) &&
                    written == body.size();
    const DWORD err = GetLastError();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        if (!update) DeleteFileW(target.c_str());  // do not leave a proxy without its pointer behind
        return r.Error(L"Could not write " + pointer + L":\n" + ErrorText(err));
    }
    r.Done(std::wstring(update ? L"Updated" : L"Installed") + L". Start " + core::AsciiW(GetGameInfo().gameName) +
           L" from Steam to play with co-op.\n\nWritten:\n    " + target + L"\n    " + pointer +
           L"\n\nRemove with: " + launcher + L" --uninstall");
    return kOk;
}

int UninstallProxy(const std::wstring& gameExeOrDir, const InstallIo& io) {
    const Reporter r{io};
    const std::wstring dir = GameDir(gameExeOrDir);
    const std::wstring target = dir + L"\\" + kInstalledName;
    const std::wstring pointer = dir + L"\\" + PointerName();
    const std::wstring mod = Mod();
    r.Line(L"uninstall: game folder " + dir);

    const bool proxyOurs = IsFile(target) && IsOurProxy(target);
    const bool pointerOurs = IsFile(pointer) && IsOurPointer(pointer);
    std::wstring foreign;
    if (Exists(target) && !proxyOurs) foreign += L"\n    " + target;
    if (Exists(pointer) && !pointerOurs) foreign += L"\n    " + pointer;
    if (!proxyOurs && !pointerOurs) {
        r.Done(L"\"Start from Steam\" mode is not installed in\n    " + dir +
               (foreign.empty() ? L"" : L"\n\nLeft alone (not from " + mod + L"):" + foreign));
        return kOk;
    }
    if (GameRunningFrom(dir)) return r.Error(L"The game is running. Close it and try again.", kRefused);
    std::wstring list;
    if (proxyOurs) list += L"\n    " + target;
    if (pointerOurs) list += L"\n    " + pointer;
    if (!r.Confirm(L"Remove \"start from Steam\" mode? This deletes:" + list +
                   L"\n\nThe game then starts without co-op unless started with " + Launcher() + L".")) {
        r.Line(L"uninstall: cancelled");
        return kRefused;
    }
    std::wstring failed;
    for (const std::wstring& p : {proxyOurs ? target : std::wstring(), pointerOurs ? pointer : std::wstring()}) {
        if (p.empty()) continue;
        if (DeleteFileW(p.c_str()))
            r.Line(L"uninstall: deleted " + p);
        else
            failed += L"\n    " + p + L" (" + ErrorText(GetLastError()) + L")";
    }
    if (!failed.empty()) return r.Error(L"Could not delete:" + failed);
    r.Done(L"Removed:" + list + (foreign.empty() ? L"" : L"\n\nLeft alone (not from " + mod + L"):" + foreign));
    return kOk;
}

} // namespace mc::launcher
