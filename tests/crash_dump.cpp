// crash_dump: the crash handler (src/core/crash.cpp) writes exactly one minidump for the exception that really ends
// the process, even after first-chance exceptions that were handled (a caught stack overflow must not use up the
// one-shot dump). Runs itself as a child process per scenario with the dumps going to a fresh temp directory, reads
// the exception code back from each dump, and deletes the directory. Exits non-zero on any failure.
#include <windows.h>
#include <dbghelp.h>
#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/crash.h"

namespace {

// ---- child side ---------------------------------------------------------------------------------------------------

volatile int g_stop = 0;
volatile int* volatile g_null = nullptr;

__declspec(noinline) int Recurse(int depth) {
    volatile char pad[2048];
    pad[0] = static_cast<char>(depth);
    if (g_stop) return 0;
    return Recurse(depth + 1) + pad[0];
}

// A stack overflow the code catches itself (first chance only; must not cost the dump).
bool HandledStackOverflow() {
    bool caught = false;
    __try {
        Recurse(0);
    } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER
                                                                : EXCEPTION_CONTINUE_SEARCH) {
        caught = true;
    }
    if (caught) _resetstkoflw();  // restore the guard page so a later overflow is detected again
    return caught;
}

bool HandledAccessViolation() {
    __try {
        *g_null = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}

bool HandledCppException() {
    try {
        throw std::string("handled");
    } catch (const std::string&) {
        return true;
    }
}

// A thread without the crash handler's stack reserve (no ReserveThreadStack; the dll does it for every thread, a test
// exe gets no DLL_THREAD_ATTACH): 1 = overflow and catch it, 2 = overflow unhandled.
DWORD WINAPI UnreservedThread(LPVOID param) {
    if (param == reinterpret_cast<LPVOID>(1)) return HandledStackOverflow() ? 0 : 1;
    return static_cast<DWORD>(Recurse(0));
}

DWORD RunUnreserved(int what) {
    HANDLE t = CreateThread(nullptr, 1024 * 1024, UnreservedThread, reinterpret_cast<LPVOID>(static_cast<INT_PTR>(what)),
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return 99;
    WaitForSingleObject(t, INFINITE);
    DWORD rc = 99;
    GetExitCodeThread(t, &rc);
    CloseHandle(t);
    return rc;
}

// Ends the process quickly once the crash handler (which chains to it) is done, instead of Windows Error Reporting.
LONG WINAPI ExitFilter(EXCEPTION_POINTERS* ep) {
    TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

int Child(const char* mode, const wchar_t* dir) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(ExitFilter);  // installed first, so the crash handler chains to it
    mc::core::crash::Install(dir);

    const bool so = HandledStackOverflow(), av = HandledAccessViolation(), cpp = HandledCppException();
    if (!so || !av || !cpp) {
        std::printf("child: handled exceptions not caught (so=%d av=%d cpp=%d)\n", so, av, cpp);
        return 10;
    }
    if (!std::strcmp(mode, "handled-only")) return 0;
    if (!std::strcmp(mode, "then-av")) *g_null = 2;                       // unhandled access violation
    if (!std::strcmp(mode, "then-overflow")) Recurse(0);                 // unhandled stack overflow
    if (!std::strcmp(mode, "unreserved-handled-then-av")) {
        if (RunUnreserved(1) != 0) return 12;
        *g_null = 3;
    }
    if (!std::strcmp(mode, "unreserved-overflow")) RunUnreserved(2);
    if (!std::strcmp(mode, "fatal-code")) RaiseException(0xC0000374, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 11;  // not reached in the crash modes
}

// ---- parent side --------------------------------------------------------------------------------------------------

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::vector<std::wstring> Dumps(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(dir + L"\\" + fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void RemoveDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Exception code recorded in a minidump's exception stream (0 = none / unreadable).
DWORD DumpExceptionCode(const std::wstring& path) {
    DWORD code = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m) {
        if (void* base = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0)) {
            MINIDUMP_DIRECTORY* dirEntry = nullptr;
            void* stream = nullptr;
            ULONG size = 0;
            if (MiniDumpReadDumpStream(base, ExceptionStream, &dirEntry, &stream, &size) && stream &&
                size >= sizeof(MINIDUMP_EXCEPTION_STREAM))
                code = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream)->ExceptionRecord.ExceptionCode;
            UnmapViewOfFile(base);
        }
        CloseHandle(m);
    }
    CloseHandle(f);
    return code;
}

struct Expected {
    DWORD code;
    bool firstChance;       // crash-<time>-firstchance.dmp
    bool optional = false;  // may or may not be there
};
struct Scenario {
    const char* mode;
    bool crashes;
    std::vector<Expected> dumps;
    const char* what;
};

void Run(const std::wstring& exe, const std::wstring& root, const Scenario& sc, int index) {
    const std::wstring dir = root + L"\\" + std::to_wstring(index);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring cmd = L"\"" + exe + L"\" --child " + std::wstring(sc.mode, sc.mode + std::strlen(sc.mode)) +
                       L" \"" + dir + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Check(false, sc.what, "CreateProcess failed");
        return;
    }
    const bool exited = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    if (!exited) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    const auto dumps = Dumps(dir);
    std::string got;
    std::vector<Expected> found;
    for (const auto& d : dumps) {
        const bool fc = d.find(L"-firstchance.dmp") != std::wstring::npos;
        const DWORD code = DumpExceptionCode(d);
        found.push_back({code, fc});
        char one[48];
        std::snprintf(one, sizeof(one), " 0x%08lX%s", code, fc ? "(first chance)" : "");
        got += one;
    }
    bool match = true;
    for (const auto& e : sc.dumps) {  // every required dump is there
        bool any = false;
        for (const auto& f : found) any = any || (f.code == e.code && f.firstChance == e.firstChance);
        match = match && (any || e.optional);
    }
    for (const auto& f : found) {  // and nothing else
        bool known = false;
        for (const auto& e : sc.dumps) known = known || (f.code == e.code && f.firstChance == e.firstChance);
        match = match && known;
    }
    size_t required = 0;
    for (const auto& e : sc.dumps) required += e.optional ? 0 : 1;
    match = match && found.size() >= required && found.size() <= sc.dumps.size();
    char detail[256];
    std::snprintf(detail, sizeof(detail), "exit 0x%08lX, dumps:%s", exitCode, got.empty() ? " none" : got.c_str());
    Check(exited && (sc.crashes ? exitCode != 0 : exitCode == 0) && match, sc.what, detail);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && !std::wcscmp(argv[1], L"--child")) {
        char mode[64];
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, mode, sizeof(mode), nullptr, nullptr);
        return Child(mode, argv[3]);
    }

    wchar_t exe[MAX_PATH], tmp[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"mc_crash_dump_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);

    constexpr DWORD kAV = EXCEPTION_ACCESS_VIOLATION, kSO = EXCEPTION_STACK_OVERFLOW;
    const Scenario scenarios[] = {
        {"handled-only", false, {}, "handled exceptions only: no dump"},
        {"then-av", true, {{kAV, false}}, "handled stack overflow, then crash: crash dumped"},
        {"then-overflow", true, {{kSO, false}}, "handled stack overflow, then real one: dumped"},
        {"unreserved-handled-then-av", true, {{kSO, true}, {kAV, false}},
         "no-reserve thread: first-chance dump, crash still dumped"},
        // the unhandled filter may or may not get far enough on a thread without reserve
        {"unreserved-overflow", true, {{kSO, true}, {kSO, false, true}}, "no-reserve thread, unhandled overflow: dumped"},
        {"fatal-code", true, {{0xC0000374, false}}, "always-fatal code (heap corruption): one dump"},
    };
    int i = 0;
    for (const auto& sc : scenarios) Run(exe, root, sc, i++);

    for (int k = 0; k < i; ++k) RemoveDir(root + L"\\" + std::to_wstring(k));
    RemoveDirectoryW(root.c_str());
    Check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "temp directory removed");

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
