#pragma once
// Crash handler: unhandled-exception filter + vectored handler for fatal-only codes (stack overflow, heap
// corruption, stack buffer overrun). On a crash: logs code, faulting module+offset, registers and a short
// unwound stack (module+offset), then writes <dumpDir>\crash-<time>.dmp via DbgHelp MiniDumpWriteDump.
#include <string>

namespace mc::core::crash {

bool Install(const std::wstring& dumpDir);
// Re-installs our unhandled-exception filter if the game replaced it (the replaced one is chained).
// Cheap; call periodically (e.g. once per heartbeat).
void Reassert();
void Uninstall();

} // namespace mc::core::crash
