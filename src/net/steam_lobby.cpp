#include "net/steam_lobby.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "net/net_internal.h"
#include "net/steam_flat.h"
#include "net/steam_friends.h"

namespace mc::net {
using namespace detail;

namespace {
constexpr double kCallTimeoutS = 20.0;
// Rich presence is set again this often while in a lobby: the game runs its own (older) steam_api instance for the
// same app and user, and anything it clears there must not leave friends without "Join Game" / JOIN FRIEND.
constexpr double kPresenceRefreshS = 30.0;
constexpr uint32_t kChatMemberEntered = 0x0001;  // EChatMemberStateChange
steam::Api& api() { return steam::Get(); }
}  // namespace

SteamLobby::SteamLobby() = default;

SteamLobby::~SteamLobby() {
    for (int h : subs_) steam::Unsubscribe(h);
}

bool SteamLobby::Init(std::string modVersion, uint32_t protocolVersion, std::string status) {
    if (inited_) return true;
    if (!steam::Load() || !steam::AcquireInterfaces()) return false;
    modVersion_ = std::move(modVersion);
    protocol_ = protocolVersion;
    status_ = std::move(status);
    subs_.push_back(steam::Subscribe(steam::kCb_GameLobbyJoinRequested, sizeof(steam::GameLobbyJoinRequested_t),
                                     [this](const void* d) {
                                         auto* e = static_cast<const steam::GameLobbyJoinRequested_t*>(d);
                                         Log(kInfo, "steam: lobby join requested (lobby %llu, via friend %llu)",
                                             static_cast<unsigned long long>(e->m_steamIDLobby.v),
                                             static_cast<unsigned long long>(e->m_steamIDFriend.v));
                                         RequestJoin(e->m_steamIDLobby.v);
                                     }));
    subs_.push_back(steam::Subscribe(
        steam::kCb_GameRichPresenceJoinRequested, sizeof(steam::GameRichPresenceJoinRequested_t),
        [this](const void* d) {
            auto* e = static_cast<const steam::GameRichPresenceJoinRequested_t*>(d);
            char connect[257];
            std::memcpy(connect, e->m_rgchConnect, 256);
            connect[256] = 0;
            Log(kInfo, "steam: rich presence join requested: '%s'", connect);
            uint64_t id = ParseConnectLobby(connect);
            if (id) RequestJoin(id);
        }));
    // LobbyEnter_t is also posted as a broadcast callback. Remember it as a fallback in case the game's
    // SteamAPI_RunCallbacks consumed our call result before we polled it (see docs/protocol.md).
    subs_.push_back(steam::Subscribe(steam::kCb_LobbyEnter, sizeof(steam::LobbyEnter_t), [this](const void* d) {
        auto* e = static_cast<const steam::LobbyEnter_t*>(d);
        if (!call_ || (state_ != State::Creating && state_ != State::Joining)) return;
        if (state_ == State::Joining && e->m_ulSteamIDLobby != lobby_) return;
        Log(kDebug, "steam: LobbyEnter_t broadcast for lobby %llu", static_cast<unsigned long long>(e->m_ulSteamIDLobby));
        enteredLobby_ = e->m_ulSteamIDLobby;
        enteredResponse_ = e->m_EChatRoomEnterResponse;
        enteredAt_ = Now();
    }));
    subs_.push_back(steam::Subscribe(steam::kCb_LobbyChatUpdate, sizeof(steam::LobbyChatUpdate_t), [this](const void* d) {
        auto* e = static_cast<const steam::LobbyChatUpdate_t*>(d);
        if (e->m_ulSteamIDLobby != lobby_) return;
        bool joined = (e->m_rgfChatMemberStateChange & kChatMemberEntered) != 0;
        Log(kInfo, "steam: lobby member %llu %s", static_cast<unsigned long long>(e->m_ulSteamIDUserChanged),
            joined ? "joined" : "left");
        if (onMemberChanged) onMemberChanged(e->m_ulSteamIDUserChanged, joined);
    }));
    inited_ = true;
    return true;
}

uint64_t SteamLobby::LocalSteamId() const {
    return steam::Ready() ? api().User_GetSteamID(api().user) : 0;
}

bool SteamLobby::Create(int maxMembers) {
    if (!inited_ || state_ != State::Idle) return false;
    call_ = api().MM_CreateLobby(api().matchmaking, steam::k_ELobbyTypeFriendsOnly, maxMembers);
    if (!call_) {
        if (onFailed) onFailed("CreateLobby failed");
        return false;
    }
    callKind_ = 1;
    callStart_ = Now();
    enteredLobby_ = 0;
    state_ = State::Creating;
    Log(kInfo, "steam: creating lobby (max %d)", maxMembers);
    return true;
}

bool SteamLobby::Join(uint64_t lobbyId) {
    if (!inited_ || !lobbyId) return false;
    if (state_ == State::InLobby && lobby_ == lobbyId) return true;
    if (state_ != State::Idle) Leave();
    call_ = api().MM_JoinLobby(api().matchmaking, lobbyId);
    if (!call_) {
        if (onFailed) onFailed("JoinLobby failed");
        return false;
    }
    callKind_ = 2;
    callStart_ = Now();
    enteredLobby_ = 0;
    lobby_ = lobbyId;
    state_ = State::Joining;
    Log(kInfo, "steam: joining lobby %llu", static_cast<unsigned long long>(lobbyId));
    return true;
}

void SteamLobby::RequestJoin(uint64_t lobbyId) {
    if (!lobbyId) return;
    if (onJoinRequested)
        onJoinRequested(lobbyId);
    else
        Join(lobbyId);
}

void SteamLobby::Leave() {
    if (!inited_) return;
    if (lobby_ && state_ != State::Idle) {
        api().MM_LeaveLobby(api().matchmaking, lobby_);
        Log(kInfo, "steam: left lobby %llu", static_cast<unsigned long long>(lobby_));
    }
    SetPresence(false);
    state_ = State::Idle;
    lobby_ = host_ = 0;
    call_ = 0;
}

bool SteamLobby::OpenInviteDialog() {
    if (state_ != State::InLobby) return false;
    api().Friends_ActivateGameOverlayInviteDialog(api().friends, lobby_);
    return true;
}

bool SteamLobby::IsOwner() const {
    return state_ == State::InLobby && api().MM_GetLobbyOwner(api().matchmaking, lobby_) == LocalSteamId();
}

std::vector<uint64_t> SteamLobby::Members() const {
    std::vector<uint64_t> out;
    if (state_ != State::InLobby) return out;
    int n = api().MM_GetNumLobbyMembers(api().matchmaking, lobby_);
    for (int i = 0; i < n; ++i) out.push_back(api().MM_GetLobbyMemberByIndex(api().matchmaking, lobby_, i));
    return out;
}

bool SteamLobby::IsMember(uint64_t steamId) const {
    for (uint64_t m : Members())
        if (m == steamId) return true;
    return false;
}

bool SteamLobby::SetData(const char* key, const char* value) {
    if (!inited_ || state_ != State::InLobby || !IsOwner() || !api().MM_SetLobbyData) return false;
    return api().MM_SetLobbyData(api().matchmaking, lobby_, key, value);
}

std::string SteamLobby::GetData(const char* key) const {
    if (!lobby_ || !inited_) return {};
    const char* v = api().MM_GetLobbyData(api().matchmaking, lobby_, key);
    return v ? v : "";
}

std::string SteamLobby::PersonaName(uint64_t steamId) const {
    if (!steam::Ready()) return {};
    const char* n = steamId ? api().Friends_GetFriendPersonaName(api().friends, steamId)
                            : api().Friends_GetPersonaName(api().friends);
    return n ? n : "";
}

void SteamLobby::SetPresence(bool on) {
    if (!steam::Ready()) return;
    presenceAt_ = Now();
    if (on) {
        std::string connect = "+connect_lobby " + std::to_string(lobby_);
        api().Friends_SetRichPresence(api().friends, "connect", connect.c_str());
        api().Friends_SetRichPresence(api().friends, "status", status_.c_str());
    } else {
        api().Friends_SetRichPresence(api().friends, "connect", "");
        api().Friends_SetRichPresence(api().friends, "status", "");
    }
}

void SteamLobby::OnCreated(bool ok, uint64_t lobby, int result) {
    if (!ok || result != steam::k_EResultOK || !lobby) {
        state_ = State::Idle;
        lobby_ = 0;
        std::string why = "lobby creation failed (EResult " + std::to_string(result) + ")";
        Log(kWarn, "steam: %s", why.c_str());
        if (onFailed) onFailed(why);
        return;
    }
    lobby_ = lobby;
    host_ = LocalSteamId();
    state_ = State::InLobby;
    api().MM_SetLobbyData(api().matchmaking, lobby_, "mc_version", modVersion_.c_str());
    api().MM_SetLobbyData(api().matchmaking, lobby_, "mc_proto", std::to_string(protocol_).c_str());
    api().MM_SetLobbyData(api().matchmaking, lobby_, "mc_host", std::to_string(host_).c_str());
    api().MM_SetLobbyJoinable(api().matchmaking, lobby_, true);
    SetPresence(true);
    Log(kInfo, "steam: lobby %llu created", static_cast<unsigned long long>(lobby_));
    if (onCreated) onCreated(lobby_);
}

void SteamLobby::OnEntered(bool ok, uint64_t lobby, uint32_t response) {
    if (!ok || response != steam::k_EChatRoomEnterResponseSuccess) {
        state_ = State::Idle;
        lobby_ = 0;
        lastEnterResponse_ = ok ? response : steam::k_EChatRoomEnterResponseError;
        Log(kWarn, "steam: could not join lobby %llu (ok=%d, EChatRoomEnterResponse %u)",
            static_cast<unsigned long long>(lobby), ok, response);
        if (onFailed) onFailed(LobbyEnterResponseText(lastEnterResponse_));
        return;
    }
    lobby_ = lobby;
    state_ = State::InLobby;
    std::string h = GetData("mc_host");
    host_ = h.empty() ? 0 : strtoull(h.c_str(), nullptr, 10);
    if (!host_) host_ = api().MM_GetLobbyOwner(api().matchmaking, lobby_);
    std::string ver = GetData("mc_version");
    if (!ver.empty() && ver != modVersion_)
        Log(kWarn, "steam: lobby mod version '%s' differs from ours '%s'", ver.c_str(), modVersion_.c_str());
    lastEnterResponse_ = response;
    SetPresence(true);
    Log(kInfo, "steam: joined lobby %llu, host %llu", static_cast<unsigned long long>(lobby_),
        static_cast<unsigned long long>(host_));
    if (onJoined) onJoined(lobby_, host_);
}

void SteamLobby::Poll() {
    if (!inited_) return;
    steam::Pump();
    if (state_ == State::InLobby && Now() - presenceAt_ > kPresenceRefreshS) SetPresence(true);
    if (!call_) return;
    auto useFallback = [&](const char* why) {
        Log(kWarn, "steam: lobby call result %s, using the LobbyEnter_t broadcast", why);
        bool creating = callKind_ == 1;
        call_ = 0;
        if (creating)
            OnCreated(enteredResponse_ == steam::k_EChatRoomEnterResponseSuccess, enteredLobby_, steam::k_EResultOK);
        else
            OnEntered(true, enteredLobby_, enteredResponse_);
    };
    bool failed = false;
    if (api().Utils_IsAPICallCompleted(api().utils, call_, &failed)) {
        uint64_t call = call_;
        bool ok;
        if (callKind_ == 1) {
            steam::LobbyCreated_t r{};
            ok = api().Utils_GetAPICallResult(api().utils, call, &r, sizeof(r), steam::kCb_LobbyCreated, &failed) &&
                 !failed;
            if (ok || !enteredLobby_) {
                Log(kDebug, "steam: CreateLobby call result: ok=%d EResult=%d", ok, r.m_eResult);
                call_ = 0;
                OnCreated(ok, r.m_ulSteamIDLobby, ok ? r.m_eResult : 0);
                return;
            }
        } else {
            steam::LobbyEnter_t r{};
            ok = api().Utils_GetAPICallResult(api().utils, call, &r, sizeof(r), steam::kCb_LobbyEnter, &failed) &&
                 !failed;
            if (ok || !enteredLobby_) {
                Log(kDebug, "steam: JoinLobby call result: ok=%d response=%u", ok, r.m_EChatRoomEnterResponse);
                call_ = 0;
                OnEntered(ok, r.m_ulSteamIDLobby, r.m_EChatRoomEnterResponse);
                return;
            }
        }
        useFallback("could not be read");
        return;
    }
    if (enteredLobby_ && Now() - enteredAt_ > 2.0) {
        useFallback("did not arrive");
        return;
    }
    if (Now() - callStart_ > kCallTimeoutS) {
        call_ = 0;
        state_ = State::Idle;
        lobby_ = 0;
        if (onFailed) onFailed("Steam did not answer the lobby request");
    }
}

uint64_t SteamLobby::ParseConnectLobby(const char* cmdline) {
    if (!cmdline) return 0;
    const char* p = std::strstr(cmdline, "+connect_lobby");
    if (!p) return 0;
    p += std::strlen("+connect_lobby");
    while (*p == ' ' || *p == '\t' || *p == '"') ++p;
    return strtoull(p, nullptr, 10);
}

uint64_t SteamLobby::CommandLineLobby() {
    const wchar_t* w = GetCommandLineW();
    if (!w) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return 0;
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return ParseConnectLobby(s.c_str());
}

}  // namespace mc::net
