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
    static void Shutdown();  // leave the session cleanly (process exit)
    // Lines for the on-screen status panel (empty = draw nothing).
    static void GetStatus(std::vector<render::OverlayLine>& out);

    // ---- runtime control (render thread); each one ends the current session first ----
    static void Host();
    static void JoinAddress(const std::string& address);  // UDP, "ip[:port]"
    static void JoinLobby(uint64_t lobbyId);              // Steam lobby; its owner is the host
    static void Leave();
    static bool InviteFriends();     // Steam overlay invite dialog for our lobby (hosting over Steam)
    static bool OpenSteamFriends();  // Steam overlay friends list (Join Game / invites)
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

    enum class Phase { Idle, Starting, Hosting, Joining, Connected };
    struct PlayerRow {
        uint8_t id = 0;  // net::PlayerId (0 = host)
        std::string name;
        int pingMs = -1;  // -1 = none (the host's own row)
        int health = -1;  // 0..100, -1 = unknown (not in the world yet)
        bool host = false, you = false;
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
