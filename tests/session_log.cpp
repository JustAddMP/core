// session_log: the session diagnostics end to end (src/dll/session_log.h) with a real host and guest over UDP on
// 127.0.0.1, no game: at start the newest trace without a bundle gets a "-recovered" bundle (its trace, the previous
// log, the settings redacted); while the session's trace is open the SES header (build, me, net, window, sys, every
// setting with the password redacted, paths, the guest as a peer), SCK every 5 s and SUM every 10 s (per peer, net,
// the game's core/diag.h numbers); at its close the FIN records and the session bundle under logs\bundles with the
// trace and the main log. Takes about 12 s. Exits non-zero on any failure. "-keep" leaves the files for a look.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/diag.h"
#include "core/log.h"
#include "core/trace.h"
#include "dll/session_log.h"
#include "mc/game_module.h"
#include "net/session.h"
#include "net/udp_transport.h"
#include "support/zip_read.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-66s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::vector<std::wstring> Files(const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
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

double Seconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

using Entries = std::vector<std::pair<std::string, std::vector<uint8_t>>>;
const std::vector<uint8_t>* Entry(const Entries& e, const std::string& name) {
    for (const auto& x : e)
        if (x.first == name) return &x.second;
    return nullptr;
}
std::string Text(const std::vector<uint8_t>* v) { return v ? std::string(v->begin(), v->end()) : std::string(); }

// the game layer (mc/game_module.h): a module that does nothing, with names for the test's message id
class TestModule final : public mc::GameModule {
public:
    void Init(const std::wstring&, const std::string&) override {}
    void RenderTick(float) override {}
    void OnSessionStart(mc::net::Session&) override {}
    void OnSessionEnd() override {}
    void OnPlayerLeft(mc::net::PlayerId) override {}
    void SessionTick(float) override {}
    const char* MessageName(uint16_t id) override { return id == 0x0100 ? "TestState" : nullptr; }
};

}  // namespace

mc::GameModule& mc::GetGameModule() {
    static TestModule module;
    return module;
}

int main(int argc, char** argv) {
    const bool keep = argc > 1 && std::string(argv[1]) == "-keep";  // leave the folder for a look at the files
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mc-session-log-test";
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring logs = dir + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    CreateDirectoryW((logs + L"\\trace").c_str(), nullptr);
    // the last run: its log and a trace that never got its bundle (the game was killed)
    WriteText(logs + L"\\st.log", "2020-01-01 00:00:00.000 [1] INF the old run\r\n");
    WriteText(logs + L"\\trace\\st-20200101-000000.trace",
              "1.0000 H trace=1 wall=2020-01-01T00:00:00.000 qpc=1.0 pid=1 game=McTest role=guest local=1\r\n"
              "2.0000 TX sT=1 p=1,2,3\r\n");
    WriteText(dir + L"\\McTest.ini", "name = Tester\r\npassword = hunter2\r\nport = 27999\r\n");
    mc::core::log::Init(logs, L"st.log");
    mc::core::Config::Get().Load(dir + L"\\McTest.ini");

    mc::SessionLog::Init();
    mc::SessionLog::WaitForBundles();
    const std::wstring bundles = logs + L"\\bundles";
    const std::wstring recovered = bundles + L"\\McTest-20200101-000000-guest-recovered.zip";
    Entries rec;
    const bool recOk = mc::testzip::ReadZip(mc::testzip::ReadFileBytes(recovered), rec);
    Check(recOk, "recovery: the last session's trace got a -recovered bundle");
    if (recOk) {
        Check(Text(Entry(rec, "trace/st-20200101-000000.trace")).find(" TX sT=1 ") != std::string::npos,
              "recovery: its trace");
        Check(Text(Entry(rec, "logs/st.log.1")).find("the old run") != std::string::npos,
              "recovery: the previous run's log");
        const std::string ini = Text(Entry(rec, "McTest.ini"));
        Check(ini.find("hunter2") == std::string::npos && ini.find("password = <redacted>") != std::string::npos &&
                  ini.find("name = Tester") != std::string::npos,
              "recovery: the settings, password redacted");
        Check(Text(Entry(rec, "manifest.txt")).find("recovered at the next start") != std::string::npos,
              "recovery: the manifest says why");
    }
    mc::SessionLog::Init();  // a second start: the trace has its bundle now, nothing more
    mc::SessionLog::WaitForBundles();
    Check(Files(bundles + L"\\*.zip").size() == 1, "recovery: only once");

    // a session: host + guest over UDP
    mc::net::UdpTransport ht, gt;
    mc::net::SessionConfig hc, gc;
    hc.modBuild = gc.modBuild = 1;
    hc.playerName = "Host";
    gc.playerName = "Guest";
    mc::net::Session host(ht, hc), guest(gt, gc);
    bool joined = false;
    guest.onJoined = [&] { joined = true; };
    Check(host.Host(0), "session: host binds");
    guest.Join("127.0.0.1:" + std::to_string(ht.BoundPort()));
    for (double end = Seconds() + 8; Seconds() < end && !joined; Sleep(1)) {
        host.Poll();
        guest.Poll();
    }
    Check(joined, "session: the guest joined");
    mc::SessionLog::SetSession(&host);
    mc::core::trace::Open("game=McTest role=host local=0", "");
    const std::wstring tracePath = mc::core::trace::Path();
    mc::core::diag::Set("test.copies", 3);
    mc::core::diag::Add("test.snaps", 2);
    const double t0 = Seconds();
    double last = t0;
    while (Seconds() - t0 < 10.6) {
        host.Poll();
        guest.Poll();
        host.Broadcast(0x0100, std::vector<uint8_t>(40, 1), mc::net::Reliability::Unreliable);
        guest.SendToHost(0x0100, std::vector<uint8_t>(40, 2), mc::net::Reliability::Unreliable);
        host.Flush();
        guest.Flush();
        const double now = Seconds();
        mc::SessionLog::Tick(static_cast<float>(now - last));
        last = now;
        Sleep(15);
    }
    mc::core::diag::Add("test.snaps", 5);
    mc::core::trace::Close();  // FIN, then the bundle
    mc::SessionLog::WaitForBundles();
    mc::SessionLog::SetSession(nullptr);
    const std::string text = ReadAll(tracePath);
    Check(text.find(" SES part=build mod=McTest ver=0.0.1 ") != std::string::npos, "SES: build (mod, version, core)");
    Check(text.find(" SES part=me role=host id=0 ") != std::string::npos &&
              text.find(" name=Host\r\n") != std::string::npos,
          "SES: me (role, player, name)");
    Check(text.find(" SES part=net ") != std::string::npos && text.find(" SES part=window ") != std::string::npos &&
              text.find(" SES part=sys win=") != std::string::npos,
          "SES: net, window, sys");
    Check(text.find(" SES part=ini key=password src=file value=<redacted>\r\n") != std::string::npos &&
              text.find("hunter2") == std::string::npos && text.find(" SES part=ini key=port src=file value=27999") !=
                                                               std::string::npos,
          "SES: every setting, the password redacted");
    Check(text.find(" SES part=path what=bundle path=") != std::string::npos, "SES: the paths");
    Check(text.find(" SES part=peer id=1 ") != std::string::npos && text.find(" name=Guest\r\n") != std::string::npos,
          "SES: the guest as a peer");
    int sck = 0;
    for (size_t at = text.find(" SCK sclk="); at != std::string::npos; at = text.find(" SCK sclk=", at + 1)) ++sck;
    Check(sck == 2, "SCK: every 5 s", std::to_string(sck));
    Check(text.find(" SUM part=peer id=1 rtt=") != std::string::npos && text.find(" SUM part=net dt=") != std::string::npos,
          "SUM: per peer and net after 10 s");
    Check(text.find(" SUM part=game test.copies=3 test.snaps=2/2") != std::string::npos,
          "SUM: the game's numbers with their deltas");
    Check(text.find(" FIN part=session dur=") != std::string::npos && text.find(" FIN part=peer id=1 ") != std::string::npos &&
              text.find(" FIN part=net msgs_out=") != std::string::npos,
          "FIN: session, peer, net");
    Check(text.find(" FIN part=game test.copies=3 test.snaps=7") != std::string::npos, "FIN: the game's totals");
    Check(text.find(" FIN part=bundle path=") != std::string::npos && text.find("session: ---- session end") !=
                                                                         std::string::npos,
          "FIN: the bundle's path, the end block in the copied log");
    std::wstring bundle;
    for (const auto& f : Files(bundles + L"\\McTest-*-host.zip")) bundle = bundles + L"\\" + f;
    Entries ses;
    const bool sesOk = !bundle.empty() && mc::testzip::ReadZip(mc::testzip::ReadFileBytes(bundle), ses);
    Check(sesOk, "bundle: McTest-<start>-host.zip written and valid");
    if (sesOk) {
        const std::wstring tn = tracePath.substr(tracePath.find_last_of(L'\\') + 1);
        std::string tname;
        for (wchar_t c : tn) tname += static_cast<char>(c);
        Check(Text(Entry(ses, "trace/" + tname)).find(" FIN part=bundle") !=
                  std::string::npos,
              "bundle: the whole trace, its FIN included");
        Check(Text(Entry(ses, "logs/st.log")).find("session: ---- session start") != std::string::npos,
              "bundle: the main log");
        Check(Text(Entry(ses, "McTest.ini")).find("<redacted>") != std::string::npos &&
                  Text(Entry(ses, "manifest.txt")).find("role: host, player 0") != std::string::npos,
              "bundle: settings (redacted) and the manifest");
    }
    Check(mc::core::diag::Snapshot().empty(), "diag: reset for the next session");
    const std::string log = ReadAll(logs + L"\\st.log");
    Check(log.find("bundle: wrote ") != std::string::npos && log.find("session: summary") == std::string::npos,
          "log: the bundle line; no 60 s summary in 10 s");
    host.Leave();
    guest.Leave();
    mc::core::log::Shutdown();
    if (keep)
        std::printf("kept %ls\n", dir.c_str());
    else
        RemoveTree(dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
