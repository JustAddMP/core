#pragma once
// Sync trace: a high-rate, line-based record of what a co-op session sent, received and did with it, for comparing
// two games after a test (a game's tools turn the traces of both machines into an error report). Separate from the
// main log, which stays readable: one file per session in <log dir>\trace\, named after the log file plus the start
// time (mafiacoop-host-20261017-151200.trace). The newest kKeepFiles files are kept.
//
// Line format: "<t> <TYPE> <fields...>", t = this machine's QueryPerformanceCounter clock in seconds (the clock a
// game's network timestamps are usually taken from), so lines of one machine order exactly; the other machine's
// lines are matched through the sender timestamps the game puts in its records. While a trace is open every line of
// the main log is copied into it too ("<t> <date time> [thread] LVL text"), so one file tells the whole story.
//
// Buffered (written about twice a second and when the buffer fills, never per line); thread-safe. A file stops
// growing at kMaxBytes (one last line says so).
#include <cstdarg>
#include <cstddef>
#include <string>

namespace mc::core::trace {

constexpr int kKeepFiles = 20;
constexpr unsigned long long kMaxBytes = 1536ull << 20;  // 1.5 GiB per session

// Opens a new trace file next to the main log (core/log.h must be open) and writes a header line
// "<t> H trace=1 wall=<local date time> qpc=<t> pid=<pid> <header>". Closes the previous one first. tag: appended to
// the file name ("" = none).
bool Open(const std::string& header, const char* tag = "");
void Close();  // flushes; a no-op when none is open
bool IsOpen();
std::wstring Path();  // the open file ("" when none)

double Now();  // the trace clock (QueryPerformanceCounter), seconds

// "<t> <type> <formatted>" (t = Now()). type: a short upper-case record name.
void Line(const char* type, const char* fmt, ...);
void LineV(const char* type, const char* fmt, va_list args);
// The same with a caller-supplied time (seconds, trace clock).
void LineAt(double t, const char* type, const char* fmt, ...);
// A finished line without the time ("<t> " is prepended; trailing CR/LF dropped); used for the copy of the main log.
void Raw(const char* text, size_t len);
void Flush();

}  // namespace mc::core::trace
