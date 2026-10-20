#pragma once
// Session bundles: one zip per co-op session in <log dir>\bundles\ for players to send after a test, e.g.
// MafiaCoop-20261020-143610-host.zip (the session's start time, the role). It holds that session's sync trace, the
// main log, the settings file (passwords redacted), the launcher logs, the crash dumps written since the session
// started and a manifest.txt (versions, role, the files and what was cut). Written on a worker thread of its own
// (core/zip_writer.h) to <name>.tmp and renamed when complete, so a half-written bundle is never mistaken for one.
// The newest kKeepBundles stay. dll/session_log.h decides when (session end, game exit, recovery at start).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::core::bundle {

constexpr int kKeepBundles = 30;
constexpr unsigned kExitWaitMs = 20000;  // game exit: how long the exit waits for a bundle being written

struct File {
    std::wstring source;     // a file on disk
    std::string name;        // its path in the zip
    bool redactIni = false;  // a settings file: password-like values replaced (RedactIni)
};
struct Job {
    std::wstring zip;       // the bundle's final path
    std::vector<File> files;
    std::string manifest;   // the head of manifest.txt; the list of files is added
    std::wstring keepDir;   // after writing: only the newest kKeepBundles *.zip stay here ("" = no pruning)
};

// Queues a bundle (any thread); the worker writes it and logs "bundle: wrote <path> (...)".
void Submit(Job job);
// Waits until every queued bundle is written (true) or ms passed (false).
bool WaitIdle(unsigned ms);
bool Busy();
std::wstring LastWritten();  // the newest bundle written by this process ("" none)

// ---- pure helpers (tests) ----
// A settings key whose value must not leave the PC (password, passwd, pwd, secret, token; any case; not the
// require_* switches).
bool IsSecretKey(std::string_view key);
// The settings file's text with the values of secret keys replaced by "<redacted>" (comments and layout kept).
std::string RedactIni(const std::string& text);
// "<base>-yyyymmdd-hhmmss.trace" -> "yyyymmdd-hhmmss" ("" when the name is not one of base's traces).
std::string TraceStamp(const std::wstring& fileName, const std::wstring& base);
// "<mod>-<stamp>-<role>[-recovered].zip"
std::wstring BundleName(const std::string& mod, const std::string& stamp, const std::string& role, bool recovered);
// A bundle for that session exists among `bundles` (file names; a recovered one counts).
bool HasBundle(const std::vector<std::wstring>& bundles, const std::string& mod, const std::string& stamp,
               const std::string& role);
// The newest of base's traces among `traces` (file names), "" none.
std::wstring NewestTrace(const std::vector<std::wstring>& traces, const std::wstring& base);
// "role=host" from a trace's first line ("" when it has none).
std::string RoleOfHeader(const std::string& firstLine);

}  // namespace mc::core::bundle
