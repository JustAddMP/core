// net: the JOIN FRIEND list: Steam friends who are playing this game, and whether (and how) they can be joined.
//
// Where a friend's co-op game is found (each source alone is enough; docs/protocol.md "JOIN FRIEND"):
//   1. ISteamFriends::GetFriendGamePlayed: the app the friend runs and, when it is visible to us, the Steam lobby the
//      friend is in (FriendGameInfo_t::m_steamIDLobby).
//   2. the friend's rich presence "connect" = "+connect_lobby <id>" (SteamLobby sets it on hosts AND guests), asked for
//      with RequestFriendRichPresence.
// The lobby's data (mc_version, mc_proto, mc_host, mc_state, mc_mode, mc_players, mc_max) comes from RequestLobbyData,
// so the list can say "version 0.13.0, you have 0.14.0" or "full" before anyone tries to join.
// A friend in our game but in no lobby is listed as playing, not hosting. Friends who are offline or run another
// game are left out.
//
// Everything is polled (no callbacks: the Steam callbacks may be eaten by whoever pumps them first, see
// docs/protocol.md), so Update() is cheap to call every frame: it scans at most once per second.
// Single-threaded: call from the thread that polls SteamLobby.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mc::net {

struct FriendGame {
    enum class Status {
        Hosting,          // hosting a co-op game: joinable
        InGame,           // a guest in someone's co-op game (hostName): joinable, we join that host
        Checking,         // in a lobby, its data has not arrived yet: joinable (the join checks the version)
        NotHosting,       // playing the game, but in no co-op lobby
        VersionMismatch,  // a co-op lobby of another mod version / protocol
        Full,             // the lobby has no free slot
    };
    uint64_t steamId = 0;
    std::string name;      // persona name
    Status status = Status::NotHosting;
    uint64_t lobby = 0;    // 0 when not in a lobby
    uint64_t host = 0;     // the lobby's host (mc_host), 0 if unknown
    std::string hostName;  // InGame: the host's persona name
    std::string version;   // the lobby's mc_version ("" = unknown)
    uint32_t protocol = 0; // the lobby's mc_proto (0 = unknown)
    bool playing = false;  // the host is in the world (mc_state "playing"), not in the pre-game lobby
    int mode = -1;         // mc_mode (-1 = unknown)
    int players = -1, maxPlayers = -1;  // mc_players / mc_max (-1 = unknown)
    bool viaPresence = false;  // the lobby id came from rich presence, not GetFriendGamePlayed

    bool Joinable() const {
        return status == Status::Hosting || status == Status::InGame || status == Status::Checking;
    }
};

// What the list says about one friend (pure; tests call it directly). `data` = the lobby data seen so far (empty
// when none arrived). lobby 0 = not in a lobby.
FriendGame::Status ClassifyFriendGame(uint64_t friendId, uint64_t lobby, const std::map<std::string, std::string>& data,
                                      const std::string& ourVersion, uint32_t ourProtocol);

// The text for a failed JoinLobby (EChatRoomEnterResponse), for the player.
std::string LobbyEnterResponseText(uint32_t response);

// One line for the list ("HOSTING - IN THE LOBBY", "VERSION 0.13.0 (YOU HAVE 0.14.0)", ...), upper case.
std::string FriendGameDetail(const FriendGame& f, const std::string& ourVersion);

class FriendGames {
public:
    // appId: the app friends must be running (ISteamUtils::GetAppID of our instance).
    void Configure(uint32_t appId, std::string modVersion, uint32_t protocolVersion, uint64_t localSteamId);
    // Rescans (at most once per scanIntervalS) and asks Steam for missing rich presence / lobby data. now in seconds.
    void Update(double now);
    // Forget everything and scan on the next Update (the panel was opened / REFRESH).
    void Reset();

    // Sorted: joinable first, then by name.
    const std::vector<FriendGame>& List() const { return list_; }
    const FriendGame* Find(uint64_t steamId) const;
    bool Available() const;      // the friends-list exports exist (else the UI only offers the Steam overlay)
    bool Scanned() const { return scans_ > 0; }
    int FriendCount() const { return friendCount_; }  // all regular friends
    int OnlineCount() const { return onlineCount_; }  // ... of them online

    double scanIntervalS = 1.0;
    double presenceRetryS = 10.0;  // RequestFriendRichPresence again after this
    double lobbyRetryS = 5.0;      // RequestLobbyData again after this (player counts change)

private:
    void Scan(double now);

    uint32_t appId_ = 0;
    std::string version_;
    uint32_t protocol_ = 0;
    uint64_t self_ = 0;
    double lastScan_ = -1e9;
    int scans_ = 0;
    int friendCount_ = 0, onlineCount_ = 0;
    std::vector<FriendGame> list_;
    std::map<uint64_t, double> presenceAsked_;  // friend -> when
    std::map<uint64_t, double> lobbyAsked_;     // lobby -> when
    std::string lastLog_;                       // log the list only when it changes
};

}  // namespace mc::net
