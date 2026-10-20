// core_trace: the sync trace (src/core/trace.h) and the log (src/core/log.h). In a temp folder: a trace is created
// next to the log with its header, records and a copy of every log line, lines from several threads are whole,
// Close() flushes and runs its hooks (the before hook's lines are in the file), only the newest kKeepFiles traces
// stay and a folder byte cap prunes the oldest; ten previous logs are kept; warnings / errors of one call site are
// rate limited (a burst, then a "repeated N more times" line) and counted. Exits non-zero on any failure.
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

    // log runs: twelve inits leave the current log and ten previous ones
    const int keep = mc::core::log::kKeepRuns;
    for (int i = 0; i < keep + 2; ++i) {
        mc::core::log::Init(dir, L"t.log");
        MC_LOG_INFO("run %d", i);
        mc::core::log::Shutdown();
    }
    Check(keep == 10 && CountFiles(dir + L"\\t.log*") == keep + 1, "log: the current run and ten previous ones are kept",
          std::to_string(CountFiles(dir + L"\\t.log*")) + " files");
    Check(ReadAll(dir + L"\\t.log.1").find("run " + std::to_string(keep)) != std::string::npos,
          "log: .1 is the previous run");
    Check(ReadAll(dir + L"\\t.log.10").find("run 1") != std::string::npos, "log: .10 is the oldest kept");

    // the repeat limiter (pure): 10 per 60 s window, the rest counted and reported when the next window starts
    {
        mc::core::log::RepeatState rs;
        uint64_t rep = 0;
        int allowed = 0;
        for (int i = 0; i < 25; ++i) allowed += mc::core::log::RepeatAllow(rs, 100.0 + i, &rep) ? 1 : 0;
        Check(allowed == 10 && rs.pending == 15, "repeats: the first 10 in a window, 15 counted");
        Check(mc::core::log::RepeatExpired(rs, 130.0) == 0, "repeats: nothing reported while the window runs");
        const bool next = mc::core::log::RepeatAllow(rs, 161.0, &rep);
        Check(next && rep == 15 && rs.pending == 0, "repeats: the next window reports the 15 and writes again");
        for (int i = 0; i < 12; ++i) mc::core::log::RepeatAllow(rs, 162.0 + i, &rep);
        Check(mc::core::log::RepeatExpired(rs, 230.0) == 3 && rs.pending == 0, "repeats: an expired window reports");
    }
    // the same in a real log: one call site 30 times, another twice
    mc::core::log::Init(dir, L"r.log");
    const auto s0 = mc::core::log::GetStats(100);
    for (int i = 0; i < 30; ++i) MC_LOG_WARN("spam warning %d", i);
    MC_LOG_ERROR("other error %d", 1);
    MC_LOG_ERROR("other error %d", 2);
    const auto s1 = mc::core::log::GetStats(100);
    const std::string rlog = ReadAll(dir + L"\\r.log");
    Check(CountLines(rlog, "spam warning") == 10 && rlog.find("spam warning 9") != std::string::npos &&
              rlog.find("spam warning 10") == std::string::npos,
          "repeats: a site's lines after the first 10 are not written", std::to_string(CountLines(rlog, "spam warning")));
    Check(CountLines(rlog, "other error") == 2, "repeats: another site is not affected");
    Check(s1.asked[2] - s0.asked[2] == 30 && s1.asked[3] - s0.asked[3] == 2 && s1.suppressed - s0.suppressed == 20,
          "stats: warnings and errors asked for, lines suppressed");
    const bool topOk = !s1.sites.empty() && s1.sites[0].fmt == "spam warning %d" && s1.sites[0].suppressed >= 20;
    Check(topOk, "stats: the most frequent site first, with its suppressed count");
    mc::core::log::FlushRepeats();  // within the window: nothing yet
    Check(ReadAll(dir + L"\\r.log").find("repeated") == std::string::npos, "repeats: no report inside the window");
    mc::core::log::Shutdown();

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

    // close hooks: before (the file still open: its line goes in), after (closed, with the path)
    {
        static std::wstring beforePath, afterPath;
        static bool afterSawOpen = true;
        mc::core::trace::SetCloseHooks(
            [](const std::wstring& p) {
                beforePath = p;
                mc::core::trace::Line("FIN", "part=test");
            },
            [](const std::wstring& p) {
                afterPath = p;
                afterSawOpen = mc::core::trace::IsOpen();
            });
        mc::core::trace::Open("hooks", "h");
        const std::wstring hp = mc::core::trace::Path();
        mc::core::trace::Close();
        mc::core::trace::SetCloseHooks(nullptr, nullptr);
        Check(beforePath == hp && afterPath == hp && !afterSawOpen, "trace: close hooks before and after, with the path");
        Check(ReadAll(hp).find(" FIN part=test") != std::string::npos, "trace: the before hook's line is in the file");
    }

    // the folder's byte cap: PruneFolder keeps the newest that fit
    {
        const std::wstring pd = dir + L"\\prune";
        CreateDirectoryW(pd.c_str(), nullptr);
        for (int i = 0; i < 6; ++i) {
            std::ofstream f(pd + L"\\f" + std::to_wstring(i) + L".trace", std::ios::binary);
            f << std::string(1000, 'x');
            f.close();
            HANDLE h = CreateFileW((pd + L"\\f" + std::to_wstring(i) + L".trace").c_str(), FILE_WRITE_ATTRIBUTES, 0,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            FILETIME ft{static_cast<DWORD>(1000000 * (i + 1)), 0x01D00000};
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
        const int gone = mc::core::trace::PruneFolder(pd, L"*.trace", 10, 3500);
        Check(gone == 3 && CountFiles(pd + L"\\*.trace") == 3 && CountFiles(pd + L"\\f5.trace") == 1 &&
                  CountFiles(pd + L"\\f0.trace") == 0,
              "trace: a byte cap removes the oldest", std::to_string(gone));
        Check(mc::core::trace::kMaxBytes == (1ull << 30) && mc::core::trace::kMaxFolderBytes == (4ull << 30),
              "trace: 1 GiB per file, 4 GiB per folder");
    }

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
