#include "net/steam_friends.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "net/net_internal.h"
#include "net/steam_flat.h"
#include "net/steam_lobby.h"

namespace mc::net {
using namespace detail;

namespace {

steam::Api& api() { return steam::Get(); }

// CSteamID: account id (32 bits), instance (20), account type (4), universe (8). Lobbies are chat accounts (type 8,
// k_EAccountTypeChat). Anything else in a lobby field is ignored (a stale or garbage value).
constexpr unsigned kAccountTypeChat = 8;
bool IsLobbyId(uint64_t id) { return id != 0 && ((id >> 52) & 0xF) == kAccountTypeChat; }

int ToInt(const std::string& s, int def) {
    if (s.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end && *end == 0 ? static_cast<int>(v) : def;
}

std::string Get(const std::map<std::string, std::string>& m, const char* k) {
    auto it = m.find(k);
    return it == m.end() ? std::string() : it->second;
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const char* StatusName(FriendGame::Status s) {
    switch (s) {
    case FriendGame::Status::Hosting: return "hosting";
    case FriendGame::Status::InGame: return "in a co-op game";
    case FriendGame::Status::Checking: return "in a lobby (checking)";
    case FriendGame::Status::NotHosting: return "playing, not hosting";
    case FriendGame::Status::VersionMismatch: return "other version";
    case FriendGame::Status::Full: return "full";
    }
    return "?";
}

}  // namespace

FriendGame::Status ClassifyFriendGame(uint64_t friendId, uint64_t lobby, const std::map<std::string, std::string>& data,
                                      const std::string& ourVersion, uint32_t ourProtocol) {
    using S = FriendGame::Status;
    if (!lobby) return S::NotHosting;
    if (data.empty()) return S::Checking;
    const std::string ver = Get(data, "mc_version");
    if (ver.empty()) return S::NotHosting;  // a lobby, but not one of ours
    const int proto = ToInt(Get(data, "mc_proto"), 0);
    if (ver != ourVersion || (proto > 0 && static_cast<uint32_t>(proto) != ourProtocol)) return S::VersionMismatch;
    const int players = ToInt(Get(data, "mc_players"), -1), maxPlayers = ToInt(Get(data, "mc_max"), -1);
    if (players > 0 && maxPlayers > 0 && players >= maxPlayers) return S::Full;
    const uint64_t host = std::strtoull(Get(data, "mc_host").c_str(), nullptr, 10);
    if (host && host != friendId) return S::InGame;
    return S::Hosting;
}

std::string LobbyEnterResponseText(uint32_t r) {
    switch (r) {
    case steam::k_EChatRoomEnterResponseSuccess: return "joined";
    case steam::k_EChatRoomEnterResponseDoesntExist: return "That game is gone: the host stopped hosting.";
    case steam::k_EChatRoomEnterResponseNotAllowed:
        return "That game is private: ask the host to invite you (CO-OP > INVITE FRIENDS).";
    case steam::k_EChatRoomEnterResponseFull: return "That game is full.";
    case steam::k_EChatRoomEnterResponseBanned: return "You are banned from that game.";
    case steam::k_EChatRoomEnterResponseLimited:
        return "Steam does not let limited accounts join lobbies (spend $5 on Steam to lift the limit).";
    case steam::k_EChatRoomEnterResponseMemberBlockedYou: return "A player in that game has blocked you on Steam.";
    case steam::k_EChatRoomEnterResponseYouBlockedMember: return "You have blocked a player in that game on Steam.";
    case steam::k_EChatRoomEnterResponseRatelimitExceeded: return "Too many join attempts: wait a minute and try again.";
    case steam::k_EChatRoomEnterResponseError:
    default: return "Steam could not join that game (error " + std::to_string(r) + "). Try again.";
    }
}

std::string FriendGameDetail(const FriendGame& f, const std::string& ourVersion) {
    using S = FriendGame::Status;
    const std::string count = f.players > 0 && f.maxPlayers > 0
                                  ? " (" + std::to_string(f.players) + "/" + std::to_string(f.maxPlayers) + ")"
                                  : std::string();
    switch (f.status) {
    case S::Hosting: return "HOSTING" + count + (f.playing ? " - PLAYING" : " - IN THE LOBBY");
    case S::InGame:
        return "IN " + (f.hostName.empty() ? std::string("A") : Upper(f.hostName) + "'S") + " CO-OP GAME" + count;
    case S::Checking: return "CHECKING THEIR GAME...";
    case S::NotHosting: return "PLAYING, NOT HOSTING";
    case S::VersionMismatch:
        return f.version == ourVersion ? "OTHER CO-OP BUILD (UPDATE BOTH)"
                                       : "VERSION " + Upper(f.version) + " - YOU HAVE " + Upper(ourVersion);
    case S::Full: return "FULL" + count;
    }
    return {};
}

void FriendGames::Configure(uint32_t appId, std::string modVersion, uint32_t protocolVersion, uint64_t localSteamId) {
    appId_ = appId;
    version_ = std::move(modVersion);
    protocol_ = protocolVersion;
    self_ = localSteamId;
}

bool FriendGames::Available() const {
    const steam::Api& a = steam::Get();
    return steam::Ready() && a.Friends_GetFriendCount && a.Friends_GetFriendByIndex && a.Friends_GetFriendGamePlayed;
}

void FriendGames::Reset() {
    lastScan_ = -1e9;
    presenceAsked_.clear();
    lobbyAsked_.clear();
}

const FriendGame* FriendGames::Find(uint64_t steamId) const {
    for (const auto& f : list_)
        if (f.steamId == steamId) return &f;
    return nullptr;
}

void FriendGames::Update(double now) {
    if (now - lastScan_ < scanIntervalS) return;
    lastScan_ = now;
    Scan(now);
}

void FriendGames::Scan(double now) {
    list_.clear();
    friendCount_ = onlineCount_ = 0;
    if (!Available()) return;
    steam::Api& a = api();
    ++scans_;
    const int n = a.Friends_GetFriendCount(a.friends, steam::k_EFriendFlagImmediate);
    friendCount_ = n > 0 ? n : 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t id = a.Friends_GetFriendByIndex(a.friends, i, steam::k_EFriendFlagImmediate);
        if (!id || id == self_) continue;
        if (a.Friends_GetFriendPersonaState &&
            a.Friends_GetFriendPersonaState(a.friends, id) != steam::k_EPersonaStateOffline)
            ++onlineCount_;
        steam::FriendGameInfo_t gi{};
        if (!a.Friends_GetFriendGamePlayed(a.friends, id, &gi)) continue;  // offline or not in a game
        const uint32_t app = static_cast<uint32_t>(gi.m_gameID & 0xFFFFFF);
        const uint32_t type = static_cast<uint32_t>((gi.m_gameID >> 24) & 0xFF);
        if (app != appId_ || type != 0) continue;  // another game (or a mod / shortcut of one)

        FriendGame f;
        f.steamId = id;
        const char* name = a.Friends_GetFriendPersonaName ? a.Friends_GetFriendPersonaName(a.friends, id) : nullptr;
        f.name = name && *name ? name : std::to_string(id);
        if (IsLobbyId(gi.m_steamIDLobby)) f.lobby = gi.m_steamIDLobby;
        if (!f.lobby && a.Friends_GetFriendRichPresence) {
            const char* rp = a.Friends_GetFriendRichPresence(a.friends, id, "connect");
            const uint64_t l = SteamLobby::ParseConnectLobby(rp);
            if (IsLobbyId(l)) {
                f.lobby = l;
                f.viaPresence = true;
            } else if (a.Friends_RequestFriendRichPresence) {
                double& asked = presenceAsked_.try_emplace(id, -1e9).first->second;
                if (now - asked >= presenceRetryS) {
                    asked = now;
                    a.Friends_RequestFriendRichPresence(a.friends, id);
                }
            }
        }
        std::map<std::string, std::string> data;
        if (f.lobby) {
            if (a.MM_RequestLobbyData) {
                double& asked = lobbyAsked_.try_emplace(f.lobby, -1e9).first->second;
                if (now - asked >= lobbyRetryS) {
                    asked = now;
                    a.MM_RequestLobbyData(a.matchmaking, f.lobby);
                }
            }
            for (const char* key : {"mc_version", "mc_proto", "mc_host", "mc_state", "mc_mode", "mc_players", "mc_max"}) {
                const char* v = a.MM_GetLobbyData ? a.MM_GetLobbyData(a.matchmaking, f.lobby, key) : nullptr;
                if (v && *v) data[key] = v;
            }
        }
        f.status = ClassifyFriendGame(id, f.lobby, data, version_, protocol_);
        f.version = Get(data, "mc_version");
        f.protocol = static_cast<uint32_t>(ToInt(Get(data, "mc_proto"), 0));
        f.host = std::strtoull(Get(data, "mc_host").c_str(), nullptr, 10);
        f.playing = Get(data, "mc_state") == "playing";
        f.mode = ToInt(Get(data, "mc_mode"), -1);
        f.players = ToInt(Get(data, "mc_players"), -1);
        f.maxPlayers = ToInt(Get(data, "mc_max"), -1);
        if (f.status == FriendGame::Status::InGame && a.Friends_GetFriendPersonaName) {
            const char* hn = a.Friends_GetFriendPersonaName(a.friends, f.host);
            f.hostName = hn && *hn && std::string(hn) != "[unknown]" ? hn : std::string();
        }
        list_.push_back(std::move(f));
    }
    std::stable_sort(list_.begin(), list_.end(), [](const FriendGame& x, const FriendGame& y) {
        if (x.Joinable() != y.Joinable()) return x.Joinable();
        return x.name < y.name;
    });

    std::string line = std::to_string(list_.size()) + " of " + std::to_string(friendCount_) + " friends (" +
                       std::to_string(onlineCount_) + " online) in this game";
    for (const auto& f : list_) {
        line += "; '" + f.name + "' " + StatusName(f.status);
        if (f.lobby) line += " lobby " + std::to_string(f.lobby) + (f.viaPresence ? " (rich presence)" : "");
        if (!f.version.empty()) line += " v" + f.version;
    }
    if (line != lastLog_) {
        lastLog_ = line;
        Log(kInfo, "steam: friends: %s", line.c_str());
    }
}

}  // namespace mc::net
