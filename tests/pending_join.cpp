// pending_join: the Steam join-request hand-off (src/dll/pending_join.h) that Coop uses so a join request raised by
// a Steam callback inside Session::Poll never tears the session down while it is still running. Mirrors the
// Coop::Tick order (Steam poll -> handle join -> session poll) with a stand-in session whose Poll raises the
// request. Exits non-zero on any failure.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dll/pending_join.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

// Stand-in for Coop's state: a session whose Poll may raise a Steam join request mid-poll (as
// SteamTransport::Poll -> steam::Pump -> GameLobbyJoinRequested does).
struct FakeSession;
struct FakeCoop {
    mc::PendingJoin pending;
    std::unique_ptr<FakeSession> session;
    uint64_t joinedLobby = 0;
    int sessionsDestroyedWhilePolling = 0;
    bool polling = false;

    void OnJoinRequested(uint64_t id) { pending.Post(id); }  // the callback: record only
    void HandleJoinRequest() {
        const uint64_t id = pending.Take();
        if (!id) return;
        if (polling) ++sessionsDestroyedWhilePolling;  // would be the use-after-free
        session.reset();
        joinedLobby = id;
    }
    void Tick();
};

struct FakeSession {
    FakeCoop* coop;
    uint64_t raiseJoin = 0;
    int polls = 0;
    void Poll() {
        ++polls;
        if (raiseJoin) coop->OnJoinRequested(std::exchange(raiseJoin, 0));
        ++polls;  // still using *this after the callback: must still be alive
    }
};

void FakeCoop::Tick() {
    // PollSteam() would run here (it may also raise a request: handled right below, same tick)
    HandleJoinRequest();
    if (session) {
        polling = true;
        session->Poll();
        polling = false;
    }
}

}  // namespace

int main() {
    // ---- basic semantics ----
    {
        mc::PendingJoin p;
        Check(p.Take() == 0 && !p.Pending(), "empty: Take() = 0");
        p.Post(0);
        Check(!p.Pending(), "Post(0) is ignored");
        p.Post(111);
        p.Post(222);
        Check(p.Pending(), "Post: pending");
        const uint64_t got = p.Take();
        Check(got == 222, "latest request wins", std::to_string(got));
        Check(p.Take() == 0, "Take() clears it");
    }

    // ---- request raised inside Session::Poll is acted on at the next tick, not mid-poll ----
    {
        FakeCoop coop;
        coop.session = std::make_unique<FakeSession>();
        coop.session->coop = &coop;
        coop.session->raiseJoin = 0x0186A0000000ABCDull;
        coop.Tick();  // the callback fires inside Poll
        Check(coop.session && coop.joinedLobby == 0 && coop.pending.Pending(),
              "tick 1: session kept, join only recorded");
        Check(coop.session && coop.session->polls == 2, "tick 1: Poll finished on a live session");
        coop.Tick();
        Check(!coop.session && coop.joinedLobby == 0x0186A0000000ABCDull, "tick 2: session left, lobby joined");
        Check(coop.sessionsDestroyedWhilePolling == 0, "never destroyed while polling");
    }

    // ---- posts from other threads: no loss of the final value, no torn reads ----
    {
        mc::PendingJoin p;
        constexpr int kThreads = 4, kPosts = 20000;
        std::atomic<bool> go{false};
        std::atomic<int> bad{0}, done{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                while (!go.load()) {
                }
                for (int i = 1; i <= kPosts; ++i) p.Post((static_cast<uint64_t>(t + 1) << 32) | static_cast<uint32_t>(i));
                ++done;
            });
        }
        go = true;
        uint64_t taken = 0;
        int takes = 0;
        while (done.load() < kThreads) {
            const uint64_t v = p.Take();
            if (!v) continue;
            ++takes;
            const uint64_t hi = v >> 32, lo = v & 0xFFFFFFFFu;
            if (hi < 1 || hi > kThreads || lo < 1 || lo > kPosts) ++bad;
            taken = v;
        }
        for (auto& th : threads) th.join();
        const uint64_t last = p.Take();
        if (last) {
            ++takes;
            taken = last;
        }
        Check(bad == 0, "cross-thread: every value taken is one that was posted");
        Check(takes > 0 && taken != 0 && !p.Pending(), "cross-thread: requests arrive, then nothing pending",
              std::to_string(takes) + " takes");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
