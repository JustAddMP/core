// net: minimal Steamworks flat-API bindings, resolved at runtime with GetProcAddress from the
// steam_api64.dll the game already loaded. No Steamworks SDK headers: the few signatures/structs we use are
// declared here from the public Steamworks documentation.
//
// Interface versions are the ones the game's steam_api64.dll exports (checked with pefile, see docs/protocol.md):
//   SteamUser020, SteamFriends017, SteamMatchMaking009, SteamUtils009, SteamNetworking006.
// That dll predates ISteamNetworkingMessages (no SteamAPI_SteamNetworkingMessages_* exports), so the Steam
// transport uses ISteamNetworking006 P2P (SendP2PPacket/ReadP2PPacket), which has the same session semantics.
#pragma once
#include <cstdint>
#include <functional>

struct HINSTANCE__;

namespace mc::net::steam {

using SteamAPICall_t = uint64_t;
using HSteamUser = int32_t;
constexpr SteamAPICall_t kInvalidAPICall = 0;

// ---- enums (values from the public Steamworks headers) ----
enum ELobbyType : int { k_ELobbyTypePrivate = 0, k_ELobbyTypeFriendsOnly = 1, k_ELobbyTypePublic = 2 };
enum EP2PSend : int {
    k_EP2PSendUnreliable = 0,
    k_EP2PSendUnreliableNoDelay = 1,
    k_EP2PSendReliable = 2,
    k_EP2PSendReliableWithBuffering = 3,
};
constexpr int k_EResultOK = 1;
constexpr uint32_t k_EChatRoomEnterResponseSuccess = 1;

// ---- callback ids (k_iSteamXCallbacks base + offset) ----
constexpr int kCb_GameLobbyJoinRequested = 333;        // k_iSteamFriendsCallbacks + 33
constexpr int kCb_GameRichPresenceJoinRequested = 337; // k_iSteamFriendsCallbacks + 37
constexpr int kCb_LobbyEnter = 504;                    // k_iSteamMatchmakingCallbacks + 4
constexpr int kCb_LobbyChatUpdate = 506;               // k_iSteamMatchmakingCallbacks + 6
constexpr int kCb_LobbyCreated = 513;                  // k_iSteamMatchmakingCallbacks + 13
constexpr int kCb_P2PSessionRequest = 1202;            // k_iSteamNetworkingCallbacks + 2
constexpr int kCb_P2PSessionConnectFail = 1203;        // k_iSteamNetworkingCallbacks + 3

// ---- callback structs. Steam packs callbacks at 8 on Windows; CSteamID itself is pack(1) (align 1). ----
#pragma pack(push, 8)
struct LobbyCreated_t {
    int32_t m_eResult;
    uint64_t m_ulSteamIDLobby;
};
static_assert(sizeof(LobbyCreated_t) == 16);
struct LobbyEnter_t {
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    bool m_bLocked;
    uint32_t m_EChatRoomEnterResponse;
};
static_assert(sizeof(LobbyEnter_t) == 24);
struct LobbyChatUpdate_t {
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDUserChanged;
    uint64_t m_ulSteamIDMakingChange;
    uint32_t m_rgfChatMemberStateChange;
};
static_assert(sizeof(LobbyChatUpdate_t) == 32);
#pragma pack(pop)
#pragma pack(push, 1)
struct CSteamIDRaw {
    uint64_t v;
};
struct GameLobbyJoinRequested_t {
    CSteamIDRaw m_steamIDLobby;
    CSteamIDRaw m_steamIDFriend;
};
static_assert(sizeof(GameLobbyJoinRequested_t) == 16);
struct GameRichPresenceJoinRequested_t {
    CSteamIDRaw m_steamIDFriend;
    char m_rgchConnect[256];
};
static_assert(sizeof(GameRichPresenceJoinRequested_t) == 264);
struct P2PSessionRequest_t {
    CSteamIDRaw m_steamIDRemote;
};
struct P2PSessionConnectFail_t {
    CSteamIDRaw m_steamIDRemote;
    uint8_t m_eP2PSessionError;
};
static_assert(sizeof(P2PSessionConnectFail_t) == 9);
#pragma pack(pop)
#pragma pack(push, 8)
struct P2PSessionState_t {
    uint8_t m_bConnectionActive;
    uint8_t m_bConnecting;
    uint8_t m_eP2PSessionError;
    uint8_t m_bUsingRelay;
    int32_t m_nBytesQueuedForSend;
    int32_t m_nPacketsQueuedForSend;
    uint32_t m_nRemoteIP;
    uint16_t m_nRemotePort;
};
static_assert(sizeof(P2PSessionState_t) == 20);
#pragma pack(pop)

// ---- the resolved API ----
struct Api {
    HINSTANCE__* module = nullptr;

    // core (SteamAPI_*)
    bool (*Init)() = nullptr;  // only for standalone tools: in-game the game initialises Steam
    void (*Shutdown)() = nullptr;
    void (*RunCallbacks)() = nullptr;
    bool (*IsSteamRunning)() = nullptr;
    void (*RegisterCallback)(void* cb, int id) = nullptr;
    void (*UnregisterCallback)(void* cb) = nullptr;
    HSteamUser (*GetHSteamUser)() = nullptr;
    void* (*FindOrCreateUserInterface)(HSteamUser, const char* version) = nullptr;

    // versioned accessors
    void* (*SteamUser_v020)() = nullptr;
    void* (*SteamFriends_v017)() = nullptr;
    void* (*SteamMatchmaking_v009)() = nullptr;
    void* (*SteamUtils_v009)() = nullptr;
    void* (*SteamNetworking_v006)() = nullptr;

    // ISteamUser
    uint64_t (*User_GetSteamID)(void* self) = nullptr;
    // ISteamFriends
    const char* (*Friends_GetPersonaName)(void* self) = nullptr;
    const char* (*Friends_GetFriendPersonaName)(void* self, uint64_t id) = nullptr;
    void (*Friends_ActivateGameOverlayInviteDialog)(void* self, uint64_t lobby) = nullptr;
    void (*Friends_ActivateGameOverlay)(void* self, const char* dialog) = nullptr;  // "Friends", "Players", ...
    bool (*Friends_SetRichPresence)(void* self, const char* key, const char* value) = nullptr;
    void (*Friends_ClearRichPresence)(void* self) = nullptr;
    // ISteamMatchmaking
    SteamAPICall_t (*MM_CreateLobby)(void* self, int type, int maxMembers) = nullptr;
    SteamAPICall_t (*MM_JoinLobby)(void* self, uint64_t lobby) = nullptr;
    void (*MM_LeaveLobby)(void* self, uint64_t lobby) = nullptr;
    bool (*MM_SetLobbyData)(void* self, uint64_t lobby, const char* key, const char* value) = nullptr;
    const char* (*MM_GetLobbyData)(void* self, uint64_t lobby, const char* key) = nullptr;
    uint64_t (*MM_GetLobbyOwner)(void* self, uint64_t lobby) = nullptr;
    int (*MM_GetNumLobbyMembers)(void* self, uint64_t lobby) = nullptr;
    uint64_t (*MM_GetLobbyMemberByIndex)(void* self, uint64_t lobby, int index) = nullptr;
    bool (*MM_SetLobbyJoinable)(void* self, uint64_t lobby, bool joinable) = nullptr;
    // ISteamUtils
    bool (*Utils_IsAPICallCompleted)(void* self, SteamAPICall_t call, bool* failed) = nullptr;
    bool (*Utils_GetAPICallResult)(void* self, SteamAPICall_t call, void* out, int size, int expectedId,
                                   bool* failed) = nullptr;
    uint32_t (*Utils_GetAppID)(void* self) = nullptr;
    // ISteamNetworking (006)
    bool (*Net_SendP2PPacket)(void* self, uint64_t to, const void* data, uint32_t size, int sendType,
                              int channel) = nullptr;
    bool (*Net_IsP2PPacketAvailable)(void* self, uint32_t* size, int channel) = nullptr;
    bool (*Net_ReadP2PPacket)(void* self, void* dest, uint32_t cap, uint32_t* size, uint64_t* from,
                              int channel) = nullptr;
    bool (*Net_AcceptP2PSessionWithUser)(void* self, uint64_t id) = nullptr;
    bool (*Net_CloseP2PSessionWithUser)(void* self, uint64_t id) = nullptr;
    bool (*Net_GetP2PSessionState)(void* self, uint64_t id, P2PSessionState_t* out) = nullptr;
    bool (*Net_AllowP2PPacketRelay)(void* self, bool allow) = nullptr;

    // interface pointers (valid after AcquireInterfaces)
    void* user = nullptr;
    void* friends = nullptr;
    void* matchmaking = nullptr;
    void* utils = nullptr;
    void* networking = nullptr;
};

// Resolve exports. module == nullptr: use the already-loaded steam_api64.dll (GetModuleHandle, never LoadLibrary).
bool Load(HINSTANCE__* module = nullptr);
// Fetch interface pointers; Steam must be initialised (by the game, or by Api::Init in a standalone tool).
bool AcquireInterfaces();
bool Ready();  // loaded and interfaces acquired
Api& Get();

// ---- broadcast callbacks ----
// Registers a CCallbackBase-compatible object with SteamAPI_RegisterCallback for `id` (once per id), so the
// callback is delivered by whoever calls SteamAPI_RunCallbacks (the game, on its own thread). The payload is
// copied into a locked queue there; Pump() hands it to subscribers on OUR thread. See docs/protocol.md.
// Returns a handle for Unsubscribe (subscribers must unsubscribe before they are destroyed).
int Subscribe(int id, int size, std::function<void(const void* data)> fn);
void Unsubscribe(int handle);
void Pump();
// Unregister all callback objects. Only for standalone tools before SteamAPI_Shutdown; the dll never calls it
// (the game's RunCallbacks may be iterating on another thread).
void UnregisterAll();

}  // namespace mc::net::steam
