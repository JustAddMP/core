#pragma once
// Line logger: timestamp + thread id, every line written straight to the file (no user-mode buffering).
// File: <dir>\<fileName>; the previous runs are kept as <fileName>.1 (newest) .. <fileName>.10 (kKeepRuns, per log
// name). While a sync trace is open (core/trace.h) every line is copied into it too.
//
// Repeats: warnings and errors are rate limited per call site (the format string's address): the first
// kRepeatBurst lines of a site in kRepeatWindowS seconds are written, the rest of that window only counted; once the
// window is over (the site's next line, or FlushRepeats() from the heartbeat) one line says
// "repeated N more times in 60 s: <the last suppressed line>". Debug and info lines are never limited. Totals per
// level and per site are kept for session summaries (GetStats).
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::core::log {

enum class Level { Debug, Info, Warn, Error };

constexpr int kKeepRuns = 10;            // previous logs kept per log name: <name>.1 .. <name>.10
constexpr int kRepeatBurst = 10;         // warnings / errors of one call site written per window ...
constexpr double kRepeatWindowS = 60.0;  // ... of this many seconds; the rest is counted

// Opens <dir>\<fileName> (creating <dir>), rotating an existing file to <fileName>.1. Safe to call once.
bool Init(const std::wstring& dir, const wchar_t* fileName);
void Shutdown();
bool IsOpen();
// The folder and the file name without ".log" given to Init ("" before it): a sync trace goes next to it.
std::wstring Dir();
std::wstring BaseName();
std::wstring Path();  // the open log file ("" before Init)

void Write(Level level, const char* fmt, ...);
void WriteV(Level level, const char* fmt, va_list args);
// For the crash handler: does not block forever on the log lock (never rate limited).
void WriteUnsafe(Level level, const char* fmt, ...);

// Writes the "repeated N more times" line of every site whose window is over (call now and then, e.g. every 10 s,
// and before a summary).
void FlushRepeats();

// Totals since the process start, for summaries.
struct Site {
    std::string fmt;          // the format string (its first 80 characters)
    int level = 0;            // Level as int
    uint64_t total = 0;       // lines asked for at this site
    uint64_t suppressed = 0;  // of those, not written (rate limited)
};
struct Stats {
    uint64_t lines[4] = {};   // written per Level (Debug, Info, Warn, Error), repeat lines not included
    uint64_t asked[4] = {};   // asked for per Level (written + suppressed)
    uint64_t suppressed = 0;  // warnings / errors not written
    std::vector<Site> sites;  // warning / error sites, most frequent first (at most topN)
};
Stats GetStats(size_t topN = 10);

// The rate limiter of one call site (pure; tests). now in seconds.
struct RepeatState {
    double windowStart = -1e300;
    int inWindow = 0;
    uint64_t pending = 0;  // suppressed in the current window, not reported yet
};
// True: write this line. *reportPending (if not null) gets the count of an earlier window's suppressed lines that
// must be reported (a "repeated N more times" line) before it: non-zero only when a new window starts.
bool RepeatAllow(RepeatState& s, double now, uint64_t* reportPending);
// The window is over (now): returns its suppressed count to report (0 = nothing), and clears it.
uint64_t RepeatExpired(RepeatState& s, double now);

} // namespace mc::core::log

#define MC_LOG_DEBUG(...) ::mc::core::log::Write(::mc::core::log::Level::Debug, __VA_ARGS__)
#define MC_LOG_INFO(...)  ::mc::core::log::Write(::mc::core::log::Level::Info, __VA_ARGS__)
#define MC_LOG_WARN(...)  ::mc::core::log::Write(::mc::core::log::Level::Warn, __VA_ARGS__)
#define MC_LOG_ERROR(...) ::mc::core::log::Write(::mc::core::log::Level::Error, __VA_ARGS__)
