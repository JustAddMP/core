// net: the pre-game lobby on top of a Session (docs/protocol.md "Lobby").
//
// Players gather before a game starts: each guest toggles "ready", the host picks a game mode (an index into the
// game's own list, e.g. 0 = Free Ride) and starts; every player then loads that mode at the same moment. The host
// also tells everyone when it is already playing (inGame + mode), so a player who joins later can follow it.
// Host-authoritative like the session: guests only send their ready flag; the lobby state and the start come from
// the host (messages that claim to be state/start from anyone else are ignored).
//
// Single-threaded like Session: call everything from the thread that polls the session.
#pragma once
#include <cstdint>
#include <functional>
#include <map>

#include "net/session.h"

namespace mc::net {

class Lobby {
public:
    // App ids (core's range 0x0000-0x00FF).
    static constexpr uint16_t kMsgState = 0x0020;  // host -> guests
    static constexpr uint16_t kMsgReady = 0x0021;  // guest -> host
    static constexpr uint16_t kMsgStart = 0x0022;  // host -> guests

    void Attach(Session& session);  // registers the message handlers; state starts empty
    void Detach();
    bool Attached() const { return session_ != nullptr; }

    // Every frame after Session::Poll. Host: tracks players coming and going (a new player is not ready) and sends
    // the state when it changed. Guest: nothing to do.
    void Tick();

    // ---- host ----
    void SetMode(int mode);                 // the mode the host has picked (0..254)
    void SetInGame(bool inGame, int mode);  // the host is playing (mode -1 = unknown)
    // Starts `mode` for everyone: Start to all guests, onStart on the host too. False (nothing sent) when not hosting
    // or, unless force, while a guest is not ready.
    bool Start(int mode, bool force = false);
    bool AllReady() const;  // every guest is ready (true with no guests)

    // ---- everyone ----
    void SetReady(bool ready);  // guest: sent to the host; host: ignored (the host starts instead)
    bool Ready(PlayerId id) const;
    int Mode() const { return mode_; }
    bool InGame() const { return inGame_; }
    int GameMode() const { return gameMode_; }
    uint32_t StartCount() const { return startSeq_; }

    // A start for `mode` (on the host and on every guest). Runs inside Session::Poll (guests) or Start (host).
    std::function<void(int mode)> onStart;
    // Something visible changed (ready flags, mode, in game).
    std::function<void()> onChanged;

private:
    void SendState();
    void OnState(PlayerId from, ByteReader& r);
    void OnReady(PlayerId from, ByteReader& r);
    void OnStart(PlayerId from, ByteReader& r);
    void Changed();

    Session* session_ = nullptr;
    std::map<PlayerId, bool> ready_;
    int mode_ = 0;
    bool inGame_ = false;
    int gameMode_ = -1;
    uint32_t startSeq_ = 0;
    bool dirty_ = false;
    bool localReady_ = false;   // guest: what we asked for (shown until the host's state confirms it)
    size_t lastPlayers_ = 0;    // host: player list signature for change detection
    uint32_t lastPlayerMask_ = 0;
};

}  // namespace mc::net
