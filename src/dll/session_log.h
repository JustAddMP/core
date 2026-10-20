#pragma once
// Session diagnostics: everything a two-PC test needs to be understood afterwards from the two players' files
// alone (the game layer adds its own records). Driven from Coop::Tick on the render thread. A game repo's docs list
// the records (Mafia DE: docs/sync-trace.md).
//
// While a co-op session's sync trace is open (core/trace.h; the game layer opens it):
//   - at its start a header block in the main log ("session: ...") and SES records in the trace: versions, role,
//     local player and Steam account, transport and addresses, every setting (password-like ones redacted), window /
//     backbuffer / monitor, fps, OS, the paths of log, trace and bundle; every player (also later joiners) gets an
//     "SES part=peer" record
//   - SCK every 5 s (this machine's trace time vs the session clock: lines up the two traces)
//   - SUM every 10 s (per peer: latency, loss, bandwidth; per lane; fps; warnings / errors; the game's numbers from
//     core/diag.h) and a compact "session: summary" log line every 60 s
//   - at its end FIN records and a "session: ---- session end" block (totals, worst latency, the game's counts,
//     warnings / errors with the most repeated ones, the bundle's path), then the session bundle (core/bundle.h)
// At game start, the newest trace of this log that has no bundle (the game crashed or was killed) gets a
// "-recovered" bundle with the previous log. At game exit with a session open the summary and bundle are written
// and the exit waits for the bundle (at most bundle::kExitWaitMs).
#include <cstdint>
#include <string>

namespace mc::net {
class Session;
}

namespace mc {

// What Coop knows about the session's connection (for the header).
struct SessionContext {
    uint32_t build = 0;       // the handshake build number
    std::string mode;         // "host", "join (udp)", "join (steam lobby)"
    std::string transport;    // the session transport's name ("udp+steam", "udp", "steam")
    std::string target;       // guest: what we connect to (address, lobby, the host's name)
    uint16_t udpPort = 0;     // host: UDP port (0 = none)
    std::string lanAddress;   // this PC's LAN IPv4
    bool steamReady = false;
    uint64_t steamId = 0, lobbyId = 0;
    std::string persona;
    double relayKBs = 0;      // per-peer budget
    bool password = false;    // a session password is set (never its value)
};
using SessionContextFn = SessionContext (*)();

class SessionLog {
public:
    // Mod::Init (after Coop::Init): the trace close hooks, the exe CRC (background), and the recovery bundle.
    static void Init();
    static void SetContextSource(SessionContextFn fn);
    // The current session (nullptr none). Coop sets it when it makes or drops one.
    static void SetSession(net::Session* session);
    // Every frame (render thread), after the session's poll and the game's tick.
    static void Tick(float dt);
    // Game exit: closes an open trace with its summary and queues its bundle (sample = the session may be read: no
    // frame runs at the same time). Then, after the session is left, WaitForBundles.
    static void CloseForExit(const char* via, bool sample);
    static void WaitForBundles();
};

}  // namespace mc
