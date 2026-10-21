// net_sim: the one-PC test tools' settings. net/net_sim.h: --net-sim / net_sim parsing (keys, units, errors, "off"),
// what it puts into UdpOptions, the launcher passing --net-sim and --window-area on to the dll, and two UdpTransports
// on 127.0.0.1 with latency=12,jitter=3 on both: the measured round trip lands at 20-30 ms (without it ~0-1 ms).
// dll/window_place.h: window_area parsing and where a new window is made. Exits non-zero on any failure.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dll/window_place.h"
#include "launcher/args.h"
#include "net/net_sim.h"
#include "net/udp_transport.h"

using namespace mc;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
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

void TestParse() {
    net::NetSim s;
    std::string err;
    const bool p1 = net::ParseNetSim("latency=12,jitter=3,loss=0,dup=0", s, &err);
    Check(p1 && s.latencyMs == 12 && s.jitterMs == 3 &&
              s.lossPct == 0 && s.dupPct == 0 && s.On(),
          "parse: latency=12,jitter=3,loss=0,dup=0", net::NetSimText(s));
    const bool p2 = net::ParseNetSim(" jitter=2.5; latency=20 loss=1.5,dup=0.5 ", s, &err);
    Check(p2 && s.latencyMs == 20 &&
              s.jitterMs == 2.5f && s.lossPct == 1.5f && s.dupPct == 0.5f,
          "parse: any order, spaces, ';' and ' ' as separators, decimals", net::NetSimText(s));
    Check(net::ParseNetSim("latency=12", s, &err) && s.latencyMs == 12 && s.jitterMs == 0 && s.On(),
          "parse: missing keys are 0");
    for (const char* off : {"", "0", "off", "none", "latency=0,jitter=0"})
        Check(net::ParseNetSim(off, s, &err) && !s.On(), (std::string("parse: '") + off + "' is no simulation").c_str());
    for (const char* bad : {"latency", "latency=abc", "latency=-1", "latency=12x", "lag=12", "latency=2000",
                            "loss=101", "jitter=", "dup=200"}) {
        const bool ok = net::ParseNetSim(bad, s, &err);
        Check(!ok && !s.On() && !err.empty(), (std::string("parse: '") + bad + "' refused").c_str(), err);
    }
    Check(net::NetSimText(net::NetSim{}) == "off", "text: no simulation reads 'off'");
    net::NetSim r;
    const bool p3 = net::ParseNetSim("latency=12,jitter=3,loss=1,dup=2", s);
    Check(p3 && net::ParseNetSim(net::NetSimText(s), r) && r.latencyMs == 12 && r.jitterMs == 3 && r.lossPct == 1 &&
              r.dupPct == 2,
          "text: parses back to the same values", net::NetSimText(s));
}

void TestApply() {
    net::NetSim s;
    net::ParseNetSim("latency=12,jitter=3,loss=5,dup=1", s);
    net::UdpOptions o;
    net::ApplyNetSim(s, o);
    Check(o.simLatencyMs == 12 && o.simJitterMs == 3 && std::fabs(o.simLoss - 0.05f) < 1e-6f &&
              std::fabs(o.simDuplicate - 0.01f) < 1e-6f,
          "apply: ms as they are, loss / dup percent -> 0..1");
    net::UdpOptions none;
    net::ApplyNetSim(net::NetSim{}, none);
    Check(none.simLatencyMs == 0 && none.simJitterMs == 0 && none.simLoss == 0 && none.simDuplicate == 0,
          "apply: off leaves the transport unsimulated");
}

std::wstring Quote(const std::wstring& a) { return a.find(L' ') == std::wstring::npos ? a : L"\"" + a + L"\""; }

void TestLauncher() {
    std::vector<const wchar_t*> argv{L"launcher.exe", L"--host", L"--net-sim", L"latency=12,jitter=3",
                                     L"--window-area", L"2916,4233,633"};
    const auto a = launcher::ParseLaunchArgs(static_cast<int>(argv.size()), argv.data(), L"MCTEST_", L"launcher.exe",
                                             Quote);
    std::wstring sim = L"<unset>", area = L"<unset>";
    for (auto& [k, v] : a.passEnv) {
        if (k == L"MCTEST_NET_SIM") sim = v;
        if (k == L"MCTEST_WINDOW_AREA") area = v;
    }
    Check(a.error.empty() && sim == L"latency=12,jitter=3" && area == L"2916,4233,633",
          "launcher: --net-sim / --window-area reach the dll (MCTEST_NET_SIM / _WINDOW_AREA)");
    std::vector<const wchar_t*> bad{L"launcher.exe", L"--net-sim", L"--host"};
    const auto b = launcher::ParseLaunchArgs(static_cast<int>(bad.size()), bad.data(), L"MCTEST_", L"launcher.exe",
                                             Quote);
    Check(!b.error.empty(), "launcher: --net-sim needs a value");
}

void TestWindowArea() {
    window_place::Area a;
    Check(window_place::ParseArea("2916,4233", a) && a.On() && a.x0 == 2916 && a.x1 == 4233 && !a.haveY,
          "window_area: x0,x1");
    Check(window_place::ParseArea("2916,4233,633", a) && a.haveY && a.y == 633, "window_area: x0,x1,y");
    Check(window_place::ParseArea("", a) && !a.On(), "window_area: empty is off");
    for (const char* bad : {"2916", "4233,2916", "2916,3000", "x,y", "2916,4233,633,1"})
        Check(!window_place::ParseArea(bad, a) && !a.On(), (std::string("window_area: '") + bad + "' refused").c_str());
    window_place::ParseArea("2916,4233,633", a);
    int x = 2320, y = 479, w = 480;
    window_place::FitCreate(a, x, y, w);
    Check(x == 2916 && y == 633 && w == 480, "window_area: the 480x481 start window is made at 2916,633",
          std::to_string(x) + "," + std::to_string(y));
    x = 0, y = 0, w = 5120;
    window_place::FitCreate(a, x, y, w);
    Check(x == 2916 && x + w <= 4233, "window_area: a wider window is made as wide as the area", std::to_string(w));
    window_place::ParseArea("2916,4233", a);
    x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = CW_USEDEFAULT;
    window_place::FitCreate(a, x, y, w);
    Check(x == 2916 && y == 0 && w == CW_USEDEFAULT, "window_area: CW_USEDEFAULT: left edge and top set, size the game's");
}

// Two transports on loopback, a ping / pong of unreliable datagrams; returns the median round trip (ms).
double MeasureRtt(const net::NetSim& sim, int pings) {
    net::UdpOptions o;
    net::ApplyNetSim(sim, o);
    o.simSeed = 7;
    net::UdpTransport host(o);
    o.simSeed = 11;
    net::UdpTransport guest(o);
    if (!host.Host(0)) return -1;
    const std::string addr = "127.0.0.1:" + std::to_string(host.BoundPort());
    const net::PeerId toHost = guest.Connect(addr);
    net::PeerId toGuest = 0;
    bool up = false;
    const double t0 = Seconds();
    while (Seconds() - t0 < 3 && !(up && toGuest)) {
        host.Poll([&](const net::Event& e) {
            if (e.type == net::EventType::Connected) toGuest = e.peer;
        });
        guest.Poll([&](const net::Event& e) {
            if (e.type == net::EventType::Connected) up = true;
        });
        Sleep(1);
    }
    if (!up || !toGuest) return -1;
    std::vector<double> rtt;
    for (int i = 0; i < pings; ++i) {
        const uint8_t ping[4] = {1, static_cast<uint8_t>(i), 0, 0};
        const double sent = Seconds();
        guest.Send(toHost, ping, net::Reliability::Unreliable);
        guest.Flush();
        bool back = false;
        while (!back && Seconds() - sent < 0.5) {
            host.Poll([&](const net::Event& e) {
                if (e.type == net::EventType::Message && e.data.size() == 4 && e.data[0] == 1) {
                    const uint8_t pong[4] = {2, e.data[1], 0, 0};
                    host.Send(toGuest, pong, net::Reliability::Unreliable);
                    host.Flush();
                }
            });
            guest.Poll([&](const net::Event& e) {
                if (e.type == net::EventType::Message && e.data.size() == 4 && e.data[0] == 2 &&
                    e.data[1] == static_cast<uint8_t>(i))
                    back = true;
            });
            if (!back) Sleep(0);
        }
        if (back) rtt.push_back((Seconds() - sent) * 1000.0);
    }
    if (rtt.size() < static_cast<size_t>(pings) * 9 / 10) return -2;
    std::sort(rtt.begin(), rtt.end());
    return rtt[rtt.size() / 2];
}

void TestLoopbackLag() {

    net::NetSim off, on;
    net::ParseNetSim("latency=12,jitter=3", on);
    const double base = MeasureRtt(off, 60);
    const double lag = MeasureRtt(on, 60);

    char d[96];
    std::snprintf(d, sizeof d, "without %.1f ms, with %.1f ms", base, lag);
    Check(base >= 0 && base < 8, "loopback: without net_sim the round trip is short", d);
    Check(lag >= 20 && lag <= 30, "loopback: latency=12,jitter=3 on both sides: round trip 20-30 ms", d);
}

}  // namespace

int main() {
    TestParse();
    TestApply();
    TestLauncher();
    TestWindowArea();
    TestLoopbackLag();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
