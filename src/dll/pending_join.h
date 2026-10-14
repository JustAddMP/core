#pragma once
// Hand-off for Steam join requests (invite accepted in the overlay, "Join Game" in the friends list).
//
// The request arrives from a Steam callback, which runs inside steam::Pump(). Pump is called from
// SteamLobby::Poll AND from SteamTransport::Poll, i.e. possibly from inside Session::Poll while the session and its
// transport are on the stack. Acting there (leaving the session, resetting the transport) would destroy objects
// that are still in use. So the callback only Post()s the lobby id; Coop::Tick Take()s it at the start of the next
// tick, after Steam polling and before the session is polled, when nothing network-related is running.
// Lock-free and safe to Post from any thread; the latest request wins (two invites in one frame: follow the last).
#include <atomic>
#include <cstdint>

namespace mc {

class PendingJoin {
public:
    void Post(uint64_t lobbyId) {
        if (lobbyId) lobby_.store(lobbyId, std::memory_order_release);
    }
    // The pending lobby id (0 = none); clears it.
    uint64_t Take() { return lobby_.exchange(0, std::memory_order_acq_rel); }
    bool Pending() const { return lobby_.load(std::memory_order_acquire) != 0; }

private:
    std::atomic<uint64_t> lobby_{0};
};

} // namespace mc
