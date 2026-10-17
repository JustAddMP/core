#include "core/log.h"

#include "core/trace.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace mc::core::log {
namespace {

HANDLE _file = INVALID_HANDLE_VALUE;
SRWLOCK _lock = SRWLOCK_INIT;
std::wstring _dir, _base;
constexpr int kKeepRuns = 5;  // previous logs kept: <name>.1 .. <name>.5

const char* LevelName(Level level) {
    switch (level) {
    case Level::Debug: return "DBG";
    case Level::Info:  return "INF";
    case Level::Warn:  return "WRN";
    case Level::Error: return "ERR";
    }
    return "???";
}

// Formats one full line (with CRLF) into buf; returns its length.
int FormatLine(char* buf, int cap, Level level, const char* fmt, va_list args) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(buf, cap, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s ",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        GetCurrentThreadId(), LevelName(level));
    if (n < 0) n = 0;
    int m = _vsnprintf_s(buf + n, cap - n - 2, _TRUNCATE, fmt, args);
    n = (m < 0) ? static_cast<int>(strlen(buf)) : n + m;  // truncated: keep what fit
    buf[n++] = '\r';
    buf[n++] = '\n';
    return n;
}

void Emit(const char* line, int len) {
    if (_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(_file, line, static_cast<DWORD>(len), &written, nullptr);
    }
    OutputDebugStringA(line);
}

} // namespace

bool Init(const std::wstring& dir, const wchar_t* fileName) {
    AcquireSRWLockExclusive(&_lock);
    if (_file == INVALID_HANDLE_VALUE) {
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\" + fileName;
        for (int i = kKeepRuns - 1; i >= 1; --i)
            MoveFileExW((path + L"." + std::to_wstring(i)).c_str(), (path + L"." + std::to_wstring(i + 1)).c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        _dir = dir;
        _base = fileName;
        if (_base.size() > 4 && _base.compare(_base.size() - 4, 4, L".log") == 0) _base.resize(_base.size() - 4);
        _file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    bool ok = _file != INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&_lock);
    return ok;
}

void Shutdown() {
    AcquireSRWLockExclusive(&_lock);
    if (_file != INVALID_HANDLE_VALUE) {
        CloseHandle(_file);
        _file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&_lock);
}

bool IsOpen() { return _file != INVALID_HANDLE_VALUE; }

std::wstring Dir() {
    AcquireSRWLockShared(&_lock);
    std::wstring d = _dir;
    ReleaseSRWLockShared(&_lock);
    return d;
}

std::wstring BaseName() {
    AcquireSRWLockShared(&_lock);
    std::wstring b = _base;
    ReleaseSRWLockShared(&_lock);
    return b;
}

void WriteV(Level level, const char* fmt, va_list args) {
    char buf[2048];
    int len = FormatLine(buf, sizeof(buf), level, fmt, args);
    AcquireSRWLockExclusive(&_lock);
    Emit(buf, len);
    ReleaseSRWLockExclusive(&_lock);
    trace::Raw(buf, static_cast<size_t>(len));  // own lock, never taken while holding ours
}

void Write(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

void WriteUnsafe(Level level, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int len = FormatLine(buf, sizeof(buf), level, fmt, args);
    va_end(args);
    // Try the lock for ~200 ms; if the crashing thread holds it, write anyway.
    bool locked = false;
    for (int i = 0; i < 20 && !(locked = TryAcquireSRWLockExclusive(&_lock) != 0); ++i) Sleep(10);
    Emit(buf, len);
    if (locked) ReleaseSRWLockExclusive(&_lock);
}

} // namespace mc::core::log
