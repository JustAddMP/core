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
};

GameModule& GetGameModule();  // defined by the game repo (dll only)

}  // namespace mc
