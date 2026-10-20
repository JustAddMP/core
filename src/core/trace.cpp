#include "core/trace.h"

#include "core/log.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mc::core::trace {
namespace {

SRWLOCK _lock = SRWLOCK_INIT;
HANDLE _file = INVALID_HANDLE_VALUE;  // read without the lock only for the cheap "none open" early out
std::wstring _path;
std::string _buf;
double _lastFlush = 0;
unsigned long long _written = 0;
bool _full = false;
CloseHook _beforeClose = nullptr, _afterClose = nullptr;
constexpr size_t kFlushBytes = 256 * 1024;
constexpr double kFlushInterval = 0.5;

void FlushLocked() {
    if (_file == INVALID_HANDLE_VALUE || _buf.empty()) {
        _buf.clear();
        return;
    }
    DWORD n = 0;
    WriteFile(_file, _buf.data(), static_cast<DWORD>(_buf.size()), &n, nullptr);
    _written += _buf.size();
    _buf.clear();
}

void AppendLocked(const char* line, size_t len, double now) {
    if (_file == INVALID_HANDLE_VALUE || _full) return;
    if (_written + _buf.size() + len > kMaxBytes) {
        _full = true;
        char msg[96];
        const int n = std::snprintf(msg, sizeof msg, "%.4f END trace size limit reached: no more lines\r\n", now);
        if (n > 0) _buf.append(msg, static_cast<size_t>(n));
        FlushLocked();
        return;
    }
    _buf.append(line, len);
    if (_buf.size() >= kFlushBytes || now - _lastFlush >= kFlushInterval) {
        _lastFlush = now;
        FlushLocked();
    }
}

int Format(char* buf, int cap, double t, const char* type, const char* fmt, va_list args) {
    int n = std::snprintf(buf, static_cast<size_t>(cap), "%.4f %s ", t, type);
    if (n < 0 || n >= cap - 2) return 0;
    const int m = _vsnprintf_s(buf + n, static_cast<size_t>(cap - n - 2), _TRUNCATE, fmt, args);
    n = m < 0 ? static_cast<int>(std::strlen(buf)) : n + m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return n;
}

}  // namespace

int PruneFolder(const std::wstring& dir, const wchar_t* pattern, int keepFiles, unsigned long long keepBytes) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    struct File {
        unsigned long long time, size;
        std::wstring name;
    };
    std::vector<File> files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const unsigned long long t = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                     fd.ftLastWriteTime.dwLowDateTime;
        const unsigned long long n = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        files.push_back({t, n, fd.cFileName});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.time > b.time; });  // newest first
    int removed = 0;
    unsigned long long kept = 0;
    bool full = false;  // from the first file that does not fit on, the older ones go too
    for (size_t i = 0; i < files.size(); ++i) {
        full = full || (keepFiles >= 0 && static_cast<int>(i) >= keepFiles) ||
               (keepBytes > 0 && kept + files[i].size > keepBytes);
        if (full) {
            if (DeleteFileW((dir + L"\\" + files[i].name).c_str())) ++removed;
        } else {
            kept += files[i].size;
        }
    }
    return removed;
}

void SetCloseHooks(CloseHook before, CloseHook after) {
    AcquireSRWLockExclusive(&_lock);
    _beforeClose = before;
    _afterClose = after;
    ReleaseSRWLockExclusive(&_lock);
}

double Now() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

bool Open(const std::string& header, const char* tag) {
    Close();
    const std::wstring logDir = log::Dir();
    if (logDir.empty()) return false;
    const std::wstring dir = logDir + L"\\trace";
    CreateDirectoryW(dir.c_str(), nullptr);
    PruneFolder(dir, L"*.trace", kKeepFiles - 1, kMaxFolderBytes - kMaxBytes);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring name = log::BaseName();
    if (tag && *tag) {
        name += L"-";
        for (const char* c = tag; *c; ++c) name += static_cast<wchar_t>(static_cast<unsigned char>(*c));
    }
    const std::wstring path = dir + L"\\" + name + L"-" + stamp + L".trace";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const double now = Now();
    std::vector<char> line(header.size() + 256);
    const int n = std::snprintf(line.data(), line.size(),
                                "%.4f H trace=1 wall=%04u-%02u-%02uT%02u:%02u:%02u.%03u qpc=%.4f pid=%lu %s\r\n", now,
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, now,
                                GetCurrentProcessId(), header.c_str());
    AcquireSRWLockExclusive(&_lock);
    _file = f;
    _path = path;
    _written = 0;
    _full = false;
    _buf.clear();
    _lastFlush = now;
    if (n > 0) AppendLocked(line.data(), std::min(static_cast<size_t>(n), line.size() - 1), now);
    FlushLocked();
    ReleaseSRWLockExclusive(&_lock);
    return true;
}

void Close() {
    AcquireSRWLockShared(&_lock);
    const bool open = _file != INVALID_HANDLE_VALUE;
    const CloseHook before = _beforeClose, after = _afterClose;
    const std::wstring path = _path;
    ReleaseSRWLockShared(&_lock);
    if (!open) return;
    if (before) before(path);  // its last records go in while the file is open
    bool closed = false;
    AcquireSRWLockExclusive(&_lock);
    if (_file != INVALID_HANDLE_VALUE && _path == path) {
        FlushLocked();
        CloseHandle(_file);
        _file = INVALID_HANDLE_VALUE;
        _path.clear();
        closed = true;
    }
    ReleaseSRWLockExclusive(&_lock);
    if (closed && after) after(path);
}

bool IsOpen() {
    AcquireSRWLockShared(&_lock);
    const bool open = _file != INVALID_HANDLE_VALUE;
    ReleaseSRWLockShared(&_lock);
    return open;
}

std::wstring Path() {
    AcquireSRWLockShared(&_lock);
    std::wstring p = _path;
    ReleaseSRWLockShared(&_lock);
    return p;
}

void LineV(const char* type, const char* fmt, va_list args) {
    if (_file == INVALID_HANDLE_VALUE) return;  // re-checked under the lock
    char buf[2048];
    const double now = Now();
    const int n = Format(buf, sizeof buf, now, type, fmt, args);
    if (n <= 0) return;
    AcquireSRWLockExclusive(&_lock);
    AppendLocked(buf, static_cast<size_t>(n), now);
    ReleaseSRWLockExclusive(&_lock);
}

void Line(const char* type, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LineV(type, fmt, args);
    va_end(args);
}

void LineAt(double t, const char* type, const char* fmt, ...) {
    if (_file == INVALID_HANDLE_VALUE) return;
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = Format(buf, sizeof buf, t, type, fmt, args);
    va_end(args);
    if (n <= 0) return;
    const double now = Now();
    AcquireSRWLockExclusive(&_lock);
    AppendLocked(buf, static_cast<size_t>(n), now);
    ReleaseSRWLockExclusive(&_lock);
}

void Raw(const char* text, size_t len) {
    if (_file == INVALID_HANDLE_VALUE) return;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) --len;
    char head[32];
    const double now = Now();
    const int h = std::snprintf(head, sizeof head, "%.4f ", now);
    if (h <= 0) return;
    std::string line;
    line.reserve(static_cast<size_t>(h) + len + 2);
    line.append(head, static_cast<size_t>(h));
    line.append(text, len);
    line.append("\r\n");
    AcquireSRWLockExclusive(&_lock);
    AppendLocked(line.data(), line.size(), now);
    ReleaseSRWLockExclusive(&_lock);
}

void Flush() {
    AcquireSRWLockExclusive(&_lock);
    FlushLocked();
    _lastFlush = Now();
    ReleaseSRWLockExclusive(&_lock);
}

}  // namespace mc::core::trace
