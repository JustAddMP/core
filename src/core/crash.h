#pragma once
// Crash handler: unhandled-exception filter (every exception nobody handled, stack overflow included) + vectored
// handler for the codes that always end the process (heap corruption, stack buffer overrun). First-chance
// exceptions that may still be handled (a caught stack overflow, a C++ throw, ...) are left alone. On a crash: logs
// code, faulting module+offset, registers and a short unwound stack (module+offset), then writes
// <dumpDir>\crash-<time>.dmp via DbgHelp MiniDumpWriteDump. The work runs on a writer thread started by Install
// (its own stack). One dump per process; a dump that could not be written does not use it up.
// Stack overflow: the unhandled filter only runs if the thread has stack kept back for it (ReserveThreadStack). For
// a thread without that reserve, the vectored handler writes one crash-<time>-firstchance.dmp instead (it may turn
// out to be handled), which does not use up the real crash dump.
#include <string>

namespace mc::core::crash {

bool Install(const std::wstring& dumpDir);  // also reserves stack on the calling thread
// Keeps 64 KB of the calling thread's stack back (SetThreadStackGuarantee) so an unhandled stack overflow on it
// still reaches the crash handler. Only on threads with >= 1 MB of stack; false otherwise. Call once per thread
// (the mod dll: DLL_PROCESS_ATTACH / DLL_THREAD_ATTACH).
bool ReserveThreadStack();
// Re-installs our unhandled-exception filter if the game replaced it (the replaced one is chained).
// Cheap; call periodically (e.g. once per heartbeat).
void Reassert();
void Uninstall();

} // namespace mc::core::crash
