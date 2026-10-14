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
volatile LONG _handled = 0;

struct CrashJob {
    EXCEPTION_POINTERS* ep;
    DWORD threadId;
    const char* source;
};

bool IsFatalVectoredCode(DWORD code) {
    switch (code) {
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
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

void WriteDump(const CrashJob& job) {
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                             : nullptr;
    if (!writeDump) {
        log::WriteUnsafe(log::Level::Error, "crash: dbghelp!MiniDumpWriteDump unavailable (%lu)", GetLastError());
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\crash-%04u%02u%02u-%02u%02u%02u.dmp", _dumpDir.c_str(), st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log::WriteUnsafe(log::Level::Error, "crash: cannot create dump file (%lu)", GetLastError());
        return;
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
}

DWORD WINAPI CrashThread(LPVOID param) {
    const CrashJob& job = *static_cast<CrashJob*>(param);
    const EXCEPTION_RECORD* rec = job.ep->ExceptionRecord;
    const CONTEXT* ctx = job.ep->ContextRecord;
    char where[MAX_PATH + 48];
    Describe(reinterpret_cast<uintptr_t>(rec->ExceptionAddress), where, sizeof(where));
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
    WriteDump(job);
    return 0;
}

void HandleCrash(EXCEPTION_POINTERS* ep, const char* source) {
    if (InterlockedExchange(&_handled, 1) != 0) return;  // only the first fatal exception
    CrashJob job{ep, GetCurrentThreadId(), source};
    // Work on a fresh thread: the faulting thread may have no stack left (stack overflow).
    HANDLE t = CreateThread(nullptr, 256 * 1024, CrashThread, &job, 0, nullptr);
    if (t) {
        WaitForSingleObject(t, 60000);
        CloseHandle(t);
    } else {
        CrashThread(&job);
    }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    HandleCrash(ep, "unhandled");
    if (_previousFilter && _previousFilter != UnhandledFilter) return _previousFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (IsFatalVectoredCode(ep->ExceptionRecord->ExceptionCode)) HandleCrash(ep, "vectored");
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool Install(const std::wstring& dumpDir) {
    _dumpDir = dumpDir;
    CreateDirectoryW(dumpDir.c_str(), nullptr);
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
}

} // namespace mc::core::crash
