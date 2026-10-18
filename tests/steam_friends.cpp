// steam_friends: the JOIN FRIEND list (src/net/steam_friends.h) against a fake Steam (steam::SetApiForTest: made-up
// friends, games, rich presence and lobby data; no steam_api64.dll). Covers: friends in another game / offline left
// out, a friend in our game but not hosting, a host found through GetFriendGamePlayed and one only through rich
// presence, a guest in someone else's game, another mod version, a full lobby, lobby data that arrives late, several
// friends sorted (joinable first), the Steam requests being throttled, the texts for failed joins, and the pure
// classification. Exits non-zero on any failure.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "net/steam_flat.h"
#include "net/steam_friends.h"

using namespace mc::net;
using St = FriendGame::Status;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

// ---- the fake Steam ----
constexpr uint32_t kApp = 1030840;
constexpr uint64_t kSelf = 76561198000000001ull;
uint64_t Lobby(uint32_t n) { return (1ull << 56) | (8ull << 52) | n; }  // universe 1, chat account

struct FakeFriend {
    uint64_t id;
    std::string name;
    int persona = 1;           // 0 offline
    uint64_t game = 0;         // 0 = not in a game
    uint64_t lobby = 0;        // FriendGameInfo_t::m_steamIDLobby
    std::string connect;       // rich presence "connect"
    bool presenceOnRequest = false;  // the rich presence only shows after RequestFriendRichPresence
};
struct Fake {
    std::vector<FakeFriend> friends;
    std::map<uint64_t, std::map<std::string, std::string>> lobbyData;  // what GetLobbyData returns
    std::map<uint64_t, std::map<std::string, std::string>> lobbyDataOnRequest;  // appears after RequestLobbyData
    std::map<uint64_t, std::map<std::string, std::string>> lobbyDataInFlight;   // ... at the next scan
    std::map<uint64_t, std::string> names;  // non-friend persona names (hosts)
    int presenceRequests = 0, lobbyRequests = 0;
    std::string tmp;
} g;

FakeFriend* FindFriend(uint64_t id) {
    for (auto& f : g.friends)
        if (f.id == id) return &f;
    return nullptr;
}

int FGetFriendCount(void*, int flags) {
    for (auto& [l, d] : g.lobbyDataInFlight) g.lobbyData[l] = d;  // the LobbyDataUpdate "arrived" since the last scan
    g.lobbyDataInFlight.clear();
    return flags == steam::k_EFriendFlagImmediate ? static_cast<int>(g.friends.size()) : 0;
}
uint64_t FGetFriendByIndex(void*, int i, int) {
    return i >= 0 && i < static_cast<int>(g.friends.size()) ? g.friends[static_cast<size_t>(i)].id : 0;
}
int FGetFriendPersonaState(void*, uint64_t id) {
    const FakeFriend* f = FindFriend(id);
    return f ? f->persona : 0;
}
bool FGetFriendGamePlayed(void*, uint64_t id, steam::FriendGameInfo_t* out) {
    const FakeFriend* f = FindFriend(id);
    if (!f || !f->persona || !f->game) return false;
    *out = {};
    out->m_gameID = f->game;
    out->m_steamIDLobby = f->lobby;
    return true;
}
const char* FGetFriendRichPresence(void*, uint64_t id, const char* key) {
    const FakeFriend* f = FindFriend(id);
    if (!f || std::string(key) != "connect" || f->presenceOnRequest) return "";
    return f->connect.c_str();
}
void FRequestFriendRichPresence(void*, uint64_t id) {
    ++g.presenceRequests;
    if (FakeFriend* f = FindFriend(id)) f->presenceOnRequest = false;
}
const char* FGetFriendPersonaName(void*, uint64_t id) {
    if (const FakeFriend* f = FindFriend(id)) return f->name.c_str();
    auto it = g.names.find(id);
    return it == g.names.end() ? "[unknown]" : it->second.c_str();
}
bool FRequestLobbyData(void*, uint64_t lobby) {
    ++g.lobbyRequests;
    auto it = g.lobbyDataOnRequest.find(lobby);
    if (it != g.lobbyDataOnRequest.end()) {
        g.lobbyDataInFlight[lobby] = it->second;
        g.lobbyDataOnRequest.erase(it);
    }
    return true;
}
const char* FGetLobbyData(void*, uint64_t lobby, const char* key) {
    auto it = g.lobbyData.find(lobby);
    if (it == g.lobbyData.end()) return "";
    auto k = it->second.find(key);
    if (k == it->second.end()) return "";
    g.tmp = k->second;
    return g.tmp.c_str();
}

void InstallFake() {
    steam::Api a;
    a.friends = reinterpret_cast<void*>(0x1);
    a.matchmaking = reinterpret_cast<void*>(0x2);
    a.Friends_GetFriendCount = &FGetFriendCount;
    a.Friends_GetFriendByIndex = &FGetFriendByIndex;
    a.Friends_GetFriendPersonaState = &FGetFriendPersonaState;
    a.Friends_GetFriendGamePlayed = &FGetFriendGamePlayed;
    a.Friends_GetFriendRichPresence = &FGetFriendRichPresence;
    a.Friends_RequestFriendRichPresence = &FRequestFriendRichPresence;
    a.Friends_GetFriendPersonaName = &FGetFriendPersonaName;
    a.MM_RequestLobbyData = &FRequestLobbyData;
    a.MM_GetLobbyData = &FGetLobbyData;
    steam::SetApiForTest(a);
}

std::map<std::string, std::string> Data(const std::string& ver, uint64_t host, int players = 1, int max = 4,
                                        const std::string& state = "lobby", const std::string& proto = "7") {
    return {{"mc_version", ver},
            {"mc_proto", proto},
            {"mc_host", std::to_string(host)},
            {"mc_state", state},
            {"mc_mode", "0"},
            {"mc_players", std::to_string(players)},
            {"mc_max", std::to_string(max)}};
}

const FriendGame* Get(const FriendGames& fg, uint64_t id) { return fg.Find(id); }

}  // namespace

int main() {
    // ---- pure classification ----
    {
        const std::map<std::string, std::string> none;
        Check(ClassifyFriendGame(5, 0, none, "0.14.0", 7) == St::NotHosting, "no lobby: not hosting");
        Check(ClassifyFriendGame(5, Lobby(1), none, "0.14.0", 7) == St::Checking, "lobby, no data yet: checking");
        Check(ClassifyFriendGame(5, Lobby(1), Data("0.14.0", 5), "0.14.0", 7) == St::Hosting, "own lobby: hosting");
        Check(ClassifyFriendGame(5, Lobby(1), Data("0.14.0", 9), "0.14.0", 7) == St::InGame, "someone else's: in game");
        Check(ClassifyFriendGame(5, Lobby(1), Data("0.13.0", 5), "0.14.0", 7) == St::VersionMismatch, "other version");
        Check(ClassifyFriendGame(5, Lobby(1), Data("0.14.0", 5, 1, 4, "lobby", "6"), "0.14.0", 7) == St::VersionMismatch,
              "same version, other protocol");
        Check(ClassifyFriendGame(5, Lobby(1), Data("0.14.0", 5, 4, 4), "0.14.0", 7) == St::Full, "4/4: full");
        Check(ClassifyFriendGame(5, Lobby(1), {{"other_key", "x"}}, "0.14.0", 7) == St::NotHosting,
              "a lobby that is not ours: not hosting");
        Check(ClassifyFriendGame(5, Lobby(1), {{"mc_version", "0.14.0"}}, "0.14.0", 7) == St::Hosting,
              "old host without mc_host / counts: hosting");
        FriendGame f;
        f.status = St::VersionMismatch;
        f.version = "0.13.0";
        Check(FriendGameDetail(f, "0.14.0") == "VERSION 0.13.0 - YOU HAVE 0.14.0", "mismatch text",
              FriendGameDetail(f, "0.14.0"));
        f.status = St::Hosting;
        f.players = 2;
        f.maxPlayers = 4;
        f.playing = true;
        Check(FriendGameDetail(f, "0.14.0") == "HOSTING (2/4) - PLAYING", "hosting text", FriendGameDetail(f, "0.14.0"));
        Check(LobbyEnterResponseText(steam::k_EChatRoomEnterResponseDoesntExist).find("stopped hosting") !=
                  std::string::npos,
              "join failure text: lobby gone");
        Check(LobbyEnterResponseText(steam::k_EChatRoomEnterResponseFull).find("full") != std::string::npos,
              "join failure text: full");
        Check(LobbyEnterResponseText(99).find("99") != std::string::npos, "join failure text: unknown code");
    }

    // ---- not available before Steam is ----
    {
        FriendGames fg;
        fg.Configure(kApp, "0.14.0", 7, kSelf);
        Check(!fg.Available(), "no Steam: not available");
        fg.Update(0);
        Check(fg.List().empty() && !fg.Scanned(), "no Steam: empty list, no scan");
    }

    InstallFake();
    // ---- no friends / nobody online ----
    {
        FriendGames fg;
        fg.Configure(kApp, "0.14.0", 7, kSelf);
        fg.Update(0);
        Check(fg.Available() && fg.Scanned() && fg.FriendCount() == 0 && fg.List().empty(), "no friends: empty");
        g.friends = {{1001, "Offline Olly", 0, kApp}};
        fg.Reset();
        fg.Update(1);
        Check(fg.FriendCount() == 1 && fg.OnlineCount() == 0 && fg.List().empty(), "offline friend left out");
    }

    // ---- the full scene: several friends ----
    const uint64_t miles = 2001, ann = 2002, bob = 2003, cat = 2004, dan = 2005, eve = 2006, fred = 2007, gus = 2008;
    const uint64_t host9 = 9009;  // not our friend
    g.friends = {
        {miles, "Miles", 1, kApp, Lobby(1)},                        // hosting, lobby via GetFriendGamePlayed
        {ann, "Ann", 1, 570},                                        // another game
        {bob, "Bob", 1, kApp},                                       // our game, not hosting
        {cat, "Cat", 1, kApp, 0, "+connect_lobby " + std::to_string(Lobby(2)), true},  // only rich presence (asked)
        {dan, "Dan", 1, kApp, Lobby(3)},                             // guest in host9's game
        {eve, "Eve", 1, kApp, Lobby(4)},                             // other version
        {fred, "Fred", 1, kApp, Lobby(5)},                           // full
        {gus, "Gus", 1, kApp | (1ull << 24), Lobby(6)},              // a mod/shortcut of the app id: not our game
    };
    g.lobbyData[Lobby(1)] = Data("0.14.0", miles, 1, 4, "playing");
    g.lobbyDataOnRequest[Lobby(2)] = Data("0.14.0", cat, 2, 4);  // arrives with RequestLobbyData
    g.lobbyData[Lobby(3)] = Data("0.14.0", host9, 2, 4);
    g.names[host9] = "Nine";
    g.lobbyData[Lobby(4)] = Data("0.13.0", eve);
    g.lobbyData[Lobby(5)] = Data("0.14.0", fred, 4, 4);
    g.lobbyData[Lobby(6)] = Data("0.14.0", gus);

    FriendGames fg;
    fg.Configure(kApp, "0.14.0", 7, kSelf);
    fg.Update(10);
    {
        const auto& l = fg.List();
        Check(l.size() == 6, "six friends in our game", std::to_string(l.size()));
        Check(!Get(fg, ann) && !Get(fg, gus), "other game / app-id mod left out");
        const FriendGame* m = Get(fg, miles);
        Check(m && m->status == St::Hosting && m->lobby == Lobby(1) && !m->viaPresence && m->playing,
              "Miles: hosting (GetFriendGamePlayed lobby), playing");
        const FriendGame* b = Get(fg, bob);
        Check(b && b->status == St::NotHosting && !b->Joinable(), "Bob: playing, not hosting");
        const FriendGame* c = Get(fg, cat);
        Check(c && c->status == St::NotHosting, "Cat: rich presence not there yet (asked for it)");
        const FriendGame* d = Get(fg, dan);
        Check(d && d->status == St::InGame && d->hostName == "Nine" && d->Joinable(), "Dan: in Nine's game, joinable");
        const FriendGame* e = Get(fg, eve);
        Check(e && e->status == St::VersionMismatch && !e->Joinable() && e->version == "0.13.0", "Eve: other version");
        const FriendGame* f = Get(fg, fred);
        Check(f && f->status == St::Full && !f->Joinable(), "Fred: full");
        bool sorted = true, seenOther = false;
        for (const auto& x : l) {
            if (!x.Joinable()) seenOther = true;
            else if (seenOther) sorted = false;
        }
        Check(sorted && l[0].name == "Dan" && l[1].name == "Miles", "joinable first, then by name",
              l[0].name + "," + l[1].name);
        Check(g.presenceRequests >= 1, "rich presence asked for the friends without a lobby",
              std::to_string(g.presenceRequests));
    }
    // a second scan within a second does nothing; after it Cat's rich presence (now there) gives her lobby, whose
    // data arrives with the request
    const int lobbyReq = g.lobbyRequests, presReq = g.presenceRequests;
    fg.Update(10.5);
    Check(g.lobbyRequests == lobbyReq && g.presenceRequests == presReq, "scans at most once a second");
    fg.Update(11.1);
    {
        const FriendGame* c = Get(fg, cat);
        Check(c && c->lobby == Lobby(2) && c->viaPresence, "Cat: lobby from rich presence");
        Check(c && c->status == St::Checking, "Cat: lobby data asked for, not seen in this scan yet",
              c ? std::to_string(static_cast<int>(c->status)) : "");
    }
    fg.Update(12.2);
    {
        const FriendGame* c = Get(fg, cat);
        Check(c && c->status == St::Hosting && c->players == 2, "Cat: data arrived, hosting (2/4)");
    }
    Check(g.lobbyRequests - lobbyReq <= 2, "lobby data asked again only after lobbyRetryS",
          std::to_string(g.lobbyRequests - lobbyReq));
    // Bob starts hosting; Miles quits
    g.friends[2].connect = "+connect_lobby " + std::to_string(Lobby(7));
    g.lobbyData[Lobby(7)] = Data("0.14.0", bob);
    g.friends[0].game = 0;
    fg.Update(13.3);
    Check(Get(fg, bob) && Get(fg, bob)->status == St::Hosting, "Bob now hosts (rich presence)");
    Check(!Get(fg, miles), "Miles quit: gone from the list");
    // garbage lobby ids are ignored
    g.friends[2].connect = "+connect_lobby 12345";
    fg.Update(14.4);
    Check(Get(fg, bob) && Get(fg, bob)->status == St::NotHosting, "a non-lobby id in rich presence is ignored");
    // ourselves never shows up
    g.friends.push_back({kSelf, "Me", 1, kApp, Lobby(8)});
    fg.Update(15.5);
    Check(!Get(fg, kSelf), "our own account is not listed");

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
