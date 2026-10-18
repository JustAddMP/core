#include "net/steam_flat.h"

#include <windows.h>

#include <map>
#include <memory>
#include <vector>

#include "net/net_internal.h"

namespace mc::net::steam {
using namespace mc::net::detail;

namespace {

Api g_api;
bool g_loaded = false;
bool g_ready = false;

template <class F>
bool Resolve(HMODULE m, F& fn, const char* name, bool required = true) {
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (!fn && required) Log(kError, "steam: export %s not found in steam_api64.dll", name);
    return fn != nullptr || !required;
}

// ---- callback plumbing ----
// Layout-compatible with Steamworks' CCallbackBase on MSVC x64:
//   +0 vtable, +8 uint8 m_nCallbackFlags, +12 int m_iCallback.
// CCallbackBase declares Run(void*), Run(void*, bool, SteamAPICall_t), GetCallbackSizeBytes(). MSVC places
// overloaded virtuals in reverse declaration order, so slot 0 is the 3-arg Run and slot 1 the 1-arg Run. Both of
// our first two slots do the same thing (enqueue the payload, ignore the extra args), so the object behaves
// correctly whichever order the steam_api build uses.
class CallbackObj {
public:
    virtual void RunCallResult(void* data, bool /*ioFailure*/, uint64_t /*call*/) { Enqueue(data); }
    virtual void Run(void* data) { Enqueue(data); }
    virtual int GetCallbackSizeBytes() { return size_; }

    uint8_t flags_ = 0;  // m_nCallbackFlags (steam_api sets the 'registered' bit)
    int id_ = 0;         // m_iCallback
    int size_ = 0;       // ours, past the CCallbackBase part

private:
    void Enqueue(void* data);
};
static_assert(offsetof(CallbackObj, flags_) == 8, "CCallbackBase layout");
static_assert(offsetof(CallbackObj, id_) == 12, "CCallbackBase layout");

struct Item {
    int id;
    std::vector<uint8_t> data;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Item> g_queue;  // guarded by g_lock (written on the game's callback thread)
std::map<int, std::unique_ptr<CallbackObj>> g_objs;
struct Sub {
    int handle;
    std::function<void(const void*)> fn;
};
std::map<int, std::vector<Sub>> g_subs;
int g_nextHandle = 1;

void CallbackObj::Enqueue(void* data) {
    if (!data || size_ <= 0) return;
    Item it{id_, std::vector<uint8_t>(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size_)};
    AcquireSRWLockExclusive(&g_lock);
    if (g_queue.size() < 4096) g_queue.push_back(std::move(it));
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

Api& Get() { return g_api; }
bool Ready() { return g_ready; }

bool Load(HINSTANCE__* module) {
    if (g_loaded && (!module || module == g_api.module)) return true;
    HMODULE m = module ? module : GetModuleHandleW(L"steam_api64.dll");
    if (!m) {
        Log(kWarn, "steam: steam_api64.dll is not loaded");
        return false;
    }
    Api a;
    a.module = m;
    bool ok = true;
    ok &= Resolve(m, a.Init, "SteamAPI_Init");
    ok &= Resolve(m, a.Shutdown, "SteamAPI_Shutdown");
    ok &= Resolve(m, a.RunCallbacks, "SteamAPI_RunCallbacks");
    ok &= Resolve(m, a.IsSteamRunning, "SteamAPI_IsSteamRunning");
    ok &= Resolve(m, a.RegisterCallback, "SteamAPI_RegisterCallback");
    ok &= Resolve(m, a.UnregisterCallback, "SteamAPI_UnregisterCallback");
    ok &= Resolve(m, a.GetHSteamUser, "SteamAPI_GetHSteamUser");
    ok &= Resolve(m, a.FindOrCreateUserInterface, "SteamInternal_FindOrCreateUserInterface");
    ok &= Resolve(m, a.SteamUser_v020, "SteamAPI_SteamUser_v020");
    ok &= Resolve(m, a.SteamFriends_v017, "SteamAPI_SteamFriends_v017");
    ok &= Resolve(m, a.SteamMatchmaking_v009, "SteamAPI_SteamMatchmaking_v009");
    ok &= Resolve(m, a.SteamUtils_v009, "SteamAPI_SteamUtils_v009");
    ok &= Resolve(m, a.SteamNetworking_v006, "SteamAPI_SteamNetworking_v006");
    ok &= Resolve(m, a.User_GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    ok &= Resolve(m, a.Friends_GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= Resolve(m, a.Friends_GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ok &= Resolve(m, a.Friends_ActivateGameOverlayInviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    ok &= Resolve(m, a.Friends_ActivateGameOverlay, "SteamAPI_ISteamFriends_ActivateGameOverlay");
    ok &= Resolve(m, a.Friends_SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    ok &= Resolve(m, a.Friends_ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence");
    // friends list (JOIN FRIEND): optional, the list is simply empty without them
    Resolve(m, a.Friends_GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount", false);
    Resolve(m, a.Friends_GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex", false);
    Resolve(m, a.Friends_GetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState", false);
    Resolve(m, a.Friends_GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed", false);
    Resolve(m, a.Friends_GetFriendRichPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence", false);
    Resolve(m, a.Friends_RequestFriendRichPresence, "SteamAPI_ISteamFriends_RequestFriendRichPresence", false);
    Resolve(m, a.MM_RequestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData", false);
    if (!a.Friends_GetFriendCount || !a.Friends_GetFriendByIndex || !a.Friends_GetFriendGamePlayed)
        Log(kWarn, "steam: friends-list exports missing: JOIN FRIEND can only open the Steam overlay");
    ok &= Resolve(m, a.MM_CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    ok &= Resolve(m, a.MM_JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    ok &= Resolve(m, a.MM_LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    ok &= Resolve(m, a.MM_SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    ok &= Resolve(m, a.MM_GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    ok &= Resolve(m, a.MM_GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    ok &= Resolve(m, a.MM_GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    ok &= Resolve(m, a.MM_GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    ok &= Resolve(m, a.MM_SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    ok &= Resolve(m, a.Utils_IsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= Resolve(m, a.Utils_GetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
    ok &= Resolve(m, a.Utils_GetAppID, "SteamAPI_ISteamUtils_GetAppID");
    ok &= Resolve(m, a.Net_SendP2PPacket, "SteamAPI_ISteamNetworking_SendP2PPacket");
    ok &= Resolve(m, a.Net_IsP2PPacketAvailable, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    ok &= Resolve(m, a.Net_ReadP2PPacket, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    ok &= Resolve(m, a.Net_AcceptP2PSessionWithUser, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    ok &= Resolve(m, a.Net_CloseP2PSessionWithUser, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    ok &= Resolve(m, a.Net_GetP2PSessionState, "SteamAPI_ISteamNetworking_GetP2PSessionState");
    ok &= Resolve(m, a.Net_AllowP2PPacketRelay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
    if (!ok) return false;
    g_api = a;
    g_loaded = true;
    g_ready = false;
    return true;
}

bool AcquireInterfaces() {
    if (!g_loaded) return false;
    if (g_ready) return true;
    Api& a = g_api;
    if (a.GetHSteamUser() == 0) {
        Log(kWarn, "steam: Steam is not initialised (no HSteamUser)");
        return false;
    }
    a.user = a.SteamUser_v020();
    a.friends = a.SteamFriends_v017();
    a.matchmaking = a.SteamMatchmaking_v009();
    a.utils = a.SteamUtils_v009();
    a.networking = a.SteamNetworking_v006();
    if (!a.user || !a.friends || !a.matchmaking || !a.utils || !a.networking) {
        Log(kError, "steam: interface missing (user=%p friends=%p mm=%p utils=%p net=%p)", a.user, a.friends,
            a.matchmaking, a.utils, a.networking);
        return false;
    }
    g_ready = true;
    return true;
}

int Subscribe(int id, int size, std::function<void(const void*)> fn) {
    if (!g_loaded) return 0;
    int handle = g_nextHandle++;
    g_subs[id].push_back({handle, std::move(fn)});
    if (g_objs.count(id)) return handle;
    auto obj = std::make_unique<CallbackObj>();
    obj->id_ = id;
    obj->size_ = size;
    g_api.RegisterCallback(obj.get(), id);
    g_objs[id] = std::move(obj);
    return handle;
}

void Unsubscribe(int handle) {
    for (auto& [id, v] : g_subs)
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].handle == handle) {
                v.erase(v.begin() + static_cast<ptrdiff_t>(i));
                return;
            }
}

void Pump() {
    std::vector<Item> items;
    AcquireSRWLockExclusive(&g_lock);
    items.swap(g_queue);
    ReleaseSRWLockExclusive(&g_lock);
    for (const Item& it : items) {
        auto s = g_subs.find(it.id);
        if (s == g_subs.end()) continue;
        // Copy: a subscriber may (un)subscribe from inside its handler.
        std::vector<Sub> subs = s->second;
        for (auto& sub : subs) sub.fn(it.data.data());
    }
}

void UnregisterAll() {
    if (g_loaded)
        for (auto& [id, obj] : g_objs) g_api.UnregisterCallback(obj.get());
    g_objs.clear();
    g_subs.clear();
    AcquireSRWLockExclusive(&g_lock);
    g_queue.clear();
    ReleaseSRWLockExclusive(&g_lock);
    g_ready = false;
}

void SetApiForTest(const Api& api) {
    g_api = api;
    g_loaded = true;
    g_ready = true;
}

}  // namespace mc::net::steam
