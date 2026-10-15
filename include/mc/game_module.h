#pragma once
// The interface between JustAddMP core and one game.
//
// A game repo provides two things, both compiled into its binaries (core never hard-codes a game):
//   - GetGameInfo(): a constant descriptor (names, Steam app id, supported exe builds, ...). Linked into the launcher,
//     the version.dll proxy and the mod dll, so it must not pull in anything else (put it in its own .cpp).
//   - GetGameModule(): the game layer of the mod dll (hooks into the game, player sync, native menu entries). Only
//     the dll calls it.
// Threads: Init runs on the dll's init thread; everything else on the game's render thread (the Present hook).
#include "mc/game_info.h"
#include "net/session.h"
#include "render/overlay.h"

#include <string>
#include <vector>

namespace mc {

// ---- native menu integration (the game patches its own menus; core's co-op UI drives them) ----
enum class MenuCommand {
    Host,          // HOST CO-OP GAME
    Leave,         // STOP HOSTING / LEAVE CO-OP
    JoinFriend,    // JOIN FRIEND (Steam overlay friends list)
    JoinByIp,      // JOIN BY IP (opens core's text box)
    Invite,        // INVITE FRIENDS (Steam overlay)
    PausePanel,    // pause menu > CO-OP (opens core's co-op panel)
    SetPassword,   // SET / CHANGE PASSWORD (opens core's text box)
    ClearPassword, // CLEAR PASSWORD
    Lobby,         // LOBBY (opens core's lobby panel: players, characters, ready, mode, start)
    Settings,      // SETTINGS (opens core's co-op settings panel)
};

enum class MenuMode { Idle, Hosting, Joining, Connected };

struct MenuState {
    MenuMode mode = MenuMode::Idle;
    int players = 0;
    bool password = false;  // a host password is set
};

// A game setting shown as a row in the pause menu's CO-OP panel (e.g. the player's character): the label on the
// left, the current value on the right between arrows. Left/Right (pad d-pad) step through the values; Enter, pad A
// and a click step forward.
struct PanelOption {
    std::string label;  // "CHARACTER"
    std::string value;  // "PAULIE"
};

// A game mode the lobby can start (index = position in GameModule::LobbyModes), e.g. "FREE RIDE".
struct LobbyMode {
    std::string name;
    bool available = true;  // false: listed but cannot be started yet
    std::string note;       // shown next to an unavailable mode, e.g. "NEXT VERSION"
};

class GameModule {
public:
    virtual ~GameModule() = default;

    // Init thread, once: after core::hook::Init, before core waits for the game window. Install the game's hooks
    // here. A feature that cannot work on this exe reports to core::compat (a required one disables co-op).
    virtual void Init(const std::wstring& dllDir, const std::string& logTag) = 0;

    // Every frame, before the co-op tick.
    virtual void RenderTick(float dt) = 0;

    // ---- session (a net::Session exists from OnSessionStart until OnSessionEnd) ----
    // Register app message handlers on the session here.
    virtual void OnSessionStart(net::Session& session) = 0;
    virtual void OnSessionEnd() = 0;
    virtual void OnPlayerLeft(net::PlayerId id) = 0;
    // Every frame while a session exists, after Session::Poll.
    virtual void SessionTick(float dt) = 0;

    // ---- co-op UI ----
    virtual int PlayerHealth(net::PlayerId) { return -1; }        // 0..100 for the players panel, -1 = unknown
    virtual void AppendStatus(std::vector<render::OverlayLine>&) {}  // lines for the debug status text
    virtual void SetMenuState(const MenuState&) {}                // published every frame
    virtual bool PopMenuCommand(MenuCommand*) { return false; }   // drained every frame
    // Rows for the CO-OP panel, asked every frame while it is open (in every session state, also without one).
    virtual void PanelOptions(std::vector<PanelOption>&) {}
    // The player stepped option `index` (from the last PanelOptions list) by delta (-1 or +1).
    virtual void PanelOptionStep(size_t /*index*/, int /*delta*/) {}

    // ---- co-op settings panel (core 0.5) ----
    // The game's own rows in the CO-OP SETTINGS panel (core adds NAME and DEBUG OVERLAY), e.g. SHOW NAME TAGS.
    virtual void SettingsOptions(std::vector<PanelOption>&) {}
    virtual void SettingsOptionStep(size_t /*index*/, int /*delta*/) {}

    // ---- pre-game lobby (core 0.5; net/lobby.h) ----
    // The modes the host can pick in the lobby, in a fixed order (the index travels on the wire).
    virtual void LobbyModes(std::vector<LobbyMode>&) {}
    // Load `mode` now (the host pressed START, or a late joiner follows the host's game). False = cannot right now
    // (e.g. the player is not at the main menu); core tells the player.
    virtual bool StartMode(int /*mode*/) { return false; }
    // The local player is in a loaded world (not the main menu / a loading screen).
    virtual bool InWorld() { return false; }
    // The lobby may open by itself now (after joining a host): e.g. the game's main menu is up, not a title or
    // loading screen that needs a key press first. Asked every frame until it says yes.
    virtual bool LobbyCanOpen() { return true; }
    // The mode the local player is playing (index into LobbyModes), -1 = none / not one of them.
    virtual int CurrentMode() { return -1; }
    // What the lobby shows as a player's character ("" = nothing).
    virtual std::string PlayerCharacter(net::PlayerId) { return {}; }

    // ---- HUD (core 0.5) ----
    // Every frame: game geometry drawn by core's overlay under its panels and banners (e.g. name tags over other
    // players). w/h: backbuffer size in pixels.
    virtual void DrawHud(render::DrawList&, float /*w*/, float /*h*/) {}
};

GameModule& GetGameModule();  // defined by the game repo (dll only)

}  // namespace mc
