// session_diag: the session diagnostics' pure parts and the bundle writer. core/diag.h (levels, running counts that
// go back, counts kept here, deltas per window, formatting into lines, bad names refused) and core/bundle.h
// (password-like settings redacted, trace stamps, bundle names, finding a session's bundle, the newest trace, the
// role in a trace header, and a real bundle written on the worker: complete under its final name, no .tmp left, its
// entries readable with the CRCs right, the settings redacted, a missing file noted in the manifest, only the newest
// kKeepBundles kept). Exits non-zero on any failure.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/bundle.h"
#include "core/diag.h"
#include "core/log.h"
#include "support/zip_read.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

const mc::core::diag::Value* Find(const std::vector<mc::core::diag::Value>& v, const std::string& name) {
    for (const auto& x : v)
        if (x.name == name) return &x;
    return nullptr;
}

void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
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

}  // namespace

// session_diag.exe <out.zip> <trace> [<log> [<ini>]]: writes a bundle of those files with the real writer (a manual
// check, e.g. python's zipfile on a bundle of real traces; not part of the automated run).
int MakeBundle(int argc, wchar_t** argv) {
    namespace bundle = mc::core::bundle;
    bundle::Job job;
    job.zip = argv[1];
    auto base = [](const std::wstring& p) {
        const size_t at = p.find_last_of(L"\\/");
        std::string n;
        for (wchar_t c : at == std::wstring::npos ? p : p.substr(at + 1)) n += c < 128 ? static_cast<char>(c) : '_';
        return n;
    };
    job.files.push_back({argv[2], "trace/" + base(argv[2]), false});
    if (argc > 3) job.files.push_back({argv[3], "logs/" + base(argv[3]), false});
    if (argc > 4) job.files.push_back({argv[4], base(argv[4]), true});  // passwords redacted
    job.manifest = "session_diag.exe test bundle\r\n";
    const auto t0 = GetTickCount64();
    bundle::Submit(job);
    const bool ok = bundle::WaitIdle(120000) && bundle::LastWritten() == job.zip;
    std::printf("%s %ls in %.2f s\n", ok ? "wrote" : "FAILED", job.zip.c_str(), (GetTickCount64() - t0) / 1000.0);
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3) return MakeBundle(argc, argv);
    namespace diag = mc::core::diag;
    namespace bundle = mc::core::bundle;

    // ---- diag ----
    uint64_t last = 0, sum = 0, delta = 0;
    diag::Accumulate(last, sum, delta, 5);
    diag::Accumulate(last, sum, delta, 8);
    diag::Accumulate(last, sum, delta, 2);  // its owner was reset: counts from 0 again
    Check(sum == 10 && delta == 10 && last == 2, "diag: a running count that goes back adds from 0");
    diag::Reset();
    diag::Set("traffic.copies", 12);
    diag::Set("traffic.copies", 7);
    diag::Total("traffic.snaps", 3);
    diag::Total("traffic.snaps", 5);
    diag::Add("vis.ped.show");
    diag::Add("vis.ped.show", 4);
    diag::Set("bad name", 1);
    diag::Set("bad=name", 1);
    auto d1 = diag::TakeDeltas();
    const auto* copies = Find(d1, "traffic.copies");
    const auto* snaps = Find(d1, "traffic.snaps");
    const auto* shows = Find(d1, "vis.ped.show");
    Check(copies && !copies->count && copies->level == 7, "diag: a level keeps its latest value");
    Check(snaps && snaps->count && snaps->sum == 5 && snaps->delta == 5, "diag: Total turns readings into a count");
    Check(shows && shows->sum == 5 && shows->delta == 5, "diag: Add counts here");
    Check(d1.size() == 3 && diag::Dropped() == 2, "diag: names with a space or '=' are refused");
    diag::Total("traffic.snaps", 9);
    auto d2 = diag::TakeDeltas();
    snaps = Find(d2, "traffic.snaps");
    shows = Find(d2, "vis.ped.show");
    Check(snaps && snaps->delta == 4 && snaps->sum == 9 && shows && shows->delta == 0 && shows->sum == 5,
          "diag: TakeDeltas starts a new window");
    const auto lines = diag::Format(d2, 40, true);
    std::string joined;
    for (const auto& l : lines) joined += "[" + l + "]";
    Check(lines.size() == 2 && lines[0] == "traffic.copies=7 traffic.snaps=4/9" && lines[1] == "vis.ped.show=0/5",
          "diag: formatted by name, cut into lines without cutting a value", joined);
    const auto totals = diag::Format(d2, 1000, false);
    Check(totals.size() == 1 && totals[0] == "traffic.copies=7 traffic.snaps=9 vis.ped.show=5",
          "diag: totals only (the end summary)", totals.empty() ? "" : totals[0]);
    diag::Reset();
    Check(diag::Snapshot().empty(), "diag: Reset forgets everything");

    // ---- bundle helpers ----
    Check(bundle::IsSecretKey("password") && bundle::IsSecretKey("Join_Password") && bundle::IsSecretKey("api_token") &&
              !bundle::IsSecretKey("name") && !bundle::IsSecretKey("port"),
          "bundle: password-like keys");
    const std::string ini = "; MafiaCoop settings\r\nname = Harry\r\npassword = hunter2\r\n  Password=x y\r\n"
                            "[section]\r\nrequire_password_for_invites = 1\r\nempty_password =\r\nport=27960";
    const std::string red = bundle::RedactIni(ini);
    Check(red.find("hunter2") == std::string::npos && red.find("x y") == std::string::npos &&
              red.find("password = <redacted>\r\n") != std::string::npos &&
              red.find("  Password=<redacted>\r\n") != std::string::npos,
          "bundle: secret values redacted, layout kept");
    Check(red.find("name = Harry\r\n") != std::string::npos && red.find("; MafiaCoop settings\r\n") == 0 &&
              red.find("empty_password =\r\n") != std::string::npos && red.size() >= 10 &&
              red.compare(red.size() - 10, 10, "port=27960") == 0,
          "bundle: other lines, comments, empty secrets and the last line unchanged");
    Check(red.find("require_password_for_invites = 1\r\n") != std::string::npos,
          "bundle: require_* switches about passwords are kept");
    Check(bundle::TraceStamp(L"mafiacoop-host-20261020-143610.trace", L"mafiacoop-host") == "20261020-143610",
          "bundle: the stamp of a trace name");
    Check(bundle::TraceStamp(L"mafiacoop-host-20261020-143610.trace", L"mafiacoop").empty() &&
              bundle::TraceStamp(L"mafiacoop-host-2026102x-143610.trace", L"mafiacoop-host").empty() &&
              bundle::TraceStamp(L"mafiacoop-host-20261020-143610.log", L"mafiacoop-host").empty(),
          "bundle: other logs' traces and other names have none");
    Check(bundle::BundleName("MafiaCoop", "20261020-143610", "host", false) == L"MafiaCoop-20261020-143610-host.zip" &&
              bundle::BundleName("MafiaCoop", "20261020-143610", "guest", true) ==
                  L"MafiaCoop-20261020-143610-guest-recovered.zip",
          "bundle: names");
    const std::vector<std::wstring> zips = {L"MafiaCoop-20261020-143610-host.zip",
                                            L"MafiaCoop-20261020-150000-guest-recovered.zip"};
    Check(bundle::HasBundle(zips, "MafiaCoop", "20261020-143610", "host") &&
              bundle::HasBundle(zips, "MafiaCoop", "20261020-150000", "guest") &&
              !bundle::HasBundle(zips, "MafiaCoop", "20261020-143610", "guest") &&
              !bundle::HasBundle(zips, "MafiaCoop", "20261020-1436", "host"),
          "bundle: a session's bundle is found by stamp and role (recovered too)");
    const std::vector<std::wstring> traces = {L"mafiacoop-host-20261020-143610.trace",
                                              L"mafiacoop-host-20261020-145524.trace",
                                              L"mafiacoop-guest-20261020-155536.trace", L"notes.txt"};
    Check(bundle::NewestTrace(traces, L"mafiacoop-host") == L"mafiacoop-host-20261020-145524.trace" &&
              bundle::NewestTrace(traces, L"mafiacoop").empty(),
          "bundle: the newest trace of this log");
    Check(bundle::RoleOfHeader("2.1 H trace=1 wall=x qpc=2.1 pid=4 game=MafiaCoop role=guest local=1") == "guest" &&
              bundle::RoleOfHeader("2.1 SES role=host").empty(),
          "bundle: the role from a trace header");

    // ---- a real bundle ----
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mc-session-diag-test";
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    mc::core::log::Init(dir, L"b.log");
    std::string traceText;
    for (int i = 0; i < 20000; ++i)
        traceText += std::to_string(1000 + i * 0.016) + " TX sT=" + std::to_string(i * 16) + " p=1.000,2.000,3.000\r\n";
    WriteText(dir + L"\\a.trace", traceText);
    WriteText(dir + L"\\Test.ini", "name = A\r\npassword = secret123\r\n");
    const std::wstring bdir = dir + L"\\bundles";
    CreateDirectoryW(bdir.c_str(), nullptr);
    for (int i = 0; i < bundle::kKeepBundles + 3; ++i) {  // old bundles: only the newest 30 stay after the write
        const std::wstring old = bdir + L"\\Old-" + std::to_wstring(100 + i) + L".zip";
        WriteText(old, "x");
        HANDLE h = CreateFileW(old.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME ft{static_cast<DWORD>(1000000 * (i + 1)), 0x01D00000};
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
    }
    bundle::Job job;
    job.zip = bdir + L"\\Test-20261020-143610-host.zip";
    job.keepDir = bdir;
    job.files.push_back({dir + L"\\a.trace", "trace/a.trace", false});
    job.files.push_back({dir + L"\\Test.ini", "Test.ini", true});
    job.files.push_back({dir + L"\\missing.dmp", "dumps/missing.dmp", false});
    job.manifest = "Test session bundle\r\nrole: host\r\n";
    bundle::Submit(job);
    const bool idle = bundle::WaitIdle(20000);
    Check(idle && !bundle::Busy(), "bundle: written on the worker within the wait");
    Check(CountFiles(job.zip) == 1 && CountFiles(bdir + L"\\*.tmp") == 0, "bundle: under its final name, no .tmp left");
    Check(bundle::LastWritten() == job.zip, "bundle: LastWritten");
    std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
    const bool read = mc::testzip::ReadZip(mc::testzip::ReadFileBytes(job.zip), entries);
    Check(read && entries.size() == 3, "bundle: a valid zip (trace, settings, manifest)", std::to_string(entries.size()));
    if (read && entries.size() == 3) {
        const std::string t(entries[0].second.begin(), entries[0].second.end());
        const std::string i(entries[1].second.begin(), entries[1].second.end());
        const std::string m(entries[2].second.begin(), entries[2].second.end());
        Check(entries[0].first == "trace/a.trace" && t == traceText, "bundle: the trace comes back");
        Check(entries[1].first == "Test.ini" && i.find("secret123") == std::string::npos &&
                  i.find("password = <redacted>") != std::string::npos,
              "bundle: the settings with the password redacted");
        Check(entries[2].first == "manifest.txt" && m.find("Test session bundle") == 0 &&
                  m.find("trace/a.trace") != std::string::npos && m.find("dumps/missing.dmp") != std::string::npos &&
                  m.find("left out") != std::string::npos,
              "bundle: the manifest lists the files and what was left out");
    }
    Check(CountFiles(bdir + L"\\*.zip") == bundle::kKeepBundles && CountFiles(job.zip) == 1,
          "bundle: only the newest 30 bundles stay", std::to_string(CountFiles(bdir + L"\\*.zip")));
    mc::core::log::Shutdown();
    RemoveTree(dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
