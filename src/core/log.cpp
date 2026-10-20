#include "core/log.h"

#include "core/trace.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace mc::core::log {
namespace {

HANDLE _file = INVALID_HANDLE_VALUE;
SRWLOCK _lock = SRWLOCK_INIT;
std::wstring _dir, _base, _path;

// Repeats and totals (under _statsLock; never taken while holding _lock).
struct SiteState {
    RepeatState rs;
    int level = 0;
    uint64_t total = 0, suppressed = 0;
    std::string last;  // the last suppressed line's text (after the level)
};
SRWLOCK _statsLock = SRWLOCK_INIT;
std::unordered_map<const char*, SiteState>* _sites = nullptr;  // never destroyed (logging at static destruction)
uint64_t _lines[4] = {}, _asked[4] = {}, _suppressed = 0;

double Seconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

const char* LevelName(Level level) {
    switch (level) {
    case Level::Debug: return "DBG";
    case Level::Info:  return "INF";
    case Level::Warn:  return "WRN";
    case Level::Error: return "ERR";
    }
    return "???";
}

// The line's prefix (time, thread, level) into buf; returns its length.
int FormatPrefix(char* buf, int cap, Level level) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int n = _snprintf_s(buf, cap, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s ", st.wYear,
                              st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                              GetCurrentThreadId(), LevelName(level));
    return n < 0 ? 0 : n;
}

// Formats one full line (with CRLF) into buf; returns its length. *textAt: where the text after the prefix starts.
int FormatLine(char* buf, int cap, Level level, const char* fmt, va_list args, int* textAt = nullptr) {
    int n = FormatPrefix(buf, cap, level);
    if (textAt) *textAt = n;
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

void EmitLine(const char* line, int len) {
    AcquireSRWLockExclusive(&_lock);
    Emit(line, len);
    ReleaseSRWLockExclusive(&_lock);
    trace::Raw(line, static_cast<size_t>(len));  // own lock, never taken while holding ours
}

// "repeated N more times in 60 s: <last>" for a site (the level of the site).
void EmitRepeat(int level, uint64_t n, const std::string& last) {
    char buf[1024];
    int p = FormatPrefix(buf, sizeof buf, static_cast<Level>(level));
    const int m = _snprintf_s(buf + p, sizeof buf - p - 2, _TRUNCATE,
                              "repeated %llu more times in %.0f s (rate limited; last: %s)",
                              static_cast<unsigned long long>(n), kRepeatWindowS, last.c_str());
    p = m < 0 ? static_cast<int>(strlen(buf)) : p + m;
    buf[p++] = '\r';
    buf[p++] = '\n';
    EmitLine(buf, p);
}

std::unordered_map<const char*, SiteState>& Sites() {
    if (!_sites) _sites = new std::unordered_map<const char*, SiteState>;
    return *_sites;
}

} // namespace

bool RepeatAllow(RepeatState& s, double now, uint64_t* reportPending) {
    if (reportPending) *reportPending = 0;
    if (now - s.windowStart >= kRepeatWindowS) {  // a new window
        if (reportPending) *reportPending = s.pending;
        s.pending = 0;
        s.windowStart = now;
        s.inWindow = 0;
    }
    if (s.inWindow < kRepeatBurst) {
        ++s.inWindow;
        return true;
    }
    ++s.pending;
    return false;
}

uint64_t RepeatExpired(RepeatState& s, double now) {
    if (s.pending == 0 || now - s.windowStart < kRepeatWindowS) return 0;
    const uint64_t n = s.pending;
    s.pending = 0;
    return n;
}

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
        _path = path;
        if (_base.size() > 4 && _base.compare(_base.size() - 4, 4, L".log") == 0) _base.resize(_base.size() - 4);
        _file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    bool ok = _file != INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&_lock);
    return ok;
}

void Shutdown() {
    FlushRepeats();
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

std::wstring Path() {
    AcquireSRWLockShared(&_lock);
    std::wstring p = _path;
    ReleaseSRWLockShared(&_lock);
    return p;
}

void WriteV(Level level, const char* fmt, va_list args) {
    char buf[2048];
    int textAt = 0;
    int len = FormatLine(buf, sizeof(buf), level, fmt, args, &textAt);
    const int li = static_cast<int>(level) & 3;
    if (level == Level::Warn || level == Level::Error) {
        uint64_t report = 0;
        bool allow = true;
        std::string last;
        AcquireSRWLockExclusive(&_statsLock);
        ++_asked[li];
        SiteState& s = Sites()[fmt];
        s.level = li;
        ++s.total;
        allow = RepeatAllow(s.rs, Seconds(), &report);
        if (report) last = s.last;
        if (allow) {
            ++_lines[li];
        } else {
            ++s.suppressed;
            ++_suppressed;
            s.last.assign(buf + textAt, static_cast<size_t>(std::max(0, len - 2 - textAt)));
        }
        ReleaseSRWLockExclusive(&_statsLock);
        if (report) EmitRepeat(li, report, last);
        if (!allow) return;
    } else {
        AcquireSRWLockExclusive(&_statsLock);
        ++_asked[li];
        ++_lines[li];
        ReleaseSRWLockExclusive(&_statsLock);
    }
    EmitLine(buf, len);
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

void FlushRepeats() {
    struct Due {
        int level;
        uint64_t n;
        std::string last;
    };
    std::vector<Due> due;
    const double now = Seconds();
    AcquireSRWLockExclusive(&_statsLock);
    if (_sites)
        for (auto& [fmt, s] : *_sites)
            if (const uint64_t n = RepeatExpired(s.rs, now)) due.push_back({s.level, n, s.last});
    ReleaseSRWLockExclusive(&_statsLock);
    for (const Due& d : due) EmitRepeat(d.level, d.n, d.last);
}

Stats GetStats(size_t topN) {
    Stats out;
    AcquireSRWLockShared(&_statsLock);
    for (int i = 0; i < 4; ++i) {
        out.lines[i] = _lines[i];
        out.asked[i] = _asked[i];
    }
    out.suppressed = _suppressed;
    if (_sites)
        for (const auto& [fmt, s] : *_sites) {
            Site site;
            site.fmt.assign(fmt, strnlen(fmt, 80));
            site.level = s.level;
            site.total = s.total;
            site.suppressed = s.suppressed;
            out.sites.push_back(std::move(site));
        }
    ReleaseSRWLockShared(&_statsLock);
    std::sort(out.sites.begin(), out.sites.end(),
              [](const Site& a, const Site& b) { return a.total > b.total || (a.total == b.total && a.fmt < b.fmt); });
    if (out.sites.size() > topN) out.sites.resize(topN);
    return out;
}

} // namespace mc::core::log
