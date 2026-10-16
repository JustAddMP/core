#include "dll/coop_ui.h"

#include "core/compat.h"
#include "core/log.h"
#include "dll/coop.h"
#include "dll/mod.h"
#include "dll/ui_input.h"
#include "core/config.h"
#include "mc/game_module.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace mc {
namespace {

// ---- look (taken from the game's menus: gold headers, off-white uppercase items, gold-tinted selection) --------
constexpr uint32_t kGold = 0xFFD1AC62;
constexpr uint32_t kGoldDim = 0xFF8C7442;
constexpr uint32_t kText = 0xFFE9E5DB;
constexpr uint32_t kTextDim = 0xFFA8A398;
constexpr uint32_t kRed = 0xFFD2483C;
constexpr uint32_t kShade = 0xB4000000;      // full-screen dim behind a panel
constexpr uint32_t kPanelBg = 0xE60D0C0B;
constexpr uint32_t kToastBg = 0xE0100E0C;

constexpr float kToastSeconds = 5.0f;
constexpr float kCompatToastSeconds = 45.0f;
constexpr float kFade = 0.25f;

struct Toast {
    std::string title, text;
    bool error;
    float seconds = kToastSeconds;  // how long it stays up
    float age = 0;
};

// PlayerMenu: what to do with the guest picked in the CO-OP panel's player list (host only); ConfirmKick: "Kick
// <name>?" before it happens.
// Lobby: the pre-game lobby (players with character and ready state, mode, start); Settings: CO-OP SETTINGS (name,
// debug overlay, the game's settings); Name: the text box for the player name.
enum class Panel { None, PauseCoop, JoinIp, Password, PlayerMenu, ConfirmKick, Lobby, Settings, Name };
enum class Action {
    None, Host, Invite, JoinFriend, JoinIp, Leave, Back, Connect, SetPassword, ClearPassword, SubmitPassword,
    PickPlayer, Kick, KickBan, ConfirmKick, Option,
    // lobby
    Ready, Mode, Start, JoinGame, OpenLobby,
    // settings
    OpenSettings, EditName, SubmitName, DebugOverlay, Setting
};

struct Item {
    std::string label;
    Action action;
    float y0 = 0, y1 = 0, x0 = 0, x1 = 0;  // hit box from the last draw
    int player = -1;  // PickPlayer: the guest's player id (drawn as its row in the player list, not as an item)
    int option = -1;  // Option: index into the game's PanelOptions list
    std::string value;  // Option: its current value
};

struct UiState {
    std::vector<Toast> toasts;
    Panel panel = Panel::None;
    std::vector<Item> items;
    int selected = 0;
    std::string ipText;
    std::string pwText;         // Password panel (shown masked)
    std::string nameText;       // Name panel
    Panel returnTo = Panel::None;  // Settings / Name: the panel to go back to (Back), None = close
    bool pwForHost = false;     // Password panel: true = set the host's password, false = retry the refused join
    int targetId = -1;          // PlayerMenu / ConfirmKick: the guest picked in the player list
    std::string targetName;
    bool targetBan = false;     // ConfirmKick: kick and ban
    float caret = 0;
    uint16_t padPrev = 0;
    float padRepeat = 0;
    render::DrawList list;
    Coop::Info info;
    float mouseX = -1, mouseY = -1;
    bool compatChecked = false;
    uint32_t starts = 0;  // Coop::Info::starts last frame
    bool wasInSession = false;
};

UiState& U() {
    static UiState* u = new UiState;
    return *u;
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void AddToast(std::string title, std::string text, bool error, float seconds = kToastSeconds) {
    UiState& u = U();
    MC_LOG_INFO("ui: notice: %s: %s", title.c_str(), text.c_str());
    if (u.toasts.size() >= 3) u.toasts.erase(u.toasts.begin());
    u.toasts.push_back({std::move(title), std::move(text), error, seconds});
}

// Once, on the first frame: tell the player when this game build is not (fully) supported (core/compat.h) instead
// of letting features fail silently. Stays up longer than other banners so it is seen past the intro videos.
void ShowCompatBanner() {
    const std::string mod = GetGameInfo().modName, ver = GetGameInfo().modVersion;
    const std::string crc = core::compat::UnsupportedBuildCrc();
    const std::string missing = core::compat::FailedList();
    const std::string found = crc.empty() ? std::string() : " (exe CRC " + crc + ")";
    if (core::compat::CoopDisabled()) {
        AddToast("Co-op disabled: unsupported game version",
                 "This game version" + found + " is not supported by " + mod + " " + ver + " (missing: " + missing +
                     "). Check for a newer " + mod + ".",
                 true, kCompatToastSeconds);
    } else if (!missing.empty()) {
        AddToast("Unsupported game version",
                 "Co-op " + missing + " are not available on this game version" + found + ". Check for a newer " +
                     mod + ".",
                 true, kCompatToastSeconds);
    } else if (!crc.empty()) {
        AddToast("Unsupported game version",
                 mod + " " + ver + " was not made for this game version" + found + ". Co-op may not work correctly.",
                 true, kCompatToastSeconds);
    }
}

// ---- panels -------------------------------------------------------------------------------------------------------
// The panels with a text box (same input code and look; the password is shown masked).
bool TextPanel(Panel p) { return p == Panel::JoinIp || p == Panel::Password || p == Panel::Name; }
std::string& TextBuf() {
    UiState& u = U();
    return u.panel == Panel::Password ? u.pwText : u.panel == Panel::Name ? u.nameText : u.ipText;
}
Action SubmitAction() {
    const Panel p = U().panel;
    return p == Panel::Password ? Action::SubmitPassword : p == Panel::Name ? Action::SubmitName : Action::Connect;
}

std::string ModeName(int mode, bool withNote = false) {
    std::vector<LobbyMode> modes;
    GetGameModule().LobbyModes(modes);
    if (mode < 0 || static_cast<size_t>(mode) >= modes.size()) return mode < 0 ? "-" : "MODE " + std::to_string(mode);
    const LobbyMode& m = modes[static_cast<size_t>(mode)];
    return withNote && !m.available && !m.note.empty() ? m.name + " (" + m.note + ")" : m.name;
}

bool Hosting() {
    const Coop::Phase ph = U().info.phase;
    return ph == Coop::Phase::Hosting || ph == Coop::Phase::Starting;
}
bool InSession() {
    const Coop::Phase ph = U().info.phase;
    return ph != Coop::Phase::Idle;
}

void PasswordItems() {
    UiState& u = U();
    u.items.push_back({u.info.passwordSet ? "CHANGE PASSWORD" : "SET PASSWORD", Action::SetPassword});
    if (u.info.passwordSet) u.items.push_back({"CLEAR PASSWORD", Action::ClearPassword});
}

int FirstMenuItem() {  // first item that is not a player row
    const UiState& u = U();
    for (size_t i = 0; i < u.items.size(); ++i)
        if (u.items[i].player < 0) return static_cast<int>(i);
    return 0;
}

void RestoreSelection(Action selAction, int selPlayer, int selOption);

void BuildPauseItems() {
    UiState& u = U();
    // keep the selection on the same entry while players come and go (rebuilt every frame while open)
    Action selAction = Action::None;
    int selPlayer = -1, selOption = -1;
    if (u.selected >= 0 && u.selected < static_cast<int>(u.items.size())) {
        selAction = u.items[static_cast<size_t>(u.selected)].action;
        selPlayer = u.items[static_cast<size_t>(u.selected)].player;
        selOption = u.items[static_cast<size_t>(u.selected)].option;
    }
    u.items.clear();
    // Hosting: each guest's row in the player list can be selected (Enter: kick / kick and ban).
    if (u.info.phase == Coop::Phase::Hosting)
        for (const auto& p : u.info.players)
            if (!p.host) {
                Item it{p.name, Action::PickPlayer};
                it.player = p.id;
                u.items.push_back(it);
            }
    // the game's settings (e.g. CHARACTER), in every state
    std::vector<PanelOption> options;
    GetGameModule().PanelOptions(options);
    for (size_t i = 0; i < options.size(); ++i) {
        Item it{options[i].label, Action::Option};
        it.option = static_cast<int>(i);
        it.value = options[i].value;
        u.items.push_back(it);
    }
    switch (u.info.phase) {
    case Coop::Phase::Idle:
        u.items.push_back({"HOST CO-OP GAME", Action::Host});
        u.items.push_back({"JOIN FRIEND", Action::JoinFriend});
        u.items.push_back({"JOIN BY IP", Action::JoinIp});
        PasswordItems();
        break;
    case Coop::Phase::Starting:
    case Coop::Phase::Hosting:
        u.items.push_back({"INVITE FRIENDS", Action::Invite});
        PasswordItems();
        u.items.push_back({"STOP HOSTING", Action::Leave});
        break;
    case Coop::Phase::Joining:
    case Coop::Phase::Connected:
        u.items.push_back({"LEAVE CO-OP", Action::Leave});
        break;
    }
    u.items.push_back({"SETTINGS", Action::OpenSettings});
    u.items.push_back({"BACK", Action::Back});
    RestoreSelection(selAction, selPlayer, selOption);
}

// The lobby: the guests (host: selectable for kick), the game's character rows, then READY (guest) or MODE and START
// (host), JOIN GAME while the host is already playing, INVITE FRIENDS, SETTINGS, LEAVE, BACK. Rebuilt every frame.
void BuildLobbyItems() {
    UiState& u = U();
    Action selAction = Action::None;
    int selPlayer = -1, selOption = -1;
    if (u.selected >= 0 && u.selected < static_cast<int>(u.items.size())) {
        selAction = u.items[static_cast<size_t>(u.selected)].action;
        selPlayer = u.items[static_cast<size_t>(u.selected)].player;
        selOption = u.items[static_cast<size_t>(u.selected)].option;
    }
    u.items.clear();
    const bool host = Hosting();
    if (host)
        for (const auto& p : u.info.players)
            if (!p.host) {
                Item it{p.name, Action::PickPlayer};
                it.player = p.id;
                u.items.push_back(it);
            }
    std::vector<PanelOption> options;
    GetGameModule().PanelOptions(options);
    for (size_t i = 0; i < options.size(); ++i) {
        Item it{options[i].label, Action::Option};
        it.option = static_cast<int>(i);
        it.value = options[i].value;
        u.items.push_back(it);
    }
    if (host) {
        Item mode{"MODE", Action::Mode};
        mode.value = ModeName(u.info.lobbyMode, true);
        u.items.push_back(mode);
        std::vector<LobbyMode> modes;
        GetGameModule().LobbyModes(modes);
        const bool avail = u.info.lobbyMode >= 0 && static_cast<size_t>(u.info.lobbyMode) < modes.size() &&
                           modes[static_cast<size_t>(u.info.lobbyMode)].available;
        u.items.push_back({!avail                ? "START (NOT AVAILABLE YET)"
                           : u.info.allReady ? "START " + ModeName(u.info.lobbyMode)
                                             : "START (WAITING FOR PLAYERS)",
                           Action::Start});
        u.items.push_back({"INVITE FRIENDS", Action::Invite});
    } else if (u.info.phase == Coop::Phase::Connected) {
        if (u.info.hostInGame) {
            u.items.push_back({"JOIN GAME (" + ModeName(u.info.hostGameMode) + ")", Action::JoinGame});
        } else {
            Item ready{"READY", Action::Ready};
            ready.value = u.info.ready ? "YES" : "NO";
            u.items.push_back(ready);
        }
    }
    u.items.push_back({"SETTINGS", Action::OpenSettings});
    if (InSession()) u.items.push_back({host ? "STOP HOSTING" : "LEAVE CO-OP", Action::Leave});
    u.items.push_back({"BACK", Action::Back});
    RestoreSelection(selAction, selPlayer, selOption);
}

// CO-OP SETTINGS: NAME, DEBUG OVERLAY, then the game's rows, BACK.
void BuildSettingsItems() {
    UiState& u = U();
    Action selAction = Action::None;
    int selOption = -1;
    if (u.selected >= 0 && u.selected < static_cast<int>(u.items.size())) {
        selAction = u.items[static_cast<size_t>(u.selected)].action;
        selOption = u.items[static_cast<size_t>(u.selected)].option;
    }
    u.items.clear();
    Item name{"NAME", Action::EditName};
    name.value = Coop::EffectiveName();
    u.items.push_back(name);
    Item dbg{"DEBUG OVERLAY", Action::DebugOverlay};
    dbg.value = Mod::DebugOverlay() ? "ON" : "OFF";
    u.items.push_back(dbg);
    std::vector<PanelOption> options;
    GetGameModule().SettingsOptions(options);
    for (size_t i = 0; i < options.size(); ++i) {
        Item it{options[i].label, Action::Setting};
        it.option = static_cast<int>(i);
        it.value = options[i].value;
        u.items.push_back(it);
    }
    u.items.push_back({"BACK", Action::Back});
    RestoreSelection(selAction, -1, selOption);
}

void RestoreSelection(Action selAction, int selPlayer, int selOption) {
    UiState& u = U();
    for (size_t i = 0; i < u.items.size(); ++i)
        if (selAction != Action::None && u.items[i].action == selAction && u.items[i].player == selPlayer &&
            u.items[i].option == selOption) {
            u.selected = static_cast<int>(i);
            return;
        }
    if (selAction == Action::PickPlayer) u.selected = FirstMenuItem();  // that player left
    u.selected = std::clamp(u.selected, 0, static_cast<int>(u.items.size()) - 1);
}

// A row that shows "< VALUE >" and steps with Left/Right.
bool Stepper(const Item& it) {
    return it.option >= 0 || it.action == Action::Mode || it.action == Action::Ready ||
           it.action == Action::DebugOverlay || it.action == Action::EditName;
}

void Open(Panel p, bool pwForHost = false) {
    UiState& u = U();
    u.panel = p;
    u.selected = 0;
    u.mouseX = u.mouseY = -1;
    u.padPrev = ui_input::PadButtons();  // a held A (that opened us) is not a new press
    if (p == Panel::PauseCoop) {
        u.items.clear();
        BuildPauseItems();
        u.selected = FirstMenuItem();  // start on the menu, not on a player row
    }
    if (p == Panel::Lobby) {
        u.items.clear();
        BuildLobbyItems();
        u.selected = FirstMenuItem();
    }
    if (p == Panel::Settings) {
        u.items.clear();
        BuildSettingsItems();
    }
    if (p == Panel::Name) {
        u.nameText = Coop::Name();
        u.items = {{"SAVE", Action::SubmitName}, {"CANCEL", Action::Back}};
    }
    if (p == Panel::PlayerMenu)
        u.items = {{"KICK", Action::Kick}, {"KICK AND BAN", Action::KickBan}, {"CANCEL", Action::Back}};
    if (p == Panel::ConfirmKick) u.items = {{"YES", Action::ConfirmKick}, {"NO", Action::Back}};
    if (p == Panel::JoinIp) {
        u.ipText = u.info.lastAddress;
        u.items = {{"CONNECT", Action::Connect}, {"CANCEL", Action::Back}};
    }
    if (p == Panel::Password) {
        u.pwText.clear();
        u.pwForHost = pwForHost;
        u.items = {{pwForHost ? "SET PASSWORD" : "CONNECT", Action::SubmitPassword}, {"CANCEL", Action::Back}};
    }
    ui_input::SetCapture(true);
    MC_LOG_INFO("ui: %s panel opened", p == Panel::PauseCoop    ? "co-op"
                                       : p == Panel::JoinIp     ? "join-by-ip"
                                       : p == Panel::Password   ? "password"
                                       : p == Panel::PlayerMenu ? "player"
                                       : p == Panel::Lobby      ? "lobby"
                                       : p == Panel::Settings   ? "settings"
                                       : p == Panel::Name       ? "name"
                                                                : "confirm-kick");
}

void Close() {
    UiState& u = U();
    if (u.panel == Panel::None) return;
    u.panel = Panel::None;
    u.returnTo = Panel::None;
    u.items.clear();
    ui_input::SetCapture(false);
    MC_LOG_INFO("ui: panel closed");
}

// Host: the next/previous mode in the game's list (unavailable ones are shown too, START says so).
void StepMode(int delta) {
    std::vector<LobbyMode> modes;
    GetGameModule().LobbyModes(modes);
    if (modes.empty()) return;
    const int n = static_cast<int>(modes.size());
    const int next = ((U().info.lobbyMode + delta) % n + n) % n;
    Coop::SetLobbyMode(next);
}

void Run(Action a) {
    UiState& u = U();
    switch (a) {
    case Action::Host: Close(); Coop::Host(); break;
    case Action::Invite: Close(); Coop::InviteFriends(); break;
    case Action::JoinFriend: Close(); Coop::OpenSteamFriends(); break;
    case Action::JoinIp: Open(Panel::JoinIp); break;
    case Action::Leave: Close(); Coop::Leave(); break;
    case Action::Back: {
        const Panel back = u.panel == Panel::Name ? Panel::Settings : u.panel == Panel::Settings ? u.returnTo
                                                                                                 : Panel::None;
        const Panel outer = u.returnTo;
        Close();
        if (back != Panel::None) {
            Open(back);
            if (back == Panel::Settings) u.returnTo = outer;
        }
        break;
    }
    case Action::SetPassword: Open(Panel::Password, true); break;
    case Action::ClearPassword: Close(); Coop::SetPassword(""); break;
    case Action::SubmitPassword: {
        const std::string pw = u.pwText;
        if (pw.empty()) {
            AddToast("PASSWORD", "Type the password first", true);
            return;
        }
        const bool forHost = u.pwForHost;
        u.pwText.clear();
        Close();
        if (forHost)
            Coop::SetPassword(pw);
        else
            Coop::RetryWithPassword(pw);
        break;
    }
    case Action::Connect: {
        std::string addr = u.ipText;
        addr.erase(std::remove(addr.begin(), addr.end(), ' '), addr.end());
        if (addr.empty()) {
            AddToast("JOIN BY IP", "Type the host's IP address first", true);
            return;
        }
        Close();
        Coop::JoinAddress(addr);
        break;
    }
    case Action::PickPlayer: {
        const Item& it = u.items[static_cast<size_t>(u.selected)];
        u.targetId = it.player;
        u.targetName = it.label;
        Open(Panel::PlayerMenu);
        break;
    }
    case Action::Kick:
    case Action::KickBan:
        u.targetBan = a == Action::KickBan;
        Open(Panel::ConfirmKick);
        break;
    case Action::ConfirmKick: {
        const int id = u.targetId;
        const bool ban = u.targetBan;
        const std::string name = u.targetName;
        Close();
        if (id < 0 || !Coop::Kick(static_cast<uint8_t>(id), ban))
            AddToast("CO-OP", name + " is no longer in the game", true);
        break;
    }
    case Action::Option: {
        const Item& it = u.items[static_cast<size_t>(u.selected)];
        if (it.option >= 0) GetGameModule().PanelOptionStep(static_cast<size_t>(it.option), +1);
        break;
    }
    case Action::Setting: {
        const Item& it = u.items[static_cast<size_t>(u.selected)];
        if (it.option >= 0) GetGameModule().SettingsOptionStep(static_cast<size_t>(it.option), +1);
        break;
    }
    case Action::Ready: Coop::SetReady(!u.info.ready); break;
    case Action::Mode: StepMode(+1); break;
    case Action::Start:
        if (Coop::StartGame()) Close();
        break;
    case Action::JoinGame:
        if (Coop::JoinRunningGame()) Close();
        break;
    case Action::OpenLobby: Open(Panel::Lobby); break;
    case Action::OpenSettings: {
        const Panel from = u.panel;
        Open(Panel::Settings);
        u.returnTo = from == Panel::Settings ? Panel::None : from;
        break;
    }
    case Action::EditName: {
        const Panel outer = u.returnTo;
        Open(Panel::Name);
        u.returnTo = outer;
        break;
    }
    case Action::SubmitName: {
        std::string name = u.nameText;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const Panel outer = u.returnTo;
        Coop::SetName(name);  // "" = back to the Steam / Windows name
        Close();
        Open(Panel::Settings);
        u.returnTo = outer;
        break;
    }
    case Action::DebugOverlay: Mod::SetDebugOverlay(!Mod::DebugOverlay(), true); break;
    case Action::None: break;
    }
}

// Left/Right on a "< VALUE >" row: step it. False if the selected row is not one.
bool StepOption(int delta) {
    UiState& u = U();
    if (TextPanel(u.panel) || u.selected < 0 || u.selected >= static_cast<int>(u.items.size())) return false;
    const Item& it = u.items[static_cast<size_t>(u.selected)];
    switch (it.action) {
    case Action::Option: GetGameModule().PanelOptionStep(static_cast<size_t>(it.option), delta); return true;
    case Action::Setting: GetGameModule().SettingsOptionStep(static_cast<size_t>(it.option), delta); return true;
    case Action::Mode: StepMode(delta); return true;
    case Action::Ready: Coop::SetReady(!u.info.ready); return true;
    case Action::DebugOverlay: Mod::SetDebugOverlay(!Mod::DebugOverlay(), true); return true;
    default: return false;
    }
}

void Move(int d) {
    UiState& u = U();
    if (u.items.empty()) return;
    const int n = static_cast<int>(u.items.size());
    u.selected = (u.selected + d + n) % n;
}

void Paste() {
    if (!OpenClipboard(nullptr)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            std::string& text = TextBuf();
            for (; *w && text.size() < 64; ++w)
                if (*w > 32 && *w < 127) text.push_back(static_cast<char>(*w));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

void HandleInput(float dt) {
    UiState& u = U();
    ui_input::Event e;
    while (ui_input::PopEvent(&e)) {
        if (u.panel == Panel::None) continue;
        switch (e.type) {
        case ui_input::Event::Key:
            switch (e.code) {
            case VK_UP: Move(-1); break;
            case VK_DOWN:
            case VK_TAB: Move(1); break;
            case VK_LEFT: StepOption(-1); break;
            case VK_RIGHT: StepOption(+1); break;
            case VK_RETURN:
            case VK_SPACE:
                if (TextPanel(u.panel) && e.code == VK_SPACE) break;
                if (TextPanel(u.panel) && e.code == VK_RETURN) {
                    Run(u.items[static_cast<size_t>(u.selected)].action == Action::Back ? Action::Back
                                                                                       : SubmitAction());
                } else {
                    Run(u.items[static_cast<size_t>(u.selected)].action);
                }
                return;
            case VK_ESCAPE: Run(Action::Back); return;
            case VK_BACK:
                if (TextPanel(u.panel)) {
                    if (!TextBuf().empty()) TextBuf().pop_back();
                } else {
                    Run(Action::Back);
                    return;
                }
                break;
            default: break;
            }
            break;
        case ui_input::Event::Char:
            if (TextPanel(u.panel)) {
                if (e.code == 0x16) {
                    Paste();  // Ctrl+V
                } else if (e.code > 32 && e.code < 127 && TextBuf().size() < 64) {
                    TextBuf().push_back(static_cast<char>(e.code));
                }
                u.caret = 0;
            }
            break;
        case ui_input::Event::MouseMove: {
            // Only a real move selects the row under the cursor. Games that keep the cursor centred send a
            // WM_MOUSEMOVE to the same spot every frame; acting on those would pin the selection to whatever row
            // lies under the window centre and undo every keyboard/pad step. The first event after a panel opens
            // only records where the cursor is.
            const bool first = u.mouseX < 0;
            const bool moved = std::fabs(e.x - u.mouseX) > 0.5f || std::fabs(e.y - u.mouseY) > 0.5f;
            u.mouseX = e.x;
            u.mouseY = e.y;
            if (first || !moved) break;
            for (size_t i = 0; i < u.items.size(); ++i) {
                const Item& it = u.items[i];
                if (e.x >= it.x0 && e.x <= it.x1 && e.y >= it.y0 && e.y <= it.y1) u.selected = static_cast<int>(i);
            }
            break;
        }
        case ui_input::Event::MouseDown:
            for (size_t i = 0; i < u.items.size(); ++i) {
                const Item& it = u.items[i];
                if (e.x >= it.x0 && e.x <= it.x1 && e.y >= it.y0 && e.y <= it.y1) {
                    u.selected = static_cast<int>(i);
                    Run(it.action);
                    return;
                }
            }
            break;
        case ui_input::Event::Wheel: Move(e.code == 1 ? -1 : 1); break;
        }
    }
    if (u.panel == Panel::None) return;

    // Gamepad: d-pad / left stick (ui_input reports the stick as d-pad bits) with repeat, A = select, B = back.
    const uint16_t pad = ui_input::PadButtons();
    const uint16_t pressed = static_cast<uint16_t>(pad & ~u.padPrev);
    u.padPrev = pad;
    if (pressed & XINPUT_GAMEPAD_A) {
        Run(TextPanel(u.panel) && u.items[static_cast<size_t>(u.selected)].action != Action::Back
                ? SubmitAction()
                : u.items[static_cast<size_t>(u.selected)].action);
        return;
    }
    if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) {
        Run(Action::Back);
        return;
    }
    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) StepOption(-1);
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) StepOption(+1);
    const bool up = (pad & XINPUT_GAMEPAD_DPAD_UP) != 0, down = (pad & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) {
        Move(-1);
        u.padRepeat = 0.4f;
    } else if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
        Move(1);
        u.padRepeat = 0.4f;
    } else if (up || down) {
        u.padRepeat -= dt;
        if (u.padRepeat <= 0) {
            Move(up ? -1 : 1);
            u.padRepeat = 0.15f;
        }
    }
}

// ---- drawing -------------------------------------------------------------------------------------------------------
std::vector<std::string> Wrap(const std::string& text, float px, float width) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&]() {
        if (word.empty()) return;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && render::DrawList::TextWidth(cand, px) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = cand;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') {
            flush();
        } else {
            word.push_back(c);
        }
    }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

uint32_t Alpha(uint32_t argb, float a) {
    const uint32_t al = static_cast<uint32_t>(static_cast<float>(argb >> 24) * std::clamp(a, 0.0f, 1.0f));
    return (argb & 0x00FFFFFF) | (al << 24);
}

// Key hint like the game's footer ("[Enter] Select"): the key in a light box, the action next to it.
float Hint(render::DrawList& d, float x, float y, const std::string& key, const std::string& action, float px) {
    const float kw = render::DrawList::TextWidth(key, px) + px * 0.5f;
    d.Rect(x, y + px * 0.05f, kw, px * 1.0f, 0xFFE9E5DB);
    d.Text(x + px * 0.25f, y + px * 0.05f, key, px, 0xFF111111);
    const float aw = d.Text(x + kw + px * 0.35f, y + px * 0.05f, action, px, kText);
    return kw + px * 0.35f + aw + px * 1.2f;
}

void DrawToasts(render::DrawList& d, float W, float s) {
    UiState& u = U();
    const float w = std::min(W - 40 * s, 680 * s);
    float y = 70 * s;
    const float titlePx = 24 * s, textPx = 21 * s, pad = 14 * s;
    for (const Toast& t : u.toasts) {
        const float a = std::min({1.0f, t.age / kFade, (t.seconds - t.age) / kFade});
        if (a <= 0) continue;
        const auto lines = Wrap(t.text, textPx, w - pad * 2 - 6 * s);
        const float h = pad * 2 + titlePx * 1.1f + textPx * 1.15f * static_cast<float>(lines.size());
        const float x = (W - w) * 0.5f;
        d.RectH(x, y, w, h, Alpha(kToastBg, a), Alpha(0xC0100E0C, a));
        d.Rect(x, y, 4 * s, h, Alpha(t.error ? kRed : kGold, a));
        d.Rect(x, y + h - 1 * s, w, 1 * s, Alpha(kGoldDim, a * 0.6f));
        d.Text(x + pad + 6 * s, y + pad, Upper(t.title), titlePx, Alpha(t.error ? kRed : kGold, a));
        float ty = y + pad + titlePx * 1.1f;
        for (const auto& l : lines) {
            d.Text(x + pad + 6 * s, ty, l, textPx, Alpha(kText, a));
            ty += textPx * 1.15f;
        }
        y += h + 10 * s;
    }
}

void DrawItems(render::DrawList& d, float x, float& y, float w, float s) {
    UiState& u = U();
    const float px = 30 * s, rowH = 46 * s;
    for (size_t i = 0; i < u.items.size(); ++i) {
        Item& it = u.items[i];
        if (it.player >= 0) continue;  // player rows are drawn in the player list (DrawPausePanel)
        const bool sel = static_cast<int>(i) == u.selected;
        it.x0 = x;
        it.x1 = x + w;
        it.y0 = y;
        it.y1 = y + rowH;
        if (sel) {
            d.RectH(x, y + 3 * s, w, rowH - 6 * s, 0x66B08A45, 0x00B08A45);
            d.Rect(x, y + 3 * s, 2 * s, rowH - 6 * s, kGold);
        }
        d.Text(x + 16 * s, y + (rowH - px) * 0.5f, it.label, px, sel ? 0xFFFFFFFF : kTextDim);
        if (Stepper(it)) {  // "<  VALUE  >" on the right
            const float vpx = 26 * s, ty = y + (rowH - vpx) * 0.5f;
            const std::string v = Upper(it.value);
            const float vw = render::DrawList::TextWidth(v, vpx), aw = render::DrawList::TextWidth(">", vpx);
            const float right = x + w - 12 * s;
            const uint32_t arrow = sel ? kGold : kGoldDim;
            d.Text(right - aw, ty, ">", vpx, arrow);
            d.Text(right - aw - 14 * s - vw, ty, v, vpx, sel ? 0xFFFFFFFF : kText);
            d.Text(right - aw - 28 * s - vw - aw, ty, "<", vpx, arrow);
        }
        y += rowH;
    }
}

// A padlock drawn from rectangles (the overlay font is ASCII only): shackle on top, body below. size = total height.
void DrawLock(render::DrawList& d, float x, float y, float size, uint32_t col) {
    const float w = size * 0.8f, bodyH = size * 0.58f, t = std::max(1.0f, size * 0.11f);
    const float sx = x + w * 0.2f, sw = w * 0.6f, sh = size - bodyH;
    d.Rect(sx, y, sw, t, col);                   // shackle top
    d.Rect(sx, y, t, sh, col);                   // shackle left
    d.Rect(sx + sw - t, y, t, sh, col);          // shackle right
    d.Rect(x, y + sh, w, bodyH, col);            // body
    d.Rect(x + w * 0.5f - t * 0.5f, y + sh + bodyH * 0.3f, t, bodyH * 0.4f, kPanelBg);  // keyhole
}

void DrawPausePanel(render::DrawList& d, float W, float H, float s) {
    UiState& u = U();
    BuildPauseItems();  // follows the session state while open
    const Coop::Info& info = u.info;
    d.Rect(0, 0, W, H, kShade);
    const float w = 620 * s, x = (W - w) * 0.5f;
    float y = H * 0.18f;
    const float top = y;
    // measure: title, status, players, items, hints
    const float rows = static_cast<float>(info.players.size());
    const auto menuItems =
        std::count_if(u.items.begin(), u.items.end(), [](const Item& it) { return it.player < 0; });
    const float h = 70 * s + 40 * s + (rows > 0 ? rows * 36 * s + 30 * s : 0) + static_cast<float>(menuItems) * 46 * s +
                    90 * s;
    d.RectH(x, top, w, h, kPanelBg, 0xD8121110);
    d.Rect(x, top, w, 2 * s, kGoldDim);
    const float pad = 36 * s;
    y += 22 * s;
    const float titleW = d.Text(x + pad, y, "CO-OP", 50 * s, kGold);
    if (info.passwordSet) {  // lock marker: the session needs a password
        const float lx = x + pad + titleW + 22 * s, ly = y + 14 * s;
        DrawLock(d, lx, ly, 26 * s, kGold);
        d.Text(lx + 36 * s, y + 18 * s, "PASSWORD", 21 * s, kTextDim);
    }
    y += 62 * s;
    std::string status;
    switch (info.phase) {
    case Coop::Phase::Idle: status = "NOT CONNECTED"; break;
    case Coop::Phase::Starting: status = "STARTING..."; break;
    case Coop::Phase::Hosting:
        status = "HOSTING";
        if (info.lobbyOpen) status += "  -  STEAM";
        if (info.port) status += "  -  " + (info.localAddress.empty() ? std::string("UDP") : info.localAddress) +
                                 (info.port == GetGameInfo().defaultPort ? std::string() : ":" + std::to_string(info.port));
        break;
    case Coop::Phase::Joining: status = "JOINING " + Upper(info.target) + "..."; break;
    case Coop::Phase::Connected: status = "CONNECTED TO " + Upper(info.target); break;
    }
    d.Text(x + pad, y, status, 22 * s, kTextDim);
    y += 40 * s;
    if (!info.players.empty()) {
        d.Rect(x + pad, y - 8 * s, w - pad * 2, 1 * s, 0x60D1AC62);
        for (const auto& p : info.players) {
            // Hosting: a guest's row is selectable (an item with its player id); hit box and highlight like an item.
            bool sel = false;
            for (size_t i = 0; i < u.items.size(); ++i) {
                Item& it = u.items[i];
                if (it.player != p.id || p.host) continue;
                it.x0 = x + pad - 16 * s;
                it.x1 = x + w - pad;
                it.y0 = y - 5 * s;
                it.y1 = y + 31 * s;
                sel = static_cast<int>(i) == u.selected;
                if (sel) {
                    d.RectH(it.x0, it.y0, it.x1 - it.x0, it.y1 - it.y0, 0x66B08A45, 0x00B08A45);
                    d.Rect(it.x0, it.y0, 2 * s, it.y1 - it.y0, kGold);
                }
            }
            std::string name = p.name + (p.host ? "  (HOST)" : "") + (p.you ? "  (YOU)" : "");
            d.Text(x + pad + 4 * s, y, Upper(name), 25 * s, sel ? 0xFFFFFFFF : p.you ? kText : kTextDim);
            if (p.pingMs >= 0) {
                const std::string ping = std::to_string(p.pingMs) + " MS";
                const uint32_t col = p.pingMs < 80 ? 0xFF8CCB7E : p.pingMs < 160 ? 0xFFE0C060 : kRed;
                d.Text(x + w - pad - render::DrawList::TextWidth(ping, 25 * s), y, ping, 25 * s, col);
            }
            if (p.health >= 0) {  // health column left of the ping
                const std::string hp = "HEALTH " + std::to_string(p.health);
                const uint32_t col = p.health > 60 ? kTextDim : p.health > 25 ? 0xFFE0C060 : kRed;
                d.Text(x + w - pad - 120 * s - render::DrawList::TextWidth(hp, 25 * s), y, hp, 25 * s, col);
            }
            y += 36 * s;
        }
        d.Rect(x + pad, y + 4 * s, w - pad * 2, 1 * s, 0x60D1AC62);
        y += 30 * s;
    }
    DrawItems(d, x + pad - 16 * s, y, w - pad * 2 + 16 * s, s);
    y += 30 * s;
    float hx = x + pad;
    const bool onOption = u.selected >= 0 && u.selected < static_cast<int>(u.items.size()) &&
                          Stepper(u.items[static_cast<size_t>(u.selected)]);
    if (onOption) hx += Hint(d, hx, y, "Left/Right", "Change", 22 * s);
    hx += Hint(d, hx, y, "Enter", onOption ? "Next" : "Select", 22 * s);
    Hint(d, hx, y, "Esc", "Back", 22 * s);
}

void DrawTextBox(render::DrawList& d, float W, float H, float s, float dt, const std::string& title,
                 const std::string& label, const std::string& shown, const std::string& submit) {
    UiState& u = U();
    u.caret += dt;
    d.Rect(0, 0, W, H, kShade);
    const float w = 620 * s, x = (W - w) * 0.5f, pad = 36 * s;
    const float h = 345 * s, top = H * 0.26f;
    d.RectH(x, top, w, h, kPanelBg, 0xD8121110);
    d.Rect(x, top, w, 2 * s, kGoldDim);
    float y = top + 22 * s;
    d.Text(x + pad, y, title, 50 * s, kGold);
    y += 66 * s;
    d.Text(x + pad, y, label, 21 * s, kTextDim);
    y += 32 * s;
    const float fh = 52 * s;
    d.Rect(x + pad, y, w - pad * 2, fh, 0xFF1D1B18);
    d.Rect(x + pad, y + fh - 2 * s, w - pad * 2, 2 * s, kGold);
    const float tw = d.Text(x + pad + 14 * s, y + (fh - 30 * s) * 0.5f, shown, 30 * s, kText);
    if (std::fmod(u.caret, 1.0f) < 0.55f)
        d.Rect(x + pad + 16 * s + tw, y + 10 * s, 2 * s, fh - 20 * s, kText);
    y += fh + 24 * s;
    DrawItems(d, x + pad - 16 * s, y, w - pad * 2 + 16 * s, s);
    y += 20 * s;
    float hx = x + pad;
    hx += Hint(d, hx, y, "Enter", submit, 22 * s);
    Hint(d, hx, y, "Esc", "Cancel", 22 * s);
}

// PlayerMenu / ConfirmKick: title, one line of text, the items and the key hints (the text box panel's frame).
void DrawChoice(render::DrawList& d, float W, float H, float s, const std::string& title, const std::string& text,
                const std::string& yes, const std::string& no) {
    UiState& u = U();
    d.Rect(0, 0, W, H, kShade);
    const float w = 620 * s, x = (W - w) * 0.5f, pad = 36 * s;
    const auto lines = Wrap(text, 25 * s, w - pad * 2);
    const float h = 22 * s + 66 * s + static_cast<float>(lines.size()) * 32 * s + 16 * s +
                    static_cast<float>(u.items.size()) * 46 * s + 20 * s + 50 * s;
    const float top = H * 0.26f;
    d.RectH(x, top, w, h, kPanelBg, 0xD8121110);
    d.Rect(x, top, w, 2 * s, kGoldDim);
    float y = top + 22 * s;
    d.Text(x + pad, y, title, 50 * s, kGold);
    y += 66 * s;
    for (const auto& l : lines) {
        d.Text(x + pad, y, l, 25 * s, kText);
        y += 32 * s;
    }
    y += 16 * s;
    DrawItems(d, x + pad - 16 * s, y, w - pad * 2 + 16 * s, s);
    y += 20 * s;
    float hx = x + pad;
    hx += Hint(d, hx, y, "Enter", yes, 22 * s);
    Hint(d, hx, y, "Esc", no, 22 * s);
}

void DrawPlayerMenu(render::DrawList& d, float W, float H, float s) {
    DrawChoice(d, W, H, s, Upper(U().targetName), "Remove this player from your co-op game?", "Select", "Cancel");
}

void DrawConfirmKick(render::DrawList& d, float W, float H, float s) {
    const UiState& u = U();
    DrawChoice(d, W, H, s, u.targetBan ? "KICK AND BAN" : "KICK",
               std::string(u.targetBan ? "Kick and ban " : "Kick ") + u.targetName + "? Enter = yes, Esc = no" +
                   (u.targetBan ? " (banned until you stop hosting)" : ""),
               "Yes", "No");
}

// The lobby: like the CO-OP panel, with each player's character and ready state.
void DrawLobby(render::DrawList& d, float W, float H, float s) {
    UiState& u = U();
    BuildLobbyItems();
    const Coop::Info& info = u.info;
    d.Rect(0, 0, W, H, kShade);
    const float w = 760 * s, x = (W - w) * 0.5f, pad = 36 * s;
    const float rows = static_cast<float>(info.players.size());
    const auto menuItems =
        std::count_if(u.items.begin(), u.items.end(), [](const Item& it) { return it.player < 0; });
    const float h = 70 * s + 40 * s + 34 * s + (rows > 0 ? rows * 40 * s + 30 * s : 0) +
                    static_cast<float>(menuItems) * 46 * s + 90 * s;
    const float top = std::max(20 * s, (H - h) * 0.4f);
    float y = top;
    d.RectH(x, top, w, h, kPanelBg, 0xD8121110);
    d.Rect(x, top, w, 2 * s, kGoldDim);
    y += 22 * s;
    const float titleW = d.Text(x + pad, y, "CO-OP LOBBY", 50 * s, kGold);
    if (info.passwordSet) DrawLock(d, x + pad + titleW + 22 * s, y + 14 * s, 26 * s, kGold);
    y += 62 * s;
    std::string status;
    switch (info.phase) {
    case Coop::Phase::Idle: status = "NOT CONNECTED"; break;
    case Coop::Phase::Starting: status = "STARTING..."; break;
    case Coop::Phase::Hosting:
        status = "HOSTING";
        if (info.lobbyOpen) status += "  -  STEAM";
        if (info.port) status += "  -  " + (info.localAddress.empty() ? std::string("UDP") : info.localAddress) +
                                 (info.port == GetGameInfo().defaultPort ? std::string() : ":" + std::to_string(info.port));
        break;
    case Coop::Phase::Joining: status = "JOINING " + Upper(info.target) + "..."; break;
    case Coop::Phase::Connected:
        status = info.hostInGame ? Upper(info.target) + " IS PLAYING " + ModeName(info.hostGameMode)
                                 : "WAITING FOR " + Upper(info.target) + " TO START";
        break;
    }
    d.Text(x + pad, y, status, 22 * s, kTextDim);
    y += 40 * s;
    // column heads
    const float colChar = x + w * 0.42f, colState = x + w - pad;
    d.Text(x + pad + 4 * s, y, "PLAYER", 19 * s, kGoldDim);
    d.Text(colChar, y, "CHARACTER", 19 * s, kGoldDim);
    d.Text(colState - render::DrawList::TextWidth("STATUS", 19 * s), y, "STATUS", 19 * s, kGoldDim);
    y += 34 * s;
    if (!info.players.empty()) {
        d.Rect(x + pad, y - 8 * s, w - pad * 2, 1 * s, 0x60D1AC62);
        for (const auto& p : info.players) {
            bool sel = false;
            for (size_t i = 0; i < u.items.size(); ++i) {
                Item& it = u.items[i];
                if (it.player != p.id || p.host) continue;
                it.x0 = x + pad - 16 * s;
                it.x1 = x + w - pad;
                it.y0 = y - 5 * s;
                it.y1 = y + 35 * s;
                sel = static_cast<int>(i) == u.selected;
                if (sel) {
                    d.RectH(it.x0, it.y0, it.x1 - it.x0, it.y1 - it.y0, 0x66B08A45, 0x00B08A45);
                    d.Rect(it.x0, it.y0, 2 * s, it.y1 - it.y0, kGold);
                }
            }
            std::string name = p.name + (p.you ? "  (YOU)" : "");
            d.Text(x + pad + 4 * s, y, Upper(name), 25 * s, sel ? 0xFFFFFFFF : p.you ? kText : kTextDim);
            if (!p.character.empty()) d.Text(colChar, y + 2 * s, Upper(p.character), 22 * s, kTextDim);
            std::string st;
            uint32_t col = kTextDim;
            if (p.host) {
                st = info.hostInGame ? "HOST - PLAYING" : "HOST";
                col = kGold;
            } else if (p.ready) {
                st = "READY";
                col = 0xFF8CCB7E;
            } else {
                st = "NOT READY";
                col = kRed;
            }
            d.Text(colState - render::DrawList::TextWidth(st, 23 * s), y + 1 * s, st, 23 * s, col);
            y += 40 * s;
        }
        d.Rect(x + pad, y + 4 * s, w - pad * 2, 1 * s, 0x60D1AC62);
        y += 30 * s;
    }
    DrawItems(d, x + pad - 16 * s, y, w - pad * 2 + 16 * s, s);
    y += 30 * s;
    float hx = x + pad;
    const bool onStep = u.selected >= 0 && u.selected < static_cast<int>(u.items.size()) &&
                        Stepper(u.items[static_cast<size_t>(u.selected)]);
    if (onStep) hx += Hint(d, hx, y, "Left/Right", "Change", 22 * s);
    hx += Hint(d, hx, y, "Enter", "Select", 22 * s);
    Hint(d, hx, y, "Esc", "Back", 22 * s);
}

void DrawSettings(render::DrawList& d, float W, float H, float s) {
    UiState& u = U();
    BuildSettingsItems();
    d.Rect(0, 0, W, H, kShade);
    const float w = 680 * s, x = (W - w) * 0.5f, pad = 36 * s;
    const float h = 70 * s + 40 * s + static_cast<float>(u.items.size()) * 46 * s + 90 * s;
    const float top = std::max(20 * s, (H - h) * 0.35f);
    d.RectH(x, top, w, h, kPanelBg, 0xD8121110);
    d.Rect(x, top, w, 2 * s, kGoldDim);
    float y = top + 22 * s;
    d.Text(x + pad, y, "CO-OP SETTINGS", 50 * s, kGold);
    y += 62 * s;
    d.Text(x + pad, y, "SAVED IN " + Upper(std::string(GetGameInfo().modName)) + ".INI", 21 * s, kTextDim);
    y += 40 * s;
    DrawItems(d, x + pad - 16 * s, y, w - pad * 2 + 16 * s, s);
    y += 30 * s;
    float hx = x + pad;
    const bool onStep = u.selected >= 0 && u.selected < static_cast<int>(u.items.size()) &&
                        Stepper(u.items[static_cast<size_t>(u.selected)]) &&
                        u.items[static_cast<size_t>(u.selected)].action != Action::EditName;
    if (onStep) hx += Hint(d, hx, y, "Left/Right", "Change", 22 * s);
    hx += Hint(d, hx, y, "Enter", onStep ? "Change" : "Select", 22 * s);
    Hint(d, hx, y, "Esc", "Back", 22 * s);
}

void DrawName(render::DrawList& d, float W, float H, float s, float dt) {
    DrawTextBox(d, W, H, s, dt, "PLAYER NAME", "EMPTY = YOUR STEAM NAME", U().nameText, "Save");
}

void DrawJoinIp(render::DrawList& d, float W, float H, float s, float dt) {
    DrawTextBox(d, W, H, s, dt, "JOIN BY IP", "HOST ADDRESS (IP OR IP:PORT)", U().ipText, "Connect");
}

void DrawPassword(render::DrawList& d, float W, float H, float s, float dt) {
    const UiState& u = U();
    DrawTextBox(d, W, H, s, dt, u.pwForHost ? "SET PASSWORD" : "PASSWORD",
                u.pwForHost ? "PLAYERS JOINING BY IP MUST TYPE IT" : "THIS GAME NEEDS A PASSWORD",
                std::string(u.pwText.size(), '*'), u.pwForHost ? "Set" : "Connect");
}

}  // namespace

void CoopUi::Init() {}

bool CoopUi::PanelOpen() { return U().panel != Panel::None; }

bool CoopUi::OpenForTest(const std::string& which) {
    if (which == "pause") Open(Panel::PauseCoop);
    else if (which == "lobby") Open(Panel::Lobby);
    else if (which == "settings") Open(Panel::Settings);
    else if (which == "joinip") Open(Panel::JoinIp);
    else return false;
    return true;
}

std::string CoopUi::Describe() {
    static const char* const kNames[] = {"none",        "pause CO-OP", "join by IP", "password", "player menu",
                                         "confirm kick", "lobby",       "settings",   "name"};
    const UiState& u = U();
    const auto pi = static_cast<size_t>(u.panel);
    std::string out = std::string("panel ") + (pi < std::size(kNames) ? kNames[pi] : "?") + ", selected " +
                      std::to_string(u.selected);
    char buf[256];
    for (size_t i = 0; i < u.items.size(); ++i) {
        const Item& it = u.items[i];
        std::snprintf(buf, sizeof buf, "\n  [%zu]%s %s%s%s  (%.0f,%.0f)-(%.0f,%.0f)", i,
                      static_cast<int>(i) == u.selected ? "*" : " ", it.label.c_str(), it.value.empty() ? "" : " = ",
                      it.value.c_str(), it.x0, it.y0, it.x1, it.y1);
        out += buf;
    }
    return out;
}

void CoopUi::Tick(float dt) {
    UiState& u = U();
    if (!u.compatChecked) {
        u.compatChecked = true;
        ShowCompatBanner();
    }
    u.info = Coop::GetInfo();
    // The session ended (left, kicked, the host left): a lobby of that session has nothing left to show.
    const bool inSession = u.info.phase != Coop::Phase::Idle;
    if (u.wasInSession && !inSession && u.panel == Panel::Lobby) Close();
    u.wasInSession = inSession;
    if (u.info.starts != u.starts) {  // the host started a game: everybody's lobby closes, the game loads
        u.starts = u.info.starts;
        if (u.panel == Panel::Lobby) Close();
    }

    // native menu entries (the game module patches them into the game's menus): publish state, run their commands
    GameModule& game = GetGameModule();
    MenuState ms;
    switch (u.info.phase) {
    case Coop::Phase::Idle: ms.mode = MenuMode::Idle; break;
    case Coop::Phase::Starting:
    case Coop::Phase::Hosting: ms.mode = MenuMode::Hosting; break;
    case Coop::Phase::Joining: ms.mode = MenuMode::Joining; break;
    case Coop::Phase::Connected: ms.mode = MenuMode::Connected; break;
    }
    ms.players = static_cast<int>(u.info.players.size());
    ms.password = u.info.passwordSet;
    game.SetMenuState(ms);
    MenuCommand cmd;
    while (game.PopMenuCommand(&cmd)) {
        switch (cmd) {
        case MenuCommand::Host: Coop::Host(); break;
        case MenuCommand::Leave: Coop::Leave(); break;
        case MenuCommand::JoinFriend: Coop::OpenSteamFriends(); break;
        case MenuCommand::JoinByIp: Open(Panel::JoinIp); break;
        case MenuCommand::Invite: Coop::InviteFriends(); break;
        case MenuCommand::PausePanel: Open(Panel::PauseCoop); break;
        case MenuCommand::SetPassword: Open(Panel::Password, true); break;
        case MenuCommand::ClearPassword: Coop::SetPassword(""); break;
        case MenuCommand::Lobby: Open(Panel::Lobby); break;
        case MenuCommand::Settings: Open(Panel::Settings); break;
        }
    }
    if (Coop::PopPasswordPrompt()) Open(Panel::Password, false);  // a join was refused for its password
    if (Coop::PopOpenLobby() && u.panel == Panel::None) Open(Panel::Lobby);  // joined at the main menu

    Coop::Notice n;
    while (Coop::PopNotice(&n)) AddToast(n.title, n.text, n.error);
    for (Toast& t : u.toasts) t.age += dt;
    u.toasts.erase(std::remove_if(u.toasts.begin(), u.toasts.end(), [](const Toast& t) { return t.age >= t.seconds; }),
                   u.toasts.end());

    HandleInput(dt);

    // geometry
    u.list.Clear();
    unsigned bw = 0, bh = 0;
    render::Overlay::BackbufferSize(&bw, &bh);
    if (!bw || !bh) return;
    const float W = static_cast<float>(bw), H = static_cast<float>(bh), s = H / 1080.0f;
    game.DrawHud(u.list, W, H);  // the game's HUD geometry (name tags) under our panels and banners
    if (u.panel == Panel::PauseCoop) DrawPausePanel(u.list, W, H, s);
    if (u.panel == Panel::Lobby) DrawLobby(u.list, W, H, s);
    if (u.panel == Panel::Settings) DrawSettings(u.list, W, H, s);
    if (u.panel == Panel::Name) DrawName(u.list, W, H, s, dt);
    if (u.panel == Panel::JoinIp) DrawJoinIp(u.list, W, H, s, dt);
    if (u.panel == Panel::Password) DrawPassword(u.list, W, H, s, dt);
    if (u.panel == Panel::PlayerMenu) DrawPlayerMenu(u.list, W, H, s);
    if (u.panel == Panel::ConfirmKick) DrawConfirmKick(u.list, W, H, s);
    DrawToasts(u.list, W, s);
}

const render::DrawList* CoopUi::Draw() {
    const UiState& u = U();
    return u.list.Empty() ? nullptr : &u.list;
}

} // namespace mc
