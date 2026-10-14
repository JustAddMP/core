// Tests the version.dll proxy (src/proxy) and MafiaCoop.exe --install / --uninstall without the game.
//
// Usage: proxy_test.exe [work folder]     (default %TEMP%\mafiacoop-proxy-test; never the real game folder)
//
// 1. Forwarding: LoadLibrary's a copy of out\mafiacoop_proxy.dll named version.dll from <work>\fwd and calls its
//    exports (by name and by ordinal) against the real System32 version.dll; this exe is not the game, so the proxy
//    must not load mafiacoop.dll even though a pointer file is there.
// 2. Game process: copies this exe as <work>\game\mafiadefinitiveedition.exe next to the proxy (as version.dll)
//    and a pointer file naming <work>\mod\mafiacoop.dll (a copy of a harmless system dll), then runs it. Its
//    static import of version.dll resolves to the proxy, exactly like the game's; the child checks that the
//    "mafiacoop.dll" is loaded before main, from the pointer's path. Then: MAFIACOOP_LAUNCHER=1 (started by the
//    launcher) -> not loaded; no pointer file -> mafiacoop.dll next to the proxy; neither -> nothing, no crash.
// 3. Install: MafiaCoop.exe --install/--uninstall --yes --game <work>\fakegame (a folder with a dummy game exe):
//    install, reinstall (update), uninstall, uninstall again, foreign version.dll / pointer file refused and left
//    untouched, other files never touched.
#include "proxy/proxy_shared.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace {

int _failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++_failures;
}

std::wstring ModulePath(HMODULE m) {
    wchar_t buf[2048];
    const DWORD n = GetModuleFileNameW(m, buf, 2048);
    return std::wstring(buf, n);
}

std::wstring DirOf(const std::wstring& p) {
    return p.substr(0, p.find_last_of(L'\\'));
}

bool IsFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadAll(const std::wstring& p) {
    std::ifstream in(p, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string();
}

void WriteAll(const std::wstring& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring SystemDll(const wchar_t* name) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\" + name;
}

// Runs a process, its output going to ours, and returns its exit code (or -1).
int Run(const std::wstring& exe, const std::wstring& args, const wchar_t* extraEnv = nullptr) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (extraEnv) SetEnvironmentVariableW(extraEnv, L"1");
    std::fflush(stdout);
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (extraEnv) SetEnvironmentVariableW(extraEnv, nullptr);
    if (!ok) {
        std::printf("       CreateProcess(%ls) failed: %lu\n", exe.c_str(), GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// ---- child: runs as <work>\game\mafiadefinitiveedition.exe --------------------------------------------------------

int Child(const std::wstring& expect) {
    // Nothing alertable has run in this thread before main, so a module loaded now was loaded by the proxy's APC
    // at the end of process initialization (the moment the game's entry point would start).
    HMODULE mod = GetModuleHandleW(L"mafiacoop.dll");
    const std::wstring self = ModulePath(nullptr);
    const std::wstring ver = ModulePath(GetModuleHandleW(L"version.dll"));
    std::printf("       child: version.dll = %ls, mafiacoop.dll = %ls\n", ver.c_str(),
                mod ? ModulePath(mod).c_str() : L"(not loaded)");
    bool ok = _wcsicmp(DirOf(ver).c_str(), DirOf(self).c_str()) == 0;  // the import resolved to the proxy
    DWORD h = 0;
    ok = ok && GetFileVersionInfoSizeW(SystemDll(L"kernel32.dll").c_str(), &h) > 0;  // static import, via the proxy
    if (expect == L"none") {
        ok = ok && !mod;
    } else {
        wchar_t env[MAX_PATH] = {};
        ok = ok && mod && _wcsicmp(ModulePath(mod).c_str(), expect.c_str()) == 0 &&
             GetEnvironmentVariableW(mc::proxy::kEnvProxy, env, MAX_PATH) > 0 && _wcsicmp(env, ver.c_str()) == 0;
    }
    return ok ? 0 : 1;
}

// ---- parent -------------------------------------------------------------------------------------------------------

using FnSizeW = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using FnInfoW = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using FnQueryW = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
using FnLangW = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
using FnSizeExW = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);

bool FileVersion(FnSizeW size, FnInfoW info, FnQueryW query, const std::wstring& file, unsigned long long& v) {
    DWORD h = 0;
    const DWORD n = size(file.c_str(), &h);
    if (!n) return false;
    std::vector<char> buf(n);
    if (!info(file.c_str(), 0, n, buf.data())) return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!query(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return false;
    v = (static_cast<unsigned long long>(ffi->dwFileVersionMS) << 32) | ffi->dwFileVersionLS;
    return true;
}

void TestForwarding(const std::wstring& work, const std::wstring& proxy, const std::wstring& dummyMod) {
    std::printf("-- forwarding (host process is not the game)\n");
    const std::wstring dir = work + L"\\fwd";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring copy = dir + L"\\version.dll";
    Check(CopyFileW(proxy.c_str(), copy.c_str(), FALSE) != 0, "copy the proxy as fwd\\version.dll");
    WriteAll(dir + L"\\" + mc::proxy::kPointerName, std::string(mc::proxy::kPointerHeader) + "\r\n" + Utf8(dummyMod));

    HMODULE p = LoadLibraryW(copy.c_str());
    Check(p != nullptr, "LoadLibrary(fwd\\version.dll)");
    if (!p) return;
    Check(p != GetModuleHandleW(SystemDll(L"version.dll").c_str()), "it is a separate module from System32's");
    auto sizeW = reinterpret_cast<FnSizeW>(GetProcAddress(p, "GetFileVersionInfoSizeW"));
    auto infoW = reinterpret_cast<FnInfoW>(GetProcAddress(p, "GetFileVersionInfoW"));
    auto queryW = reinterpret_cast<FnQueryW>(GetProcAddress(p, "VerQueryValueW"));
    auto langW = reinterpret_cast<FnLangW>(GetProcAddress(p, "VerLanguageNameW"));
    auto sizeExW = reinterpret_cast<FnSizeExW>(GetProcAddress(p, "GetFileVersionInfoSizeExW"));
    auto byOrdinal = reinterpret_cast<FnSizeW>(GetProcAddress(p, MAKEINTRESOURCEA(8)));
    Check(sizeW && infoW && queryW && langW && sizeExW && byOrdinal, "exports present (by name and ordinal 8)");
    if (!(sizeW && infoW && queryW && langW && sizeExW && byOrdinal)) return;
    Check(GetProcAddress(p, "GetFileVersionInfoByHandle") && GetProcAddress(p, "VerInstallFileA") &&
              GetProcAddress(p, "VerFindFileW"),
          "all 17 version.dll exports (spot check of the undocumented/rare ones)");

    const std::wstring k32 = SystemDll(L"kernel32.dll");
    DWORD h = 0;
    const DWORD realSize = GetFileVersionInfoSizeW(k32.c_str(), &h);
    Check(realSize > 0 && sizeW(k32.c_str(), &h) == realSize, "GetFileVersionInfoSizeW(kernel32.dll) == System32's");
    Check(byOrdinal(k32.c_str(), &h) == realSize, "ordinal 8 is GetFileVersionInfoSizeW");
    Check(sizeExW(FILE_VER_GET_NEUTRAL, k32.c_str(), &h) == GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, k32.c_str(), &h),
          "GetFileVersionInfoSizeExW == System32's");
    unsigned long long vProxy = 0, vReal = 0;
    const bool okProxy = FileVersion(sizeW, infoW, queryW, k32, vProxy);
    const bool okReal = FileVersion(&GetFileVersionInfoSizeW, &GetFileVersionInfoW, &VerQueryValueW, k32, vReal);
    std::printf("       kernel32.dll file version via proxy %llu.%llu.%llu.%llu\n", vProxy >> 48, (vProxy >> 32) & 0xFFFF,
                (vProxy >> 16) & 0xFFFF, vProxy & 0xFFFF);
    Check(okProxy && okReal && vProxy == vReal, "GetFileVersionInfoW + VerQueryValueW give the same file version");
    wchar_t lang[128] = {};
    langW(0x409, lang, 128);
    Check(std::wcsstr(lang, L"English") != nullptr, "VerLanguageNameW(0x409) (forwarded on to kernel32)");
    SetLastError(0);
    Check(sizeW(L"Z:\\no\\such\\file.dll", &h) == 0 && GetLastError() != 0, "errors pass through (missing file)");

    SleepEx(0, TRUE);  // deliver any APC the proxy might have queued
    Check(GetModuleHandleW(L"mafiacoop.dll") == nullptr, "no mafiacoop.dll loaded: this process is not the game");
}

void TestGameProcess(const std::wstring& work, const std::wstring& proxy, const std::wstring& dummyMod) {
    std::printf("-- game process (a copy of this exe named mafiadefinitiveedition.exe)\n");
    const std::wstring dir = work + L"\\game";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring exe = dir + L"\\" + mc::proxy::kGameExe;
    const std::wstring pointer = dir + L"\\" + mc::proxy::kPointerName;
    const std::wstring local = dir + L"\\mafiacoop.dll";
    Check(CopyFileW(ModulePath(nullptr).c_str(), exe.c_str(), FALSE) && CopyFileW(proxy.c_str(), (dir + L"\\version.dll").c_str(), FALSE),
          "copy this exe as mafiadefinitiveedition.exe and the proxy as version.dll");
    WriteAll(pointer, std::string(mc::proxy::kPointerHeader) + ": test\r\n\r\n" + Utf8(dummyMod) + "\r\n");
    DeleteFileW(local.c_str());

    Check(Run(exe, L"--child \"" + dummyMod + L"\"") == 0, "proxy loads mafiacoop.dll from the pointer file before main");
    Check(Run(exe, L"--child none", mc::proxy::kEnvLauncher) == 0,
          "MAFIACOOP_LAUNCHER=1 (started by MafiaCoop.exe): the proxy leaves the loading to the launcher");
    DeleteFileW(pointer.c_str());
    Check(Run(exe, L"--child none") == 0, "no pointer file and no mafiacoop.dll next to it: nothing loaded, no crash");
    CopyFileW(dummyMod.c_str(), local.c_str(), FALSE);
    Check(Run(exe, L"--child \"" + local + L"\"") == 0, "no pointer file: mafiacoop.dll next to the proxy is used");
    DeleteFileW(local.c_str());
}

void TestInstall(const std::wstring& work, const std::wstring& outDir, const std::wstring& proxy) {
    std::printf("-- MafiaCoop.exe --install / --uninstall against a fake game folder\n");
    const std::wstring launcher = outDir + L"\\MafiaCoop.exe";
    const std::wstring fake = work + L"\\fakegame";
    CreateDirectoryW(fake.c_str(), nullptr);
    const std::wstring gameExe = fake + L"\\" + mc::proxy::kGameExe;
    const std::wstring target = fake + L"\\version.dll";
    const std::wstring pointer = fake + L"\\" + mc::proxy::kPointerName;
    const std::wstring other = fake + L"\\other_mod_file.txt";
    WriteAll(gameExe, "MZ dummy game exe for proxy_test");
    WriteAll(other, "not ours");
    DeleteFileW(target.c_str());
    DeleteFileW(pointer.c_str());
    const std::wstring game = L"--game \"" + gameExe + L"\"";
    const std::string proxyBytes = ReadAll(proxy);

    Check(Run(launcher, L"--install --yes " + game) == 0, "--install exits 0");
    Check(ReadAll(target) == proxyBytes, "version.dll in the game folder is mafiacoop_proxy.dll");
    const std::string ptr = ReadAll(pointer);
    Check(ptr.rfind(mc::proxy::kPointerHeader, 0) == 0 && ptr.find(Utf8(outDir + L"\\mafiacoop.dll")) != std::string::npos,
          "mafiacoop_path.txt names out\\mafiacoop.dll");
    Check(Run(launcher, L"--install --yes --game \"" + fake + L"\"") == 0, "--install again (update, --game as folder) exits 0");
    Check(Run(launcher, L"--uninstall --yes " + game) == 0, "--uninstall exits 0");
    Check(!IsFile(target) && !IsFile(pointer), "both files removed");
    Check(IsFile(gameExe) && ReadAll(other) == "not ours", "other files untouched");
    Check(Run(launcher, L"--uninstall --yes " + game) == 0, "--uninstall when not installed exits 0");

    WriteAll(target, "foreign version.dll from another mod");
    Check(Run(launcher, L"--install --yes " + game) == 3, "--install with a foreign version.dll is refused (exit 3)");
    Check(ReadAll(target) == "foreign version.dll from another mod" && !IsFile(pointer),
          "foreign version.dll unchanged, no pointer file written");
    Check(Run(launcher, L"--uninstall --yes " + game) == 0 && ReadAll(target) == "foreign version.dll from another mod",
          "--uninstall leaves the foreign version.dll alone");
    DeleteFileW(target.c_str());

    WriteAll(pointer, "someone else's file");
    Check(Run(launcher, L"--install --yes " + game) == 3 && !IsFile(target), "--install with a foreign mafiacoop_path.txt is refused");
    Check(ReadAll(pointer) == "someone else's file", "foreign mafiacoop_path.txt unchanged");
    DeleteFileW(pointer.c_str());

    Check(Run(launcher, L"--install --yes --game \"" + work + L"\"") == 1 && !IsFile(work + L"\\version.dll"),
          "--install into a folder without the game exe fails and writes nothing");
    DeleteFileW(gameExe.c_str());
    DeleteFileW(other.c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wcscmp(argv[1], L"--child") == 0) return Child(argv[2]);

    const std::wstring outDir = DirOf(ModulePath(nullptr));
    std::wstring work;
    if (argc >= 2) {
        work = argv[1];
    } else {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        work = std::wstring(tmp) + L"mafiacoop-proxy-test";
    }
    while (!work.empty() && work.back() == L'\\') work.pop_back();
    CreateDirectoryW(work.c_str(), nullptr);
    std::printf("proxy_test: work folder %ls\n", work.c_str());

    const std::wstring proxy = outDir + L"\\" + mc::proxy::kBuiltName;
    if (!IsFile(proxy)) {
        std::printf("FAIL: %ls not built\n", proxy.c_str());
        return 1;
    }
    Check(ReadAll(proxy).find(mc::proxy::kMarker) != std::string::npos, "mafiacoop_proxy.dll carries the marker");
    // Stand-in for mafiacoop.dll: a harmless system dll under that name (DllMain does nothing visible).
    const std::wstring modDir = work + L"\\mod";
    CreateDirectoryW(modDir.c_str(), nullptr);
    const std::wstring dummyMod = modDir + L"\\mafiacoop.dll";
    Check(CopyFileW(SystemDll(L"msimg32.dll").c_str(), dummyMod.c_str(), FALSE) != 0, "dummy mafiacoop.dll (msimg32 copy)");

    TestForwarding(work, proxy, dummyMod);
    TestGameProcess(work, proxy, dummyMod);
    TestInstall(work, outDir, proxy);

    std::printf("%s (%d failure%s)\n", _failures ? "FAILED" : "PASSED", _failures, _failures == 1 ? "" : "s");
    return _failures ? 1 : 0;
}
