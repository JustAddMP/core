#include "core/crash.h"

#include "core/log.h"
#include "core/mem.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

namespace mc::core::crash {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::wstring _dumpDir;
LPTOP_LEVEL_EXCEPTION_FILTER _previousFilter = nullptr;
PVOID _vectoredHandle = nullptr;

// One dump per process: 0 = none yet, 1 = being written, 2 = written. Only a dump that was actually written for an
// exception that ends the process sets 2; a failed write goes back to 0 so a later crash can still try.
volatile LONG _dumpState = 0;

// Stack kept back on each thread (SetThreadStackGuarantee) so that after a stack overflow the exception can still
// be dispatched to the unhandled-exception filter. Without it the dispatch itself overflows again and the process
// dies with no dump. Only threads with at least kMinStackForReserve get it (smaller stacks would lose too much).
constexpr ULONG kStackReserve = 64 * 1024;
constexpr ULONG_PTR kMinStackForReserve = 1024 * 1024;

// A stack overflow on a thread without that reserve may not reach the unhandled filter, so the vectored handler writes
// one "first-chance" dump for it (once per process). That dump does NOT use up _dumpState: the overflow may still be
// caught, and a later real crash must still get its own dump.
volatile LONG _firstChanceDumped = 0;

struct CrashJob {
    EXCEPTION_POINTERS* ep;
    DWORD threadId;
    const char* source;
    bool firstChance;  // vectored stack overflow on a thread without reserve: may still be handled
    bool written;
};

// The writer thread is started by Install, so a crashing thread (possibly with almost no stack left after a stack
// overflow) only has to SetEvent + WaitForSingleObject; the logging, unwinding and MiniDumpWriteDump run on the
// writer's own 256 KB stack. The job lives in static storage, not on the faulting thread's stack.
CrashJob _job{};
HANDLE _writerThread = nullptr;
DWORD _writerThreadId = 0;
HANDLE _requestEvent = nullptr;  // auto-reset: a job is ready
HANDLE _doneEvent = nullptr;     // auto-reset: the job is finished
volatile LONG _writerStop = 0;
volatile LONG _writerBusy = 0;   // one job at a time

// Codes the vectored handler acts on: only those after which the process always terminates, and which may never
// reach the unhandled-exception filter. A stack overflow is NOT one of them: code can catch it (__except +
// _resetstkoflw), so a first-chance one says nothing; if nobody handles it, the unhandled filter sees it.
bool IsAlwaysFatalCode(DWORD code) {
    switch (code) {
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION (the heap terminates the process after reporting it)
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN / fail-fast
        return true;
    default:
        return false;
    }
}

const char* CodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN";
    case 0xE06D7363: return "C++ EXCEPTION";
    default: return "?";
    }
}

// "module.dll+0x1234" for an address, without taking locks we might already hold.
void Describe(uintptr_t addr, char* buf, size_t cap) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        char path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = path;
        for (DWORD i = 0; i < n; ++i) if (path[i] == '\\') base = path + i + 1;
        _snprintf_s(buf, cap, _TRUNCATE, "%s+0x%llx", base,
                    static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        _snprintf_s(buf, cap, _TRUNCATE, "0x%016llx (no module)", static_cast<unsigned long long>(addr));
    }
}

// Walks the faulting thread's stack from its exception context with RtlLookupFunctionEntry/RtlVirtualUnwind
// (x64 unwind data; no symbols needed). Runs on the helper thread; the faulting thread is blocked meanwhile.
void LogStack(const CONTEXT& start) {
    CONTEXT ctx = start;
    for (int frame = 0; frame < 32 && ctx.Rip; ++frame) {
        char where[MAX_PATH + 48];
        Describe(static_cast<uintptr_t>(ctx.Rip), where, sizeof(where));
        log::WriteUnsafe(log::Level::Error, "crash:   #%02d %016llx %s", frame,
                         static_cast<unsigned long long>(ctx.Rip), where);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        } else {
            // Leaf function (or code without unwind info): return address is at [rsp].
            if (!mem::IsReadable(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
    }
}

bool WriteDump(const CrashJob& job) {
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                             : nullptr;
    if (!writeDump) {
        log::WriteUnsafe(log::Level::Error, "crash: dbghelp!MiniDumpWriteDump unavailable (%lu)", GetLastError());
        return false;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\crash-%04u%02u%02u-%02u%02u%02u%s.dmp", _dumpDir.c_str(), st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, job.firstChance ? L"-firstchance" : L"");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log::WriteUnsafe(log::Level::Error, "crash: cannot create dump file (%lu)", GetLastError());
        return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = job.threadId;
    mei.ExceptionPointers = job.ep;
    mei.ClientPointers = FALSE;
    auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                           MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &mei, nullptr, nullptr);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(file);
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
    if (ok) log::WriteUnsafe(log::Level::Error, "crash: minidump written: %s", narrow);
    else log::WriteUnsafe(log::Level::Error, "crash: MiniDumpWriteDump failed (0x%08lx)", err);
    if (!ok) DeleteFileW(path);
    return ok != FALSE;
}

DWORD WINAPI CrashThread(LPVOID param) {
    CrashJob& job = *static_cast<CrashJob*>(param);
    const EXCEPTION_RECORD* rec = job.ep->ExceptionRecord;
    const CONTEXT* ctx = job.ep->ContextRecord;
    char where[MAX_PATH + 48];
    Describe(reinterpret_cast<uintptr_t>(rec->ExceptionAddress), where, sizeof(where));
    if (job.firstChance)
        log::WriteUnsafe(log::Level::Error,
                         "crash: ===== STACK OVERFLOW (first chance, thread without stack reserve; may still be "
                         "handled) on thread %lu =====",
                         job.threadId);
    else
        log::WriteUnsafe(log::Level::Error, "crash: ===== FATAL EXCEPTION (%s) on thread %lu =====", job.source,
                         job.threadId);
    log::WriteUnsafe(log::Level::Error, "crash: code 0x%08lX %s at %p %s", rec->ExceptionCode,
                     CodeName(rec->ExceptionCode), rec->ExceptionAddress, where);
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
        const char* op = rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write"
                                                                                                         : "execute";
        log::WriteUnsafe(log::Level::Error, "crash: %s of address 0x%016llx", op,
                         static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    }
    log::WriteUnsafe(log::Level::Error,
                     "crash: rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx",
                     ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi);
    log::WriteUnsafe(log::Level::Error, "crash: rsp=%016llx rbp=%016llx r8=%016llx r9=%016llx rip=%016llx",
                     ctx->Rsp, ctx->Rbp, ctx->R8, ctx->R9, ctx->Rip);
    log::WriteUnsafe(log::Level::Error, "crash: stack:");
    LogStack(*ctx);
    job.written = WriteDump(job);
    return 0;
}

DWORD WINAPI WriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(_requestEvent, INFINITE);
        if (_writerStop) return 0;
        CrashThread(&_job);
        SetEvent(_doneEvent);
    }
}

// Runs one job on the writer thread (the caller only needs a little stack). Returns false if it did not finish.
bool RunJob(EXCEPTION_POINTERS* ep, const char* source, bool firstChance, bool waitForWriter, bool* written) {
    *written = false;
    // One job at a time; a real crash waits for a first-chance dump in progress, a first-chance one never waits.
    for (int i = 0; InterlockedCompareExchange(&_writerBusy, 1, 0) != 0; ++i) {
        if (!waitForWriter || i >= 6000) return false;
        Sleep(10);
    }
    _job = CrashJob{ep, GetCurrentThreadId(), source, firstChance, false};
    bool finished = true;
    if (_writerThread && GetCurrentThreadId() != _writerThreadId) {
        SetEvent(_requestEvent);
        finished = WaitForSingleObject(_doneEvent, 60000) == WAIT_OBJECT_0;
    } else {
        // No writer thread (Install could not start it) or the writer itself crashed: try a fresh thread, else
        // write right here.
        HANDLE t = CreateThread(nullptr, 256 * 1024, CrashThread, &_job, 0, nullptr);
        if (t) {
            finished = WaitForSingleObject(t, 60000) == WAIT_OBJECT_0;
            CloseHandle(t);
        } else {
            CrashThread(&_job);
        }
    }
    if (!finished) return false;  // still writing after 60 s: keep _writerBusy, leave it be
    *written = _job.written;
    InterlockedExchange(&_writerBusy, 0);
    return true;
}

// Only for exceptions that end the process (unhandled filter, always-fatal codes). Writes the log lines and the
// dump, once per process.
void HandleCrash(EXCEPTION_POINTERS* ep, const char* source) {
    if (InterlockedCompareExchange(&_dumpState, 1, 0) != 0) return;  // written already, or another thread is at it
    bool written = false;
    if (!RunJob(ep, source, false, true, &written)) {
        InterlockedExchange(&_dumpState, 2);  // the writer is stuck: don't start another
        return;
    }
    InterlockedExchange(&_dumpState, written ? 2 : 0);
}

bool ThreadHasStackReserve() {
    ULONG current = 0;  // 0 in = query
    return SetThreadStackGuarantee(&current) && current >= kStackReserve;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    HandleCrash(ep, "unhandled");
    if (_previousFilter && _previousFilter != UnhandledFilter) return _previousFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// First chance, before any __except / catch: anything else may still be handled by the code that raised it, so only
// the always-fatal codes are acted on here; the rest reach UnhandledFilter if nobody handles them.
LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (IsAlwaysFatalCode(code)) {
        HandleCrash(ep, "vectored");
    } else if (code == EXCEPTION_STACK_OVERFLOW && _dumpState == 0 && !ThreadHasStackReserve() &&
               InterlockedCompareExchange(&_firstChanceDumped, 1, 0) == 0) {
        // This thread has no stack reserve: if the overflow is not handled, the unhandled filter may never run.
        bool written = false;
        RunJob(ep, "vectored", true, false, &written);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool ReserveThreadStack() {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (high - low < kMinStackForReserve) return false;
    ULONG size = kStackReserve;
    return SetThreadStackGuarantee(&size) != FALSE;
}

bool Install(const std::wstring& dumpDir) {
    _dumpDir = dumpDir;
    ReserveThreadStack();
    CreateDirectoryW(dumpDir.c_str(), nullptr);
    if (!_writerThread) {
        if (!_requestEvent) _requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!_doneEvent) _doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _writerStop = 0;
        if (_requestEvent && _doneEvent)
            _writerThread = CreateThread(nullptr, 256 * 1024, WriterThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                         &_writerThreadId);
        if (!_writerThread) MC_LOG_WARN("crash: no dump writer thread (%lu); dumps use a fresh thread", GetLastError());
    }
    _previousFilter = SetUnhandledExceptionFilter(UnhandledFilter);
    _vectoredHandle = AddVectoredExceptionHandler(1, VectoredHandler);
    MC_LOG_INFO("crash: handler installed (previous filter %p, vectored %p)", _previousFilter, _vectoredHandle);
    return _vectoredHandle != nullptr;
}

void Reassert() {
    LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(UnhandledFilter);
    if (current != UnhandledFilter) {
        char where[MAX_PATH + 48] = "null";
        if (current) Describe(reinterpret_cast<uintptr_t>(current), where, sizeof(where));
        MC_LOG_WARN("crash: unhandled-exception filter had been replaced by %p (%s); re-installed ours, chaining it",
                    current, where);
        _previousFilter = current;
    }
}

void Uninstall() {
    if (_vectoredHandle) RemoveVectoredExceptionHandler(_vectoredHandle);
    _vectoredHandle = nullptr;
    SetUnhandledExceptionFilter(_previousFilter);
    if (_writerThread) {
        InterlockedExchange(&_writerStop, 1);
        SetEvent(_requestEvent);
        WaitForSingleObject(_writerThread, 5000);
        CloseHandle(_writerThread);
        _writerThread = nullptr;
        _writerThreadId = 0;
    }
}

} // namespace mc::core::crash
