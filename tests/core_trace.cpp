// core_trace: the sync trace (src/core/trace.h) and the log runs kept (src/core/log.h). In a temp folder: a trace is
// created next to the log with its header, records and a copy of every log line, lines from several threads are
// whole, Close() flushes, only the newest kKeepFiles traces stay, and five previous logs are kept. Exits non-zero
// on any failure.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/trace.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-56s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int CountFiles(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do ++n;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(p);
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

int CountLines(const std::string& text, const std::string& needle) {
    int n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (line.find(needle) != std::string::npos) ++n;
    return n;
}

}  // namespace

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mc-core-trace-test";
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    // log runs: six inits leave the current log and five previous ones
    for (int i = 0; i < 7; ++i) {
        mc::core::log::Init(dir, L"t.log");
        MC_LOG_INFO("run %d", i);
        mc::core::log::Shutdown();
    }
    Check(CountFiles(dir + L"\\t.log*") == 6, "log: the current run and five previous ones are kept",
          std::to_string(CountFiles(dir + L"\\t.log*")) + " files");
    Check(ReadAll(dir + L"\\t.log.1").find("run 5") != std::string::npos, "log: .1 is the previous run");
    Check(ReadAll(dir + L"\\t.log.5").find("run 1") != std::string::npos, "log: .5 is the oldest kept");

    mc::core::log::Init(dir, L"t.log");
    Check(mc::core::log::BaseName() == L"t" && mc::core::log::Dir() == dir, "log: Dir() and BaseName()");
    MC_LOG_INFO("before the trace");
    Check(!mc::core::trace::IsOpen(), "trace: none open at first");
    mc::core::trace::Line("XX", "dropped=%d", 1);  // no trace: a no-op
    Check(mc::core::trace::Open("role=test local=0", "a"), "trace: opens");
    const std::wstring path = mc::core::trace::Path();
    Check(path.find(L"\\trace\\t-a-") != std::wstring::npos && path.size() > 6 &&
              path.compare(path.size() - 6, 6, L".trace") == 0,
          "trace: logs\\trace\\<log name>-<tag>-<time>.trace");
    mc::core::trace::Line("TX", "sT=%u p=%.1f", 1234u, 1.5);
    MC_LOG_INFO("while the trace is open");
    // four threads, 500 lines each: every line whole
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 500; ++i) mc::core::trace::Line("MT", "thread=%d i=%d end", t, i);
        });
    for (auto& th : threads) th.join();
    mc::core::trace::Close();
    Check(!mc::core::trace::IsOpen(), "trace: closed");
    MC_LOG_INFO("after the trace");
    const std::string text = ReadAll(path);
    Check(text.find(" H trace=1 wall=") != std::string::npos && text.find("role=test local=0") != std::string::npos,
          "trace: header with the caller's fields");
    Check(text.find(" TX sT=1234 p=1.5") != std::string::npos, "trace: records with time and type");
    Check(text.find("while the trace is open") != std::string::npos, "trace: main log lines are copied in");
    Check(text.find("before the trace") == std::string::npos && text.find("after the trace") == std::string::npos,
          "trace: only while it is open");
    Check(text.find("dropped") == std::string::npos, "trace: nothing written while none is open");
    const int whole = CountLines(text, " end");
    Check(whole == 2000 && CountLines(text, " MT ") == 2000, "trace: 2000 lines from 4 threads, all whole",
          std::to_string(whole));

    // pruning: after many sessions only the newest kKeepFiles remain
    for (int i = 0; i < mc::core::trace::kKeepFiles + 5; ++i) {
        // a distinct name per session (Open names files by the second; the tag keeps them apart here)
        mc::core::trace::Open("x", ("p" + std::to_string(i)).c_str());
        mc::core::trace::Close();
    }
    const int kept = CountFiles(dir + L"\\trace\\*.trace");
    Check(kept == mc::core::trace::kKeepFiles, "trace: only the newest traces are kept", std::to_string(kept));
    mc::core::log::Shutdown();
    RemoveTree(dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
