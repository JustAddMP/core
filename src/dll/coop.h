#pragma once
// Co-op connection management, driven from Mod::Tick on the render thread.
//
// Sessions can be started from the game's menus at runtime (Host / JoinAddress / JoinLobby / Leave, see
// dll/coop_ui.h and the game module's menu commands) or from launcher args / the ini at startup:
//   --host                 host: UDP on --port (GameInfo::defaultPort) plus a Steam lobby (P2P) when Steam is
//                          available
//   --join <ip[:port]>     guest over UDP
//   --join-lobby <id>      guest over Steam (also "+connect_lobby <id>" on the game's command line)
//   --transport udp|steam  restrict the host to one transport; with steam, --join <id> is a lobby id
//   --name <name>          player name (default: Steam persona name, else the Windows user name)
//   --password <p>         host: guests must answer with this password; guest: the password to join with
//   require_password_for_invites = 1   (ini) Steam lobby members (invited friends) need the password too
// Steam: we do not rely on the game's own steam_api (Mafia DE memory-loads an old one): we LoadLibrary the on-disk
// steam_api64.dll, call its SteamAPI_Init a few seconds after the first frame and pump its SteamAPI_RunCallbacks
// ourselves every frame.
// Accepting a Steam invite (or "Join Game" in the friends list) while the game runs leaves the current session and
// joins the inviting host. The Steam callback only records the lobby id (dll/pending_join.h); Tick acts on it at the
// start of the next frame, so the session is never torn down while it is being polled.
#include "render/overlay.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

class Coop {
public:
    static void Init();
    static void Tick(float dt);
    // Leave the session cleanly at process exit: the others are told at once (host: "host left"; guest: "left the
    // game") instead of timing out. Any thread; the caller makes sure no Tick runs at the same time (Mod does).
    // Idempotent.
    static void Shutdown();
    // Per-peer bandwidth budget in bytes/s (ini `net_budget_kbps`, KiB/s, default 1024): the host's relay limit per
    // guest; games use it for their net::InterestManager (interest::Config::bytesPerSecond).
    static double NetBudgetBytesPerS();
    // Lines for the on-screen status panel (empty = draw nothing).
    static void GetStatus(std::vector<render::OverlayLine>& out);

    // ---- runtime control (render thread); each one ends the current session first ----
    static void Host();
    static void JoinAddress(const std::string& address);  // UDP, "ip[:port]"
    static void JoinLobby(uint64_t lobbyId);              // Steam lobby; its owner is the host
    static void Leave();
    static bool InviteFriends();     // Steam overlay invite dialog for our lobby (hosting over Steam)
    static bool OpenSteamFriends();  // Steam overlay friends list (Join Game / invites)

    // ---- JOIN FRIEND (net/steam_friends.h) ----
    struct FriendRow {
        uint64_t steamId = 0;
        std::string name;
        std::string detail;     // "HOSTING (2/4) - IN THE LOBBY", "PLAYING, NOT HOSTING", "VERSION 0.13.0 - ..."
        bool joinable = false;
        bool warn = false;      // another version / full: shown as a problem
        bool playing = false;   // the host is in the world (mode = what it plays)
        int mode = -1;
    };
    struct FriendList {
        bool steamReady = false;
        bool available = false;  // the friends-list API exists (else only the Steam overlay)
        bool scanned = false;    // at least one scan done
        int friends = 0, online = 0;
        std::vector<FriendRow> rows;  // friends in this game; joinable first
    };
    // Rescans at most once a second; call while the JOIN FRIEND panel is open.
    static FriendList GetFriends();
    static void RefreshFriends();  // forget the cached state, ask Steam again
    // Join the co-op game of a friend from GetFriends (their lobby: theirs, or the host's when they are a guest).
    // False with a notice when it cannot be joined (not hosting, other version, full, gone).
    static bool JoinFriend(uint64_t steamId);
    // Session password. Host side: "" = none; applies to the running host session too (players in it stay).
    static void SetPassword(const std::string& password);
    // Guest side: the last join was refused for its password (required / wrong); true once per refusal, so the UI
    // can ask for one and call RetryWithPassword.
    static bool PopPasswordPrompt();
    static void RetryWithPassword(const std::string& password);  // repeats the last join with this password
    // Host: kick a guest (ban = also refuse its rejoins for the rest of this hosted session; net::Session::Kick /
    // KickAndBan). False when not hosting or the player is gone.
    static bool Kick(uint8_t playerId, bool ban);
    // Host: names on this session's ban list (empty when not hosting), and Unban by index into it.
    static std::vector<std::string> BanList();
    static bool Unban(size_t index);

    // ---- pre-game lobby (net/lobby.h; render thread) ----
    static void SetReady(bool ready);    // guest
    static void SetLobbyMode(int mode);  // host
    // Host: START for everyone (force = even if a guest is not ready). False when not hosting / not all ready / the
    // mode is unavailable.
    static bool StartGame(bool force = false);
    // Guest: load the mode the host is playing (late join). False when the host is not in a game.
    static bool JoinRunningGame();
    // True once after we joined a host while not in the world: the UI opens the lobby.
    static bool PopOpenLobby();

    // ---- settings ----
    // The name saved in the settings (CO-OP SETTINGS > NAME; "" = Steam persona / Windows user). A launch-time
    // --name (tests) is used for that run only: it is not what the NAME box shows and is never saved by it.
    static std::string Name();
    static std::string EffectiveName();  // what the others see: --name, else the saved name, else the Steam persona
    static void SetName(const std::string& name);  // saved; used from the next session
    enum class Phase { Idle, Starting, Hosting, Joining, Connected };
    struct PlayerRow {
        uint8_t id = 0;  // net::PlayerId (0 = host)
        std::string name;
        int pingMs = -1;  // -1 = none (the host's own row)
        int health = -1;  // 0..100, -1 = unknown (not in the world yet)
        bool host = false, you = false;
        bool ready = false;     // lobby: this guest is ready
        std::string character;  // GameModule::PlayerCharacter
    };
    struct Info {
        Phase phase = Phase::Idle;
        std::string target;        // host name / address while joining or connected
        std::vector<PlayerRow> players;
        bool steamReady = false;
        bool lobbyOpen = false;    // hosting with a Steam lobby (friends can join via the overlay)
        uint16_t port = 0;         // UDP port while hosting
        std::string localAddress;  // this PC's LAN IPv4 (for "join by IP")
        std::string lastAddress;   // last address passed to JoinAddress (prefill for the input box)
        bool passwordSet = false;  // a host password is set (hosting needs it from guests)
        // lobby (while a session is active)
        int lobbyMode = 0;         // the host's picked mode
        bool hostInGame = false;   // the host is playing
        int hostGameMode = -1;     // ... this mode
        bool allReady = true;      // every guest is ready
        bool ready = false;        // we are ready (guest)
        uint32_t starts = 0;       // lobby starts seen (the lobby panel closes when it changes)
    };
    static Info GetInfo();

    // Events worth telling the player about (joined/left/connected/errors/invites), oldest first.
    struct Notice {
        std::string title, text;
        bool error = false;
    };
    static bool PopNotice(Notice* out);
};

} // namespace mc
