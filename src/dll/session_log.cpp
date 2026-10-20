#include "dll/session_log.h"

#include "core/bundle.h"
#include "core/config.h"
#include "core/diag.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/trace.h"
#include "core/zip_writer.h"
#include "dll/window_place.h"
#include "mc/core_version.h"
#include "mc/game_info.h"
#include "mc/game_module.h"
#include "net/session.h"
#include "render/present_hook.h"

#include <windows.h>
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc {
namespace {

constexpr double kSckEvery = 5.0, kSumEvery = 10.0, kLogEvery = 60.0, kRepeatsEvery = 10.0;
constexpr size_t kLineChars = 1500;  // a record's payload (core/trace.h lines are cut at 2 KB)

struct PeerAcc {
    std::string name;
    double firstAt = 0, lastAt = 0;
    net::PeerStats last;
    bool haveLast = false;
    uint64_t out = 0, in = 0, pktOut = 0, pktIn = 0, resends = 0;
    double rttSum = 0, lossSum = 0;
    int samples = 0;
    float rttMax = 0, lossMax = 0;
};

struct State {
    std::recursive_mutex mu;
    SessionContextFn ctxFn = nullptr;
    net::Session* session = nullptr;
    bool sampleOk = true;
    // the session whose trace is open
    bool inSession = false;
    std::wstring tracePath, bundlePath;
    std::string stamp, role;
    uint8_t local = 0;
    double startT = 0;  // trace clock
    FILETIME startFt{};
    std::string startWall;
    std::map<uint8_t, uint32_t> seen;  // player id -> joinSeq written
    std::map<uint8_t, PeerAcc> peers;
    std::map<uint16_t, net::MsgTraffic> traffic;  // at the last SUM
    double sckAt = 0, sumAt = 0, logAt = 0, checkAt = 0;
    uint64_t warn0 = 0, err0 = 0, supp0 = 0, warnSum = 0, errSum = 0, suppSum = 0, warnLog = 0, errLog = 0;
    std::map<std::string, uint64_t> sites0;  // log sites' totals at the session start
    std::map<std::string, uint64_t> diagAtLog;
    // always
    double now = 0, repeatsAt = 0;
    double fpsT = 0;
    unsigned fpsN = 0;
    float fpsMaxDt = 0;
    double fps = 0, fpsMin = 0;  // the last full window (10 s)
};

State& S() {
    static State* s = new State;  // never destroyed (exit hooks run late)
    return *s;
}

std::atomic<int> _crcState{0};  // 0 not started, 1 running, 2 done, 3 failed
std::atomic<uint32_t> _exeCrc{0};

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One token for key=value records (spaces and '=' would split it).
std::string Token(std::string v) {
    if (v.empty()) return "-";
    for (char& c : v)
        if (c == ' ' || c == '=' || c == '\t' || c == '\r' || c == '\n') c = '_';
    return v;
}

std::wstring FileName(const std::wstring& path) {
    const size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? path : path.substr(at + 1);
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

std::string WallNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[40];
    std::snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return b;
}

std::string StampNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[24];
    std::snprintf(b, sizeof b, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return b;
}

std::wstring LogDir() { return core::log::Dir(); }
std::wstring BundleDir() { return LogDir() + L"\\bundles"; }

void StartExeCrc() {
    int expected = 0;
    if (!_crcState.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        HANDLE f = CreateFileW(ExePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            _crcState = 3;
            return;
        }
        std::vector<uint8_t> buf(1 << 20);
        uint32_t crc = 0;
        DWORD got = 0;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
            crc = core::zip::Crc32(crc, buf.data(), got);
        CloseHandle(f);
        _exeCrc = crc;
        _crcState = 2;
    }).detach();
}

std::string ExeCrcText(std::string* label) {
    if (_crcState != 2) return _crcState == 3 ? "failed" : "pending";
    const uint32_t crc = _exeCrc;
    const GameInfo& gi = GetGameInfo();
    *label = "unknown build";
    for (size_t i = 0; i < gi.buildCount; ++i)
        if (gi.builds[i].crc32 == crc) *label = gi.builds[i].label;
    char b[16];
    std::snprintf(b, sizeof b, "%08x", crc);
    return b;
}

std::string OsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")); fn && fn(&v) == 0) {
            char b[48];
            std::snprintf(b, sizeof b, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
            return b;
        }
    return "?";
}

// "key=value" of every setting: the file's, then launch overrides (environment) not in the file.
struct Setting {
    std::string key, value, src;
};
std::vector<Setting> Settings() {
    std::vector<Setting> out;
    const auto& cfg = core::Config::Get();
    std::map<std::string, std::string> keys;  // key -> source
    for (const auto& [k, v] : cfg.Values()) keys[k] = "file";
    const std::string prefix = GetGameInfo().envPrefix;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* e = env; *e; e += wcslen(e) + 1) {
            const std::string var = Narrow(e);
            if (var.compare(0, prefix.size(), prefix) != 0) continue;
            const size_t eq = var.find('=');
            if (eq == std::string::npos || eq <= prefix.size()) continue;
            std::string k = var.substr(prefix.size(), eq - prefix.size());
            for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = keys.find(k);
            if (it == keys.end()) keys[k] = "env";
            else if (cfg.GetString(k) != cfg.GetFileString(k)) it->second = "env";
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [k, src] : keys) {
        std::string v = cfg.GetString(k);
        if (core::bundle::IsSecretKey(k) && !v.empty()) v = "<redacted>";
        out.push_back({k, v, src});
    }
    return out;
}

void Ses(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    core::trace::LineV("SES", fmt, a);
    va_end(a);
}

void WriteWindow() {
    HWND hwnd = static_cast<HWND>(window_place::Window());
    RECT wr{}, cr{};
    LONG_PTR style = 0;
    int maximized = 0, minimized = 0;
    if (hwnd && IsWindow(hwnd)) {
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        maximized = IsZoomed(hwnd) ? 1 : 0;
        minimized = IsIconic(hwnd) ? 1 : 0;
    }
    unsigned bbW = 0, bbH = 0, fmt = 0, windowed = 1, refresh = 0;
    BOOL fullscreen = FALSE;
    if (IDXGISwapChain* sc = render::GameSwapChain()) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            bbW = d.BufferDesc.Width;
            bbH = d.BufferDesc.Height;
            fmt = static_cast<unsigned>(d.BufferDesc.Format);
            windowed = d.Windowed ? 1 : 0;
            if (d.BufferDesc.RefreshRate.Denominator)
                refresh = d.BufferDesc.RefreshRate.Numerator / d.BufferDesc.RefreshRate.Denominator;
        }
        IDXGIOutput* target = nullptr;
        if (SUCCEEDED(sc->GetFullscreenState(&fullscreen, &target)) && target) target->Release();
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 0;
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    if (hwnd)
        if (auto fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))
            dpi = fn(hwnd);
    const RECT& m = mi.rcMonitor;
    const std::string monName = Token(Narrow(mi.szDevice));
    Ses("part=window x=%ld y=%ld w=%ld h=%ld client=%ldx%ld bb=%ux%u fmt=%u windowed=%u fullscreen=%d refresh=%u "
        "style=0x%llx maximized=%d minimized=%d monitor=%s mon=%ld,%ld,%ldx%ld primary=%d dpi=%u",
        wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom, bbW, bbH, fmt, windowed,
        fullscreen ? 1 : 0, refresh, static_cast<unsigned long long>(style), maximized, minimized, monName.c_str(),
        m.left, m.top, m.right - m.left, m.bottom - m.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0, dpi);
    MC_LOG_INFO("session: window %ldx%ld at %ld,%ld (client %ldx%ld%s%s), backbuffer %ux%u %s, monitor %s %ldx%ld%s, "
                "dpi %u",
                wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top, cr.right, cr.bottom,
                maximized ? ", maximized" : "", minimized ? ", minimized" : "", bbW, bbH,
                fullscreen ? "fullscreen" : (windowed ? "windowed" : "not windowed"), monName.c_str(),
                m.right - m.left, m.bottom - m.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "", dpi);
}

void WritePeer(State& s, const net::PlayerInfo& p, bool later) {
    net::Session& session = *s.session;
    const uint64_t steam = p.peer ? session.GetTransport().PeerSteamId(p.peer) : 0;
    const std::string addr = p.peer ? session.GetTransport().PeerAddress(p.peer) : std::string();
    const bool me = p.id == session.LocalId();
    Ses("part=peer id=%u seq=%u me=%d host=%d steam=%llu via=%s addr=%s model=%s later=%d name=%s", p.id, p.joinSeq,
        me ? 1 : 0, p.id == net::kHostPlayer ? 1 : 0, static_cast<unsigned long long>(steam),
        me ? "-" : (steam ? "steam" : "udp"), Token(addr).c_str(), Token(p.model).c_str(), later ? 1 : 0,
        p.name.c_str());
    if (!me)
        MC_LOG_INFO("session: peer %u '%s'%s: %s%s%s, Steam id %llu, model '%s' (join #%u)", p.id, p.name.c_str(),
                    p.id == net::kHostPlayer ? " (host)" : "", steam ? "over Steam" : "over UDP",
                    addr.empty() ? "" : " from ", addr.c_str(), static_cast<unsigned long long>(steam),
                    p.model.c_str(), p.joinSeq);
    if (me) return;
    PeerAcc& acc = s.peers[p.id];
    acc.name = p.name;
    if (acc.firstAt == 0) acc.firstAt = core::trace::Now();
    // a new connection: its transport counters start anew; the first SUM counts from here
    acc.haveLast = p.peer != net::kInvalidPeer && session.GetTransport().GetStats(p.peer, acc.last);
}

void StartSession(State& s, const std::wstring& path) {
    net::Session& session = *s.session;
    const GameInfo& gi = GetGameInfo();
    const SessionContext ctx = s.ctxFn ? s.ctxFn() : SessionContext{};
    s.inSession = true;
    s.tracePath = path;
    s.role = session.IsHost() ? "host" : "guest";
    s.local = session.LocalId();
    s.stamp = core::bundle::TraceStamp(FileName(path), core::log::BaseName());
    if (s.stamp.empty()) s.stamp = StampNow();
    s.bundlePath = BundleDir() + L"\\" + core::bundle::BundleName(gi.modName, s.stamp, s.role, false);
    s.startT = core::trace::Now();
    GetSystemTimeAsFileTime(&s.startFt);
    s.startWall = WallNow();
    s.seen.clear();
    s.peers.clear();
    s.traffic = session.Traffic();
    s.sckAt = s.sumAt = s.logAt = s.startT;
    const auto ls = core::log::GetStats(1000);
    s.warn0 = s.warnSum = s.warnLog = ls.asked[2];
    s.err0 = s.errSum = s.errLog = ls.asked[3];
    s.supp0 = s.suppSum = ls.suppressed;
    s.sites0.clear();
    for (const auto& site : ls.sites) s.sites0[site.fmt] = site.total;
    s.diagAtLog.clear();
    for (const auto& v : core::diag::Snapshot())
        if (v.count) s.diagAtLog[v.name] = v.sum;

    const net::PlayerInfo* me = session.FindPlayer(s.local);
    const std::string myName = me ? me->name : ctx.persona;
    std::string label;
    const std::string crc = ExeCrcText(&label);
    const auto& mm = core::mem::MainModule();
    std::string built = __DATE__ "_" __TIME__;
    for (char& c : built)
        if (c == ' ') c = '_';
    MC_LOG_INFO("session: ---- session start: %s, player %u '%s' ----", s.role.c_str(), s.local, myName.c_str());
    MC_LOG_INFO("session: %s %s (core %s), session build %u, protocol %u, core built %s, exe crc %s (%s), exe "
                "timestamp 0x%08x",
                gi.modName, gi.modVersion, kCoreVersion, ctx.build, net::kProtocolVersion, built.c_str(), crc.c_str(),
                label.c_str(), mm.timeDateStamp);
    Ses("part=build mod=%s ver=%s core=%s build=%u proto=%u core_built=%s exe_crc=%s exe_ts=0x%08x exe_label=%s",
        Token(gi.modName).c_str(), Token(gi.modVersion).c_str(), kCoreVersion, ctx.build, net::kProtocolVersion,
        built.c_str(), crc.c_str(), mm.timeDateStamp, Token(label).c_str());
    Ses("part=me role=%s id=%u steam=%llu steam_ready=%d name=%s", s.role.c_str(), s.local,
        static_cast<unsigned long long>(ctx.steamId), ctx.steamReady ? 1 : 0, myName.c_str());
    Ses("part=persona name=%s", ctx.persona.c_str());
    MC_LOG_INFO("session: me: %s, player %u '%s', Steam %s (id %llu, persona '%s')", s.role.c_str(), s.local,
                myName.c_str(), ctx.steamReady ? "ready" : "not ready", static_cast<unsigned long long>(ctx.steamId),
                ctx.persona.c_str());
    Ses("part=net mode=%s transport=%s port=%u lan=%s lobby=%llu relay_kbs=%.0f password=%d target=%s",
        Token(ctx.mode).c_str(), Token(ctx.transport.empty() ? session.GetTransport().Name() : ctx.transport).c_str(),
        ctx.udpPort, Token(ctx.lanAddress).c_str(), static_cast<unsigned long long>(ctx.lobbyId), ctx.relayKBs,
        ctx.password ? 1 : 0, ctx.target.c_str());
    MC_LOG_INFO("session: net: %s over %s, UDP port %u, LAN %s, Steam lobby %llu, relay budget %.0f KB/s per peer, "
                "password %s%s%s",
                ctx.mode.c_str(), session.GetTransport().Name(), ctx.udpPort,
                ctx.lanAddress.empty() ? "-" : ctx.lanAddress.c_str(), static_cast<unsigned long long>(ctx.lobbyId),
                ctx.relayKBs, ctx.password ? "set" : "none", ctx.target.empty() ? "" : ", target ",
                ctx.target.c_str());
    WriteWindow();
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const std::string os = OsVersion();
    Ses("part=sys win=%s cpus=%lu ram_mb=%llu ram_free_mb=%llu fps=%.1f fps_min=%.1f", os.c_str(),
        si.dwNumberOfProcessors, static_cast<unsigned long long>(ms.ullTotalPhys >> 20),
        static_cast<unsigned long long>(ms.ullAvailPhys >> 20), s.fps, s.fpsMin);
    MC_LOG_INFO("session: Windows %s, %lu CPUs, %llu MB RAM (%llu MB free), fps %.1f (lowest %.1f) in the last 10 s",
                os.c_str(), si.dwNumberOfProcessors, static_cast<unsigned long long>(ms.ullTotalPhys >> 20),
                static_cast<unsigned long long>(ms.ullAvailPhys >> 20), s.fps, s.fpsMin);
    std::string all;
    for (const Setting& st : Settings()) {
        Ses("part=ini key=%s src=%s value=%s", Token(st.key).c_str(), st.src.c_str(), st.value.c_str());
        all += (all.empty() ? "" : ", ") + st.key + " = " + st.value + (st.src == "file" ? "" : " (" + st.src + ")");
    }
    MC_LOG_INFO("session: settings: %s", all.empty() ? "(none)" : all.c_str());
    const std::wstring logPath = core::log::Path();
    const std::wstring iniPath = core::Config::Get().Path();
    struct P {
        const char* what;
        std::wstring path;
    };
    for (const P& p : {P{"log", logPath}, P{"trace", path}, P{"bundle", s.bundlePath}, P{"ini", iniPath},
                       P{"exe", ExePath()}, P{"dumps", LogDir()}}) {
        Ses("part=path what=%s path=%s", p.what, Narrow(p.path).c_str());
    }
    MC_LOG_INFO("session: files: log %s, trace %s, bundle at the end %s", Narrow(logPath).c_str(),
                Narrow(path).c_str(), Narrow(s.bundlePath).c_str());
    for (const auto& p : session.Players()) {
        WritePeer(s, p, false);
        s.seen[p.id] = p.joinSeq;
    }
}

// Transport counters of every remote peer into its PeerAcc; returns per-peer interval values for SUM lines.
struct PeerNow {
    uint8_t id = 0;
    net::PeerStats st;
    double kbo = 0, kbi = 0, pko = 0, pki = 0;
    uint32_t resends = 0;
};
std::vector<PeerNow> SamplePeers(State& s, double dt) {
    std::vector<PeerNow> out;
    if (!s.session || !s.sampleOk) return out;
    net::Session& session = *s.session;
    const double now = core::trace::Now();
    for (const auto& p : session.Players()) {
        if (p.id == session.LocalId() || p.peer == net::kInvalidPeer) continue;
        net::PeerStats st;
        if (!session.GetTransport().GetStats(p.peer, st)) continue;
        PeerAcc& a = s.peers[p.id];
        if (a.name.empty()) a.name = p.name;
        PeerNow n;
        n.id = p.id;
        n.st = st;
        if (a.haveLast) {
            auto d = [](uint64_t x, uint64_t y) { return x >= y ? x - y : 0; };
            const uint64_t bo = d(st.bytesSent, a.last.bytesSent), bi = d(st.bytesReceived, a.last.bytesReceived);
            const uint64_t po = d(st.packetsSent, a.last.packetsSent);
            const uint64_t pi = d(st.packetsReceived, a.last.packetsReceived);
            const uint32_t rs = st.resends >= a.last.resends ? st.resends - a.last.resends : 0;
            a.out += bo;
            a.in += bi;
            a.pktOut += po;
            a.pktIn += pi;
            a.resends += rs;
            if (dt > 0) {
                n.kbo = static_cast<double>(bo) / 1024.0 / dt;
                n.kbi = static_cast<double>(bi) / 1024.0 / dt;
                n.pko = static_cast<double>(po) / dt;
                n.pki = static_cast<double>(pi) / dt;
            }
            n.resends = rs;
        }
        a.last = st;
        a.haveLast = true;
        a.lastAt = now;
        if (a.firstAt == 0) a.firstAt = now;
        const float rtt = st.rttMs > 0 ? st.rttMs : p.rttMs;
        a.rttSum += rtt;
        a.lossSum += st.loss;
        ++a.samples;
        a.rttMax = std::max(a.rttMax, rtt);
        a.lossMax = std::max(a.lossMax, st.loss);
        out.push_back(n);
    }
    return out;
}

// Per lane: KB/s out / in over dt from the session's per-message counters; also the totals.
std::string Lanes(State& s, double dt, double* kbo, double* kbi) {
    *kbo = *kbi = 0;
    if (!s.session || !s.sampleOk) return "-";
    const auto& now = s.session->Traffic();
    double out[net::kLanes] = {}, in[net::kLanes] = {};
    for (const auto& [id, t] : now) {
        net::MsgTraffic prev;
        if (auto it = s.traffic.find(id); it != s.traffic.end()) prev = it->second;
        const uint8_t lane = std::min<uint8_t>(s.session->LaneOf(id), net::kLanes - 1);
        out[lane] += static_cast<double>(t.sentBytes >= prev.sentBytes ? t.sentBytes - prev.sentBytes : t.sentBytes);
        in[lane] += static_cast<double>(t.recvBytes >= prev.recvBytes ? t.recvBytes - prev.recvBytes : t.recvBytes);
    }
    s.traffic = now;
    std::string lanes;
    for (int l = 0; l < net::kLanes; ++l) {
        if (out[l] == 0 && in[l] == 0) continue;
        char b[48];
        std::snprintf(b, sizeof b, "%s%d:%.1f/%.1f", lanes.empty() ? "" : ",", l, dt > 0 ? out[l] / 1024 / dt : 0.0,
                      dt > 0 ? in[l] / 1024 / dt : 0.0);
        lanes += b;
        *kbo += dt > 0 ? out[l] / 1024 / dt : 0;
        *kbi += dt > 0 ? in[l] / 1024 / dt : 0;
    }
    return lanes.empty() ? "-" : lanes;
}

void Summary(State& s, double now) {
    const double dt = now - s.sumAt;
    s.sumAt = now;
    for (const PeerNow& p : SamplePeers(s, dt)) {
        const PeerAcc& a = s.peers[p.id];
        (void)a;
        core::trace::Line("SUM", "part=peer id=%u rtt=%.1f loss=%.2f kbo=%.2f kbi=%.2f pko=%.0f pki=%.0f rs=%u q=%u",
                          p.id, p.st.rttMs, p.st.loss * 100.0, p.kbo, p.kbi, p.pko, p.pki, p.resends,
                          p.st.reliableQueued);
    }
    double kbo = 0, kbi = 0;
    const std::string lanes = Lanes(s, dt, &kbo, &kbi);
    const auto ls = core::log::GetStats(0);
    const uint64_t w = ls.asked[2], e = ls.asked[3];
    const double off = s.session && s.sampleOk ? s.session->ClockOffsetS() * 1000.0 : 0.0;
    const double err = s.session && s.sampleOk ? s.session->ClockErrorMs() : 0.0;
    core::trace::Line("SUM", "part=net dt=%.1f kbo=%.2f kbi=%.2f lanes=%s fps=%.1f fps_min=%.1f warn=%llu/%llu "
                             "err=%llu/%llu supp=%llu/%llu off=%.2f off_err=%.2f",
                      dt, kbo, kbi, lanes.c_str(), s.fps, s.fpsMin, static_cast<unsigned long long>(w - s.warnSum),
                      static_cast<unsigned long long>(w - s.warn0), static_cast<unsigned long long>(e - s.errSum),
                      static_cast<unsigned long long>(e - s.err0),
                      static_cast<unsigned long long>(ls.suppressed - s.suppSum),
                      static_cast<unsigned long long>(ls.suppressed - s.supp0), off, err);
    s.warnSum = w;
    s.errSum = e;
    s.suppSum = ls.suppressed;
    for (const std::string& line : core::diag::Format(core::diag::TakeDeltas(), kLineChars, true))
        core::trace::Line("SUM", "part=game %s", line.c_str());
}

void LogSummary(State& s, double now) {
    const double span = now - s.logAt;
    s.logAt = now;
    std::string peers;
    for (const auto& [id, a] : s.peers) {
        if (!a.haveLast) continue;
        char b[200];
        std::snprintf(b, sizeof b, "%splayer %u '%s' rtt %.0f ms (max %.0f) loss %.1f %% (max %.1f)",
                      peers.empty() ? "" : "; ", id, a.name.c_str(), a.last.rttMs, a.rttMax, a.last.loss * 100.0,
                      a.lossMax * 100.0);
        peers += b;
    }
    const auto ls = core::log::GetStats(0);
    std::vector<core::diag::Value> vals = core::diag::Snapshot();
    for (auto& v : vals)
        if (v.count) {
            const uint64_t was = s.diagAtLog[v.name];
            v.delta = v.sum >= was ? v.sum - was : v.sum;
            s.diagAtLog[v.name] = v.sum;
        }
    std::string game;
    for (const std::string& line : core::diag::Format(vals, 100000, true)) game += line;
    if (game.size() > 1400) game = game.substr(0, 1400) + " ...";
    MC_LOG_INFO("session: summary (%.0f s): %s; fps %.1f (lowest %.1f); warnings +%llu, errors +%llu; %s", span,
                peers.empty() ? "no peers" : peers.c_str(), s.fps, s.fpsMin,
                static_cast<unsigned long long>(ls.asked[2] - s.warnLog),
                static_cast<unsigned long long>(ls.asked[3] - s.errLog), game.c_str());
    s.warnLog = ls.asked[2];
    s.errLog = ls.asked[3];
}

void Sck(State& s) {
    if (!s.session || !s.sampleOk) return;
    net::Session& session = *s.session;
    const net::PlayerInfo* host = session.FindPlayer(net::kHostPlayer);
    const bool isHost = session.IsHost();
    core::trace::Line("SCK", "sclk=%.1f off=%.2f err=%.2f rtt=%.1f synced=%d", session.SessionTime() * 1000.0,
                      session.ClockOffsetS() * 1000.0, session.ClockErrorMs(),
                      isHost ? -1.0 : (host ? host->rttMs : -1.0), session.ClockSynced() ? 1 : 0);
}

std::string Mb(uint64_t b) {
    char x[32];
    std::snprintf(x, sizeof x, "%.2f", static_cast<double>(b) / 1e6);
    return x;
}

// The end of the session: FIN records and the log block (the trace is still open).
void Finish(State& s) {
    if (!s.inSession) return;
    const double now = core::trace::Now();
    if (s.session && s.sampleOk) SamplePeers(s, now - s.sumAt);
    const double dur = now - s.startT;
    core::log::FlushRepeats();
    std::string ids;
    for (const auto& [id, a] : s.peers) ids += (ids.empty() ? "" : ",") + std::to_string(id);
    core::trace::Line("FIN", "part=session dur=%.1f role=%s local=%u peers=%s start=%s end=%s", dur, s.role.c_str(),
                      s.local, ids.empty() ? "-" : ids.c_str(), s.startWall.c_str(), WallNow().c_str());
    MC_LOG_INFO("session: ---- session end: %s, %.0f s (%.1f min), players seen: %s ----", s.role.c_str(), dur,
                dur / 60.0, ids.empty() ? "-" : ids.c_str());
    for (const auto& [id, a] : s.peers) {
        const double span = std::max(1.0, a.lastAt - a.firstAt);
        const double rtt = a.samples ? a.rttSum / a.samples : 0, loss = a.samples ? a.lossSum / a.samples * 100 : 0;
        core::trace::Line("FIN", "part=peer id=%u secs=%.0f out_mb=%s in_mb=%s kbo=%.2f kbi=%.2f pko=%llu pki=%llu "
                                 "rtt_avg=%.1f rtt_max=%.1f loss_avg=%.2f loss_max=%.2f resends=%llu name=%s",
                          id, span, Mb(a.out).c_str(), Mb(a.in).c_str(), a.out / 1024.0 / span, a.in / 1024.0 / span,
                          static_cast<unsigned long long>(a.pktOut), static_cast<unsigned long long>(a.pktIn), rtt,
                          a.rttMax, loss, a.lossMax * 100.0, static_cast<unsigned long long>(a.resends),
                          a.name.c_str());
        MC_LOG_INFO("session:   player %u '%s': %.0f s, out %s MB, in %s MB, rtt %.0f ms on average (worst %.0f), "
                    "loss %.1f %% on average (worst %.1f %%), %llu resends",
                    id, a.name.c_str(), span, Mb(a.out).c_str(), Mb(a.in).c_str(), rtt, a.rttMax, loss,
                    a.lossMax * 100.0, static_cast<unsigned long long>(a.resends));
    }
    if (s.session && s.sampleOk) {
        uint64_t mo = 0, mi = 0, bo = 0, bi = 0;
        std::vector<std::pair<uint64_t, uint16_t>> top;
        uint64_t laneOut[net::kLanes] = {}, laneIn[net::kLanes] = {};
        for (const auto& [id, t] : s.session->Traffic()) {
            mo += t.sentMsgs;
            mi += t.recvMsgs;
            bo += t.sentBytes;
            bi += t.recvBytes;
            const uint8_t lane = std::min<uint8_t>(s.session->LaneOf(id), net::kLanes - 1);
            laneOut[lane] += t.sentBytes;
            laneIn[lane] += t.recvBytes;
            top.push_back({t.sentBytes + t.recvBytes, id});
        }
        std::string lanes;
        for (int l = 0; l < net::kLanes; ++l)
            if (laneOut[l] || laneIn[l])
                lanes += (lanes.empty() ? "" : ",") + std::to_string(l) + ":" + Mb(laneOut[l]) + "/" + Mb(laneIn[l]);
        std::sort(top.rbegin(), top.rend());
        std::string msgs;
        for (size_t i = 0; i < top.size() && i < 12; ++i) {
            const auto& t = s.session->Traffic().at(top[i].second);
            const char* n = GetGameModule().MessageName(top[i].second);
            char b[96];
            std::snprintf(b, sizeof b, "%s%s:%llu/%llu:%s/%s", msgs.empty() ? "" : ",",
                          n ? n : ("0x" + std::to_string(top[i].second)).c_str(),
                          static_cast<unsigned long long>(t.sentMsgs), static_cast<unsigned long long>(t.recvMsgs),
                          Mb(t.sentBytes).c_str(), Mb(t.recvBytes).c_str());
            msgs += b;
        }
        core::trace::Line("FIN", "part=net msgs_out=%llu msgs_in=%llu out_mb=%s in_mb=%s lanes=%s",
                          static_cast<unsigned long long>(mo), static_cast<unsigned long long>(mi), Mb(bo).c_str(),
                          Mb(bi).c_str(), lanes.empty() ? "-" : lanes.c_str());
        core::trace::Line("FIN", "part=msgs top=%s", msgs.empty() ? "-" : msgs.c_str());
        MC_LOG_INFO("session:   messages: %llu sent (%s MB), %llu received (%s MB); per lane MB out/in %s",
                    static_cast<unsigned long long>(mo), Mb(bo).c_str(), static_cast<unsigned long long>(mi),
                    Mb(bi).c_str(), lanes.empty() ? "-" : lanes.c_str());
        MC_LOG_INFO("session:   top messages (sent/received count: MB): %s", msgs.c_str());
    }
    const auto values = core::diag::Snapshot();
    for (const std::string& line : core::diag::Format(values, kLineChars, false)) {
        core::trace::Line("FIN", "part=game %s", line.c_str());
        MC_LOG_INFO("session:   game: %s", line.c_str());
    }
    const auto ls = core::log::GetStats(1000);
    core::trace::Line("FIN", "part=log warn=%llu err=%llu supp=%llu dropped_names=%llu",
                      static_cast<unsigned long long>(ls.asked[2] - s.warn0),
                      static_cast<unsigned long long>(ls.asked[3] - s.err0),
                      static_cast<unsigned long long>(ls.suppressed - s.supp0),
                      static_cast<unsigned long long>(core::diag::Dropped()));
    std::vector<std::pair<uint64_t, const core::log::Site*>> sites;
    for (const auto& site : ls.sites) {
        auto it = s.sites0.find(site.fmt);
        const uint64_t n = site.total - (it == s.sites0.end() ? 0 : std::min(it->second, site.total));
        if (n) sites.push_back({n, &site});
    }
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    MC_LOG_INFO("session:   warnings %llu, errors %llu (%llu lines not written: rate limited)%s",
                static_cast<unsigned long long>(ls.asked[2] - s.warn0),
                static_cast<unsigned long long>(ls.asked[3] - s.err0),
                static_cast<unsigned long long>(ls.suppressed - s.supp0), sites.empty() ? "" : "; most frequent:");
    for (size_t i = 0; i < sites.size() && i < 5; ++i) {
        const core::log::Site& site = *sites[i].second;
        core::trace::Line("FIN", "part=repeat n=%llu level=%s fmt=%s", static_cast<unsigned long long>(sites[i].first),
                          site.level == 3 ? "ERR" : "WRN", site.fmt.c_str());
        MC_LOG_INFO("session:     %llux %s \"%s\"", static_cast<unsigned long long>(sites[i].first),
                    site.level == 3 ? "ERR" : "WRN", site.fmt.c_str());
    }
    core::trace::Line("FIN", "part=bundle path=%s", Narrow(s.bundlePath).c_str());
    MC_LOG_INFO("session:   bundle: %s (being written)", Narrow(s.bundlePath).c_str());
}

// ---- bundles --------------------------------------------------------------------------------------------------------
std::vector<std::wstring> List(const std::wstring& dir, const wchar_t* pattern, const FILETIME* since = nullptr,
                               std::vector<FILETIME>* times = nullptr) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (since && CompareFileTime(&fd.ftLastWriteTime, since) < 0) continue;
        out.push_back(fd.cFileName);
        if (times) times->push_back(fd.ftLastWriteTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

FILETIME HoursAgo(int hours) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= static_cast<ULONGLONG>(hours) * 3600ull * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

// The files every bundle has besides the trace and the log: settings, launcher logs (24 h), dumps since `since`.
void CommonFiles(core::bundle::Job& job, const FILETIME& since) {
    const GameInfo& gi = GetGameInfo();
    const std::wstring ini = core::Config::Get().Path();
    if (!ini.empty()) job.files.push_back({ini, Narrow(FileName(ini)), true});  // passwords redacted (bundle::RedactIni)
    const FILETIME day = HoursAgo(24);
    auto launchers = List(LogDir(), L"launcher*.log", &day);
    if (launchers.size() > 6) launchers.resize(6);
    for (const auto& n : launchers) job.files.push_back({LogDir() + L"\\" + n, "logs/" + Narrow(n), false});
    for (const auto& n : List(LogDir(), L"crash-*.dmp", &since))
        job.files.push_back({LogDir() + L"\\" + n, "dumps/" + Narrow(n), false});
    (void)gi;
}

std::string ManifestHead(const char* what, const std::string& role, const std::string& stamp,
                         const std::string& player) {
    const GameInfo& gi = GetGameInfo();
    std::string label;
    const std::string crc = ExeCrcText(&label);
    char b[1024];
    std::snprintf(b, sizeof b,
                  "%s session bundle (%s)\r\n"
                  "mod: %s %s, core %s, protocol %u\r\n"
                  "exe: %s, crc %s (%s)\r\n"
                  "role: %s, player %s\r\n"
                  "session start: %s (the trace's name)\r\n"
                  "Windows %s\r\n"
                  "The settings file is in it with password-like values replaced by <redacted>.\r\n"
                  "Send this file to the developers together with the other player's newest bundle.\r\n",
                  gi.modName, what, gi.modName, gi.modVersion, kCoreVersion, net::kProtocolVersion,
                  Narrow(ExePath()).c_str(), crc.c_str(), label.c_str(), role.c_str(), player.c_str(), stamp.c_str(),
                  OsVersion().c_str());
    return b;
}

void QueueBundle(State& s, const std::wstring& tracePath) {
    if (s.bundlePath.empty()) return;
    CreateDirectoryW(BundleDir().c_str(), nullptr);
    core::bundle::Job job;
    job.zip = s.bundlePath;
    job.keepDir = BundleDir();
    job.files.push_back({tracePath, "trace/" + Narrow(FileName(tracePath)), false});
    const std::wstring logPath = core::log::Path();
    job.files.push_back({logPath, "logs/" + Narrow(FileName(logPath)), false});
    CommonFiles(job, s.startFt);
    job.manifest = ManifestHead("session end", s.role, s.stamp, std::to_string(s.local));
    core::bundle::Submit(std::move(job));
}

void Recover() {
    const std::wstring traceDir = LogDir() + L"\\trace";
    const std::wstring base = core::log::BaseName();
    if (LogDir().empty() || base.empty()) return;
    for (const auto& t : List(BundleDir(), L"*.tmp")) DeleteFileW((BundleDir() + L"\\" + t).c_str());  // cut off
    const std::wstring newest = core::bundle::NewestTrace(List(traceDir, L"*.trace"), base);
    if (newest.empty()) return;
    const std::wstring path = traceDir + L"\\" + newest;
    // still being written by another game (two on one PC with the same log name): leave it
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME created{};
    GetFileTime(f, &created, nullptr, nullptr);
    char head[1024] = {};
    DWORD got = 0;
    ReadFile(f, head, sizeof head - 1, &got, nullptr);
    CloseHandle(f);
    std::string first(head, got);
    if (const size_t nl = first.find('\n'); nl != std::string::npos) first.resize(nl);
    const std::string role = core::bundle::RoleOfHeader(first);
    const std::string stamp = core::bundle::TraceStamp(newest, base);
    const GameInfo& gi = GetGameInfo();
    if (core::bundle::HasBundle(List(BundleDir(), L"*.zip"), gi.modName, stamp, role)) return;
    CreateDirectoryW(BundleDir().c_str(), nullptr);
    core::bundle::Job job;
    job.zip = BundleDir() + L"\\" + core::bundle::BundleName(gi.modName, stamp, role, true);
    job.keepDir = BundleDir();
    job.files.push_back({path, "trace/" + Narrow(newest), false});
    const std::wstring prevLog = core::log::Path() + L".1";
    job.files.push_back({prevLog, "logs/" + Narrow(FileName(prevLog)), false});  // the run of that session
    CommonFiles(job, created);
    job.manifest = ManifestHead("recovered at the next start: the game had crashed or was closed without ending the "
                                "session",
                                role, stamp, "(see the trace header)");
    MC_LOG_INFO("bundle: the last session's trace %s has no bundle (the game crashed or was closed hard): writing %s",
                Narrow(newest).c_str(), Narrow(job.zip).c_str());
    core::bundle::Submit(std::move(job));
}

void BeforeClose(const std::wstring&) {
    State& s = S();
    std::lock_guard lock(s.mu);
    Finish(s);
}

void AfterClose(const std::wstring& path) {
    State& s = S();
    std::lock_guard lock(s.mu);
    if (!s.inSession) return;
    s.inSession = false;
    QueueBundle(s, path);
    s.tracePath.clear();
    core::diag::Reset();  // the next session counts from 0
}

}  // namespace

void SessionLog::Init() {
    core::trace::SetCloseHooks(&BeforeClose, &AfterClose);
    StartExeCrc();
    Recover();
}

void SessionLog::SetContextSource(SessionContextFn fn) {
    State& s = S();
    std::lock_guard lock(s.mu);
    s.ctxFn = fn;
}

void SessionLog::SetSession(net::Session* session) {
    State& s = S();
    std::lock_guard lock(s.mu);
    s.session = session;
}

void SessionLog::Tick(float dt) {
    State& s = S();
    std::lock_guard lock(s.mu);
    s.now += dt;
    // fps over 10 s windows
    if (dt > 0) {
        s.fpsT += dt;
        ++s.fpsN;
        s.fpsMaxDt = std::max(s.fpsMaxDt, dt);
        if (s.fpsT >= 10.0) {
            s.fps = s.fpsN / s.fpsT;
            s.fpsMin = s.fpsMaxDt > 0 ? 1.0 / s.fpsMaxDt : 0;
            s.fpsT = 0;
            s.fpsN = 0;
            s.fpsMaxDt = 0;
        }
    }
    if (s.now - s.repeatsAt >= kRepeatsEvery) {
        s.repeatsAt = s.now;
        core::log::FlushRepeats();
    }
    if (s.now - s.checkAt < 0.25) return;  // the trace's state, four times a second
    s.checkAt = s.now;
    if (!core::trace::IsOpen()) return;
    if (!s.inSession) {
        if (s.session && s.session->IsActive()) StartSession(s, core::trace::Path());
        if (!s.inSession) return;
    }
    if (s.session && s.session->IsActive()) {
        for (const auto& p : s.session->Players()) {
            auto it = s.seen.find(p.id);
            if (it != s.seen.end() && it->second == p.joinSeq) continue;
            WritePeer(s, p, true);
            s.seen[p.id] = p.joinSeq;
        }
    }
    const double now = core::trace::Now();
    if (now - s.sckAt >= kSckEvery) {
        s.sckAt = now;
        Sck(s);
    }
    if (now - s.sumAt >= kSumEvery) Summary(s, now);
    if (now - s.logAt >= kLogEvery) LogSummary(s, now);
}

void SessionLog::CloseForExit(const char* via, bool sample) {
    State& s = S();
    {
        std::lock_guard lock(s.mu);
        s.sampleOk = sample;
    }
    if (core::trace::IsOpen()) {
        core::trace::Line("END", "why=process-exit via=%s", via);
        core::trace::Close();  // FIN (BeforeClose), then the bundle is queued (AfterClose)
    }
}

void SessionLog::WaitForBundles() {
    if (!core::bundle::Busy()) return;
    MC_LOG_INFO("bundle: waiting for the session bundle to be written (at most %u s)", core::bundle::kExitWaitMs / 1000);
    const bool done = core::bundle::WaitIdle(core::bundle::kExitWaitMs);
    if (!done) MC_LOG_WARN("bundle: not finished after %u s: the game exits without it (it is written again at the "
                           "next start)", core::bundle::kExitWaitMs / 1000);
}

}  // namespace mc
