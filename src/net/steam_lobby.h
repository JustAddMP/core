// net: Steam lobby (ISteamMatchmaking009) + invites (ISteamFriends017).
// Friends-only lobby, max 4. Lobby data: mc_version, mc_proto, mc_host. Rich presence "connect" =
// "+connect_lobby <id>" so "Join Game" works from the friends list. Single-threaded: call Poll() each frame.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc::net {

class SteamLobby {
public:
    enum class State { Idle, Creating, Joining, InLobby };

    SteamLobby();
    ~SteamLobby();
    SteamLobby(const SteamLobby&) = delete;
    SteamLobby& operator=(const SteamLobby&) = delete;

    // Resolves the flat API (steam_api64.dll must be loaded and Steam initialised) and registers callbacks.
    // status: Steam rich presence "status" shown to friends while in a lobby (GameInfo::steamStatus).
    bool Init(std::string modVersion, uint32_t protocolVersion, std::string status = "co-op");
    void Poll();

    bool Create(int maxMembers = 4);  // friends-only
    bool Join(uint64_t lobbyId);
    void Leave();
    bool OpenInviteDialog();  // Steam overlay invite dialog for the current lobby

    State GetState() const { return state_; }
    uint64_t LobbyId() const { return lobby_; }
    uint64_t HostSteamId() const { return host_; }
    uint64_t LocalSteamId() const;
    bool IsOwner() const;
    bool IsMember(uint64_t steamId) const;
    std::vector<uint64_t> Members() const;
    std::string GetData(const char* key) const;
    // Owner only, while in the lobby: lobby data friends see before joining (e.g. mc_state, mc_mode). False otherwise.
    bool SetData(const char* key, const char* value);
    std::string PersonaName(uint64_t steamId = 0) const;  // 0 = local user

    // Events (all on the Poll() thread).
    std::function<void(uint64_t lobbyId)> onCreated;
    std::function<void(uint64_t lobbyId, uint64_t hostSteamId)> onJoined;
    std::function<void(const std::string& reason)> onFailed;
    // Invite accepted in the overlay / "Join Game" in the friends list. Default (unset): Join(lobbyId).
    std::function<void(uint64_t lobbyId)> onJoinRequested;
    std::function<void(uint64_t steamId, bool joined)> onMemberChanged;

    // "+connect_lobby <id>" from a command line (Steam passes it when launching via Join Game). 0 if absent.
    static uint64_t ParseConnectLobby(const char* cmdline);
    static uint64_t CommandLineLobby();  // from GetCommandLineW()

private:
    void OnCreated(bool ok, uint64_t lobby, int result);
    void OnEntered(bool ok, uint64_t lobby, uint32_t response);
    void RequestJoin(uint64_t lobbyId);
    void SetPresence(bool on);

    State state_ = State::Idle;
    uint64_t lobby_ = 0;
    uint64_t host_ = 0;
    uint64_t call_ = 0;  // pending SteamAPICall_t
    int callKind_ = 0;   // 1 create, 2 join
    double callStart_ = 0;
    // LobbyEnter_t broadcast seen while a create/join call is pending (fallback if the call result is lost).
    uint64_t enteredLobby_ = 0;
    uint32_t enteredResponse_ = 0;
    double enteredAt_ = 0;
    std::string modVersion_;
    uint32_t protocol_ = 0;
    std::string status_;
    bool inited_ = false;
    std::vector<int> subs_;
};

}  // namespace mc::net
