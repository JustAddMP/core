#include "dll/coop.h"

#include "core/compat.h"
#include "core/config.h"
#include "core/log.h"
#include "core/version.h"
#include "dll/ui_input.h"
#include "game/player_sync.h"
#include "net/multi_transport.h"
#include "net/session.h"
#include "net/steam_flat.h"
#include "net/steam_lobby.h"
#include "net/steam_transport.h"
#include "net/udp_transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>

namespace mc {
namespace {

constexpr double kSteamInitDelayS = 3.0;  // after the first frame: the game's own Steam init is long done by then
constexpr double kErrorShowS = 20.0;      // how long a disconnect reason stays on screen

constexpr uint32_t kColTitle = 0xFFFFFFFF;
constexpr uint32_t kColText = 0xFFC8C8C8;
constexpr uint32_t kColGood = 0xFF8CE08C;
constexpr uint32_t kColWarn = 0xFFFFC864;
constexpr uint32_t kColError = 0xFFFF7878;

enum class Mode { None, Host, JoinUdp, JoinLobby };
enum class SteamState { Pending, Ready, Failed, Disabled };

const char* ModeName(Mode m) {
    switch (m) {
    case Mode::None: return "idle";
    case Mode::Host: return "host";
    case Mode::JoinUdp: return "join (udp)";
    case Mode::JoinLobby: return "join (steam lobby)";
    }
    return "?";
}

// Hello carries this; both sides must match exactly. Derived from kModVersion ("1.2.3" -> 10203), so players
// can connect whenever they run the same release. Bump kModVersion when the wire format or sync changes.
uint32_t BuildNumber() {
    uint32_t parts[3] = {0, 0, 0};
    int i = 0;
    for (const char* s = kModVersion; *s && i < 3; ++s) {
        if (*s == '.') ++i;
        else if (*s >= '0' && *s <= '9') parts[i] = parts[i] * 10 + static_cast<uint32_t>(*s - '0');
    }
    return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

struct State {
    double now = 0;

    // configuration
    Mode mode = Mode::None;
    bool started = false;
    std::string address;    // JoinUdp
    uint64_t lobbyId = 0;   // JoinLobby
    std::string transport;  // "", "udp", "steam"
    std::string name;       // from config; empty = Steam persona / Windows user
    uint16_t port = net::kDefaultPort;

    // Steam (our own steam_api64.dll instance, see coop.h)
    SteamState steam = SteamState::Pending;
    uint64_t steamId = 0;
    std::string persona;
    net::SteamLobby lobby;

    // session
    std::unique_ptr<net::Transport> net;
    std::unique_ptr<net::Session> session;
    std::string target;     // what we are connecting to, for the status line
    bool teardown = false;  // set from session callbacks; done at the end of Tick
    std::string lastError;
    double lastErrorAt = -1e9;

    std::deque<Coop::Notice> notices;
    std::string localAddress;  // computed once
    ULONGLONG localAddressTried = 0;  // GetTickCount64 of the last failed lookup (retried every 30 s, not per frame)
    std::string lastAddress;   // last JoinAddress target

    // session password (never logged)
    std::string password;            // host: required from guests ("" = none)
    std::string joinPassword;        // guest: sent (as an HMAC answer) when a host asks
    bool requireInvitePassword = false;  // Steam lobby members need it too
    bool passwordPrompt = false;     // a join was refused for the password; the UI asks (PopPasswordPrompt)
    Mode retryMode = Mode::None;     // the join that was refused (JoinUdp: address, JoinLobby: lobbyId)
};

State& S() {
    static State* s = new State;  // never destroyed: no network teardown during static destruction
    return *s;
}

std::string Lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::string Narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void NetLog(int level, const char* msg) {
    switch (level) {
    case 0: MC_LOG_DEBUG("net: %s", msg); break;
    case 1: MC_LOG_INFO("net: %s", msg); break;
    case 2: MC_LOG_WARN("net: %s", msg); break;
    default: MC_LOG_ERROR("net: %s", msg); break;
    }
}

std::wstring DirOf(HMODULE mod) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

HMODULE OurModule() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&S), &mod);
    return mod;
}

std::string PlayerName() {
    State& s = S();
    if (!s.name.empty()) return s.name;
    if (s.steam == SteamState::Ready && !s.persona.empty()) return s.persona;
    wchar_t user[257];
    DWORD n = 257;
    if (GetUserNameW(user, &n)) return Narrow(user);
    return "Player";
}

void SetError(const std::string& why) {
    State& s = S();
    s.lastError = why;
    s.lastErrorAt = s.now;
}

void Notify(std::string title, std::string text, bool error = false) {
    State& s = S();
    if (s.notices.size() > 16) s.notices.pop_front();
    s.notices.push_back({std::move(title), std::move(text), error});
}

// This PC's first non-loopback IPv4 address (for the "join by IP" hint), "" if none.
std::string LocalAddress() {
    State& s = S();
    if (!s.localAddress.empty()) return s.localAddress;
    // Called every frame (Coop::GetInfo): on a PC without a usable IPv4 address, getaddrinfo (which can wait on
    // DNS/LLMNR) must not run on the render thread each frame.
    const ULONGLONG nowMs = GetTickCount64();
    if (s.localAddressTried && nowMs - s.localAddressTried < 30000) return {};
    s.localAddressTried = nowMs;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return {};
    char host[256] = {};
    if (gethostname(host, sizeof(host)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* a = res; a; a = a->ai_next) {
                char buf[64] = {};
                const auto* in = reinterpret_cast<const sockaddr_in*>(a->ai_addr);
                inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
                if (std::strncmp(buf, "127.", 4) != 0 && std::strncmp(buf, "169.254.", 8) != 0) {
                    s.localAddress = buf;
                    break;
                }
            }
            freeaddrinfo(res);
        }
    }
    WSACleanup();
    return s.localAddress;
}

// Drops the session and transports and goes back to idle. Never call from inside a session callback (set
// s.teardown instead; Tick does it after Poll returns).
void Teardown() {
    State& s = S();
    s.teardown = false;
    game::PlayerSync::Detach();
    if (s.session) s.session->Leave();
    s.session.reset();
    s.net.reset();
    s.target.clear();
    if (s.steam == SteamState::Ready && s.lobby.GetState() != net::SteamLobby::State::Idle) s.lobby.Leave();
    if (s.mode != Mode::None) MC_LOG_INFO("coop: back to idle");
    s.mode = Mode::None;
}

void Fail(const std::string& why) {
    MC_LOG_ERROR("coop: %s", why.c_str());
    SetError(why);
    Notify("CO-OP", why, true);
    Teardown();
}

void LogPlayers() {
    State& s = S();
    if (!s.session) return;
    for (const auto& p : s.session->Players()) {
        MC_LOG_INFO("coop:   player %u '%s'%s%s, model '%s'", p.id, p.name.c_str(),
                    p.id == net::kHostPlayer ? " (host)" : "", p.id == s.session->LocalId() ? " (you)" : "",
                    p.model.c_str());
    }
}

void MakeSession(std::unique_ptr<net::Transport> transport) {
    State& s = S();
    net::SessionConfig cfg;
    cfg.modBuild = BuildNumber();
    cfg.playerName = PlayerName();
    cfg.playerModel = core::Config::Get().GetString("model");  // character we appear as (alias or spawn profile)
    cfg.password = s.mode == Mode::Host ? s.password : s.joinPassword;
    cfg.invitesSkipPassword = !s.requireInvitePassword;
    s.net = std::move(transport);
    s.session = std::make_unique<net::Session>(*s.net, cfg);
    net::Session& session = *s.session;
    game::PlayerSync::Attach(session);
    MC_LOG_INFO("coop: session as '%s' over %s (protocol %u, build %08x)", session.Config().playerName.c_str(),
                s.net->Name(), cfg.protocolVersion, cfg.modBuild);

    session.onJoined = [] {
        State& st = S();
        MC_LOG_INFO("coop: handshake complete, joined as player %u (%zu players)", st.session->LocalId(),
                    st.session->Players().size());
        LogPlayers();
        const net::PlayerInfo* host = st.session->FindPlayer(net::kHostPlayer);
        Notify("CO-OP", "Connected to " + (host ? host->name : st.target) + "'s game");
    };
    session.onPlayerJoined = [](const net::PlayerInfo& p) {
        MC_LOG_INFO("coop: player %u '%s' joined (%zu players)", p.id, p.name.c_str(), S().session->Players().size());
        if (p.id != S().session->LocalId()) Notify("CO-OP", p.name + " joined the game");
    };
    session.onPlayerLeft = [](const net::PlayerInfo& p, const std::string& reason) {
        MC_LOG_INFO("coop: player %u '%s' left: %s", p.id, p.name.c_str(), reason.c_str());
        Notify("CO-OP", p.name + " left the game");
        game::PlayerSync::OnPlayerLeft(p.id);
    };
    session.onChat = [](net::PlayerId from, const std::string& text) {
        MC_LOG_INFO("coop: chat from %u: %s", from, text.c_str());
    };
    session.onDisconnected = [](const std::string& reason) {
        State& st = S();
        MC_LOG_WARN("coop: disconnected: %s", reason.c_str());
        SetError("disconnected: " + reason);
        const net::RejectReason rr = st.session ? st.session->LastReject() : net::RejectReason::None;
        if (rr == net::RejectReason::PasswordRequired || rr == net::RejectReason::WrongPassword) {
            Notify("CO-OP", rr == net::RejectReason::WrongPassword ? "Wrong password. Try again."
                                                                   : "This game needs a password.",
                   true);
            st.passwordPrompt = true;
            st.retryMode = st.mode;
        } else if (rr == net::RejectReason::TooManyAttempts) {
            Notify("CO-OP", "Too many wrong passwords. Try again in a minute.", true);
        } else {
            Notify("CO-OP", "Disconnected: " + reason, true);
        }
        st.teardown = true;
    };
}

void JoinSteamHost(uint64_t host) {
    State& s = S();
    if (host == 0) return Fail("the lobby has no host");
    if (host == s.steamId) return Fail("the lobby host is this Steam account (same PC?); use --join 127.0.0.1");
    auto st = std::make_unique<net::SteamTransport>();
    if (!st->Init()) return Fail("Steam transport could not start");
    MakeSession(std::move(st));
    s.target = s.lobby.PersonaName(host);
    if (s.target.empty()) s.target = std::to_string(host);
    if (!s.session->Join(std::to_string(host))) return Fail("could not connect: " + s.session->LastError());
}

// The game build lacks something co-op needs (core/compat.h): refuse with a banner instead of failing silently.
bool Blocked() {
    if (!core::compat::CoopDisabled()) return false;
    S().started = true;
    Fail("Co-op disabled: unsupported game version");
    return true;
}

void StartLobbyJoin(uint64_t lobbyId) {
    State& s = S();
    if (Blocked()) return;
    s.mode = Mode::JoinLobby;
    s.lobbyId = lobbyId;
    s.started = true;
    s.target = "Steam lobby " + std::to_string(lobbyId);
    if (!s.lobby.Join(lobbyId)) Fail("could not join Steam lobby " + std::to_string(lobbyId));
}

// Invite accepted in the overlay / "Join Game" in the friends list while the game runs.
void OnJoinRequested(uint64_t lobbyId) {
    State& s = S();
    MC_LOG_INFO("coop: join requested from Steam for lobby %llu", static_cast<unsigned long long>(lobbyId));
    Notify("CO-OP", "Joining your friend's game...");
    if (s.mode == Mode::JoinLobby && s.lobbyId == lobbyId &&
        (s.session || s.lobby.GetState() != net::SteamLobby::State::Idle)) {
        MC_LOG_INFO("coop: already in or joining that lobby");
        return;
    }
    if (s.session) {
        MC_LOG_INFO("coop: leaving the current session to follow the invite");
        game::PlayerSync::Detach();
        s.session->Leave();
        s.session.reset();
        s.net.reset();
    }
    StartLobbyJoin(lobbyId);  // SteamLobby::Join leaves a lobby we host
}

// The game memory-loads its own old steam_api (docs/re-notes.md 3.1), so there is no steam_api64.dll module and
// no flat API in the process. We load the newer on-disk dll ourselves; its SteamAPI_Init opens our own pipe to
// the same steamclient64.dll, and we pump its callbacks (PollSteam).
void InitSteam() {
    State& s = S();
    std::wstring path = DirOf(nullptr) + L"\\steam_api64.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) path = DirOf(OurModule()) + L"\\steam_api64.dll";
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) {
        MC_LOG_WARN("coop: Steam: LoadLibrary(%s) failed (%lu); Steam features off", Narrow(path.c_str()).c_str(),
                    GetLastError());
        s.steam = SteamState::Failed;
        return;
    }
    MC_LOG_INFO("coop: Steam: loaded %s", Narrow(path.c_str()).c_str());
    if (!net::steam::Load(mod)) {
        s.steam = SteamState::Failed;
        return;
    }
    auto& api = net::steam::Get();
    if (!api.IsSteamRunning()) MC_LOG_WARN("coop: Steam: SteamAPI_IsSteamRunning() = false");
    if (!api.Init()) {
        MC_LOG_WARN("coop: Steam: SteamAPI_Init failed; Steam features off (UDP still works)");
        s.steam = SteamState::Failed;
        return;
    }
    if (!net::steam::AcquireInterfaces()) {
        s.steam = SteamState::Failed;
        return;
    }
    s.steamId = api.User_GetSteamID(api.user);
    const char* persona = api.Friends_GetPersonaName(api.friends);
    s.persona = persona ? persona : "";
    MC_LOG_INFO("coop: Steam ready: SteamID %llu, persona '%s', app %u", static_cast<unsigned long long>(s.steamId),
                s.persona.c_str(), api.Utils_GetAppID(api.utils));

    std::string version = kModVersion;
    if (!s.lobby.Init(version, net::kProtocolVersion)) {
        s.steam = SteamState::Failed;
        return;
    }
    s.lobby.onCreated = [](uint64_t id) {
        MC_LOG_INFO("coop: Steam lobby %llu open; friends can join from the Steam overlay (Shift+Tab)",
                    static_cast<unsigned long long>(id));
    };
    s.lobby.onJoined = [](uint64_t, uint64_t host) {
        if (S().mode == Mode::JoinLobby && !S().session) JoinSteamHost(host);
    };
    s.lobby.onFailed = [](const std::string& why) {
        if (S().mode == Mode::JoinLobby && !S().session)
            Fail(why);
        else
            MC_LOG_WARN("coop: Steam lobby: %s", why.c_str());
    };
    s.lobby.onJoinRequested = &OnJoinRequested;
    s.lobby.onMemberChanged = [](uint64_t id, bool joined) {
        MC_LOG_INFO("coop: lobby member %llu '%s' %s", static_cast<unsigned long long>(id),
                    S().lobby.PersonaName(id).c_str(), joined ? "joined" : "left");
    };
    s.steam = SteamState::Ready;
}

void PollSteam() {
    State& s = S();
    if (s.steam == SteamState::Pending && s.now >= kSteamInitDelayS) InitSteam();
    if (s.steam != SteamState::Ready) return;
    net::steam::Get().RunCallbacks();  // our steam_api instance: nobody else pumps it
    s.lobby.Poll();
}

void TryStart() {
    State& s = S();
    const bool steamPending = s.steam == SteamState::Pending;
    if (s.mode != Mode::None && Blocked()) return;
    switch (s.mode) {
    case Mode::None:
        break;
    case Mode::Host: {
        const bool wantSteam = s.transport != "udp";
        if (steamPending && (wantSteam || s.name.empty())) return;  // wait for Steam (persona name / lobby)
        s.started = true;
        if (s.transport == "steam" && s.steam != SteamState::Ready) {
            return Fail("--transport steam, but Steam is not available");
        }
        auto multi = std::make_unique<net::MultiTransport>();
        net::MultiTransport* multiRaw = multi.get();
        net::Transport* steamRaw = nullptr;
        if (s.transport != "steam") multi->Add(std::make_unique<net::UdpTransport>());
        const bool steamPart = wantSteam && s.steam == SteamState::Ready;
        if (steamPart) {
            auto st = std::make_unique<net::SteamTransport>();
            st->SetAcceptFilter([](net::PeerId id) { return S().lobby.IsMember(id); });
            steamRaw = st.get();
            multi->Add(std::move(st));
        }
        MakeSession(std::move(multi));
        // Invited friends (members of our Steam lobby, over Steam P2P) skip the password unless
        // require_password_for_invites = 1. The transports live as long as the session (Teardown drops it first).
        s.session->isInvitedPeer = [multiRaw, steamRaw](net::PeerId id) {
            return steamRaw && multiRaw->Owner(id) == steamRaw && S().lobby.IsMember(id);
        };
        if (!s.session->Host(s.port)) return Fail("could not host: " + s.session->LastError());
        MC_LOG_INFO("coop: hosting over %s, UDP port %u", s.net->Name(), s.port);
        if (steamPart) s.lobby.Create(net::kMaxPlayers);
        {
            std::string how;
            if (steamPart) how = "Invite friends from the pause menu (CO-OP) or the Steam overlay.";
            if (s.transport != "steam") {
                const std::string ip = LocalAddress();
                if (!how.empty()) how += " ";
                how += "Join by IP: " + (ip.empty() ? std::string("this PC") : ip) +
                       (s.port == net::kDefaultPort ? std::string() : ":" + std::to_string(s.port));
            }
            if (!s.password.empty()) how += " Password protected.";
            Notify("HOSTING CO-OP", how);
        }
        break;
    }
    case Mode::JoinUdp:
        if (steamPending && s.name.empty()) return;
        s.started = true;
        Notify("CO-OP", "Connecting to " + s.address + "...");
        MakeSession(std::make_unique<net::UdpTransport>());
        s.target = s.address;
        if (!s.session->Join(s.address)) return Fail("could not join '" + s.address + "': " + s.session->LastError());
        break;
    case Mode::JoinLobby:
        if (steamPending) return;
        s.started = true;
        if (s.steam != SteamState::Ready) return Fail("joining a Steam lobby needs Steam, which is not available");
        StartLobbyJoin(s.lobbyId);
        break;
    }
    s.started = true;
}

std::string Ms(float ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d ms", static_cast<int>(ms + 0.5f));
    return buf;
}

} // namespace

void Coop::Init() {
    State& s = S();
    const auto& cfg = core::Config::Get();
    net::SetLogSink(&NetLog);

    s.name = cfg.GetString("name");
    s.transport = Lower(cfg.GetString("transport"));
    if (!s.transport.empty() && s.transport != "udp" && s.transport != "steam") {
        MC_LOG_WARN("coop: unknown transport '%s' (udp|steam); using both", s.transport.c_str());
        s.transport.clear();
    }
    const int port = cfg.GetInt("port", net::kDefaultPort);
    if (port < 0 || port > 65535) MC_LOG_WARN("coop: port %d out of range; using %u", port, net::kDefaultPort);
    s.port = port < 0 || port > 65535 ? net::kDefaultPort : static_cast<uint16_t>(port);
    if (!cfg.GetBool("steam", true)) s.steam = SteamState::Disabled;
    s.password = cfg.GetString("password");  // host: required; guest: used to join
    s.joinPassword = s.password;
    s.requireInvitePassword = cfg.GetBool("require_password_for_invites", false);
    if (!s.password.empty())
        MC_LOG_INFO("coop: session password set from config%s",
                    s.requireInvitePassword ? " (also for Steam invites)" : " (Steam invites skip it)");

    uint64_t lobby = strtoull(cfg.GetString("join_lobby").c_str(), nullptr, 10);
    if (!lobby) lobby = net::SteamLobby::CommandLineLobby();
    std::string join = cfg.GetString("join");
    if (!lobby && s.transport == "steam" && !join.empty()) lobby = strtoull(join.c_str(), nullptr, 10);

    if (lobby) {
        s.mode = Mode::JoinLobby;
        s.lobbyId = lobby;
    } else if (!join.empty() && join != "1") {
        s.mode = Mode::JoinUdp;
        s.address = join;
    } else if (cfg.GetBool("host")) {
        s.mode = Mode::Host;
    }
    MC_LOG_INFO("coop: mode %s%s%s, transport %s, name '%s'", ModeName(s.mode), s.address.empty() ? "" : " ",
                s.address.c_str(), s.transport.empty() ? "auto" : s.transport.c_str(),
                s.name.empty() ? "(Steam/Windows)" : s.name.c_str());
    if (s.mode == Mode::JoinLobby) MC_LOG_INFO("coop: lobby %llu", static_cast<unsigned long long>(s.lobbyId));
    if (s.steam == SteamState::Disabled) MC_LOG_INFO("coop: Steam disabled by config (steam = 0)");
}

namespace {
// The enter-vehicle control (keyboard E, pad Y) held in our focused window while no panel of ours takes input.
bool EnterVehicleHeld() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId())) return false;
    if (ui_input::Blocking()) return false;
    constexpr uint16_t kPadY = 0x8000;  // XINPUT_GAMEPAD_Y
    return (GetAsyncKeyState('E') & 0x8000) != 0 || (ui_input::PadButtons() & kPadY) != 0;
}

// The aim control (right mouse button, pad left trigger) held in our focused window while no panel takes input.
// The game itself does not tell scripts whether the player aims (docs/game-api.md 10), so this is read from the input.
bool AimHeld() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId())) return false;
    if (ui_input::Blocking()) return false;
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 || ui_input::PadLeftTrigger() > 60;
}
}  // namespace

void Coop::Tick(float dt) {
    State& s = S();
    s.now += dt;
    PollSteam();
    if (!s.started) TryStart();
    if (s.session) s.session->Poll();
    if (s.session) game::PlayerSync::Tick(dt, EnterVehicleHeld(), AimHeld());
    if (s.teardown) Teardown();
}

void Coop::Shutdown() {
    State& s = S();
    if (s.session) s.session->Leave();
    if (s.steam == SteamState::Ready) s.lobby.Leave();
}

// ---- runtime control -------------------------------------------------------------------------------------------------
namespace {
void StopCurrent() {
    State& s = S();
    if (s.session || s.mode != Mode::None ||
        (s.steam == SteamState::Ready && s.lobby.GetState() != net::SteamLobby::State::Idle))
        Teardown();
    s.lastError.clear();
}
}  // namespace

void Coop::Host() {
    State& s = S();
    MC_LOG_INFO("coop: host requested from the menu");
    StopCurrent();
    s.mode = Mode::Host;
    s.started = false;  // TryStart (waits for Steam if it is still starting)
}

void Coop::JoinAddress(const std::string& address) {
    State& s = S();
    MC_LOG_INFO("coop: join '%s' requested from the menu", address.c_str());
    StopCurrent();
    s.mode = Mode::JoinUdp;
    s.address = address;
    s.lastAddress = address;
    s.started = false;
}

void Coop::JoinLobby(uint64_t lobbyId) {
    State& s = S();
    StopCurrent();
    s.mode = Mode::JoinLobby;
    s.lobbyId = lobbyId;
    s.started = false;
}

void Coop::Leave() {
    State& s = S();
    const bool active = s.session || s.mode != Mode::None;
    MC_LOG_INFO("coop: leave requested from the menu");
    StopCurrent();
    s.started = true;  // stay idle
    if (active) Notify("CO-OP", "You left the co-op session");
}

bool Coop::InviteFriends() {
    State& s = S();
    if (s.steam != SteamState::Ready) {
        Notify("CO-OP", "Steam is not available: friends can join by IP instead", true);
        return false;
    }
    if (s.lobby.OpenInviteDialog()) return true;
    Notify("CO-OP", "Host a co-op game first, then invite friends", true);
    return false;
}

bool Coop::OpenSteamFriends() {
    State& s = S();
    if (s.steam != SteamState::Ready) {
        Notify("CO-OP", "Steam is not available: use JOIN BY IP", true);
        return false;
    }
    auto& api = net::steam::Get();
    api.Friends_ActivateGameOverlay(api.friends, "Friends");
    Notify("JOIN FRIEND", "Right-click a friend who is hosting and choose Join Game, or accept their invite.");
    return true;
}

void Coop::SetPassword(const std::string& password) {
    State& s = S();
    s.password = password;
    if (s.session && s.session->IsHost()) s.session->SetPassword(password);
    MC_LOG_INFO("coop: host password %s", password.empty() ? "cleared" : "set");
    if (password.empty())
        Notify("CO-OP", "Password cleared: anyone with your address can join");
    else
        Notify("CO-OP", s.requireInvitePassword ? "Password set: everyone joining needs it"
                                                : "Password set: players joining by IP need it (Steam invites skip it)");
}

bool Coop::PopPasswordPrompt() {
    State& s = S();
    if (!s.passwordPrompt) return false;
    s.passwordPrompt = false;
    return true;
}

void Coop::RetryWithPassword(const std::string& password) {
    State& s = S();
    s.joinPassword = password;
    MC_LOG_INFO("coop: retrying the join with a password");
    if (s.retryMode == Mode::JoinLobby && s.lobbyId) {
        JoinLobby(s.lobbyId);
    } else if (!s.address.empty()) {
        JoinAddress(s.address);
    } else {
        Notify("CO-OP", "Nothing to retry: join a game first", true);
    }
}

Coop::Info Coop::GetInfo() {
    State& s = S();
    Info info;
    info.passwordSet = !s.password.empty();
    info.steamReady = s.steam == SteamState::Ready;
    info.lastAddress = s.lastAddress;
    info.localAddress = LocalAddress();
    info.target = s.target;
    if (s.session) {
        const net::Session& session = *s.session;
        switch (session.State()) {
        case net::SessionState::Hosting: info.phase = Phase::Hosting; break;
        case net::SessionState::Connected: info.phase = Phase::Connected; break;
        default: info.phase = Phase::Joining; break;
        }
        if (info.phase == Phase::Hosting) {
            info.port = s.transport == "steam" ? 0 : s.port;
            info.lobbyOpen = s.lobby.GetState() == net::SteamLobby::State::InLobby;
        }
        if (const net::PlayerInfo* host = session.FindPlayer(net::kHostPlayer); host && info.phase == Phase::Connected)
            info.target = host->name;
        for (const auto& p : session.Players()) {
            PlayerRow row;
            row.name = p.name;
            row.host = p.id == net::kHostPlayer;
            row.you = p.id == session.LocalId();
            row.pingMs = row.host ? -1 : static_cast<int>(p.rttMs + 0.5f);
            row.health = game::PlayerSync::Health(p.id);
            info.players.push_back(row);
        }
    } else if (s.mode == Mode::JoinLobby) {
        info.phase = Phase::Joining;
    } else if (s.mode != Mode::None) {
        info.phase = s.mode == Mode::Host ? Phase::Starting : Phase::Joining;
    }
    return info;
}

bool Coop::PopNotice(Notice* out) {
    State& s = S();
    if (s.notices.empty()) return false;
    *out = std::move(s.notices.front());
    s.notices.pop_front();
    return true;
}

void Coop::GetStatus(std::vector<render::OverlayLine>& out) {
    State& s = S();
    if (s.session) {
        const net::Session& session = *s.session;
        const auto& players = session.Players();
        switch (session.State()) {
        case net::SessionState::Hosting: {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Co-op: hosting (%zu player%s)", players.size(),
                          players.size() == 1 ? "" : "s");
            out.push_back({buf, kColTitle});
            std::string via;
            if (s.transport != "steam") via = "UDP port " + std::to_string(s.port);
            if (s.lobby.GetState() == net::SteamLobby::State::InLobby)
                via += std::string(via.empty() ? "" : ", ") + "Steam lobby (Shift+Tab to invite)";
            if (session.HasPassword()) via += std::string(via.empty() ? "" : ", ") + "password";
            else if (s.lobby.GetState() == net::SteamLobby::State::Creating)
                via += std::string(via.empty() ? "" : ", ") + "creating Steam lobby...";
            if (!via.empty()) out.push_back({"  " + via, kColText});
            break;
        }
        case net::SessionState::Connecting:
            out.push_back({"Co-op: connecting to " + s.target + "...", kColWarn});
            break;
        case net::SessionState::Connected: {
            const net::PlayerInfo* host = session.FindPlayer(net::kHostPlayer);
            out.push_back({"Co-op: connected to " + (host ? host->name : s.target) + " \xE2\x80\x94 " +
                               Ms(host ? host->rttMs : 0.0f),
                           kColTitle});
            break;
        }
        default:
            break;
        }
        if (session.IsActive()) {
            game::PlayerSync::GetStatus(out);
            // Ping column: RTT of each player to the host (the host's own row has none).
            for (const auto& p : players) {
                std::string line = "  " + p.name;
                if (p.id == net::kHostPlayer) line += " (host)";
                if (p.id == session.LocalId()) line += " (you)";
                if (p.id != net::kHostPlayer) line += "  " + Ms(p.rttMs);
                out.push_back({line, p.id == session.LocalId() ? kColGood : kColText});
            }
        }
    } else if (s.mode == Mode::JoinLobby && s.lobby.GetState() == net::SteamLobby::State::Joining) {
        out.push_back({"Co-op: joining " + s.target + "...", kColWarn});
    } else if (s.mode != Mode::None && !s.started) {
        out.push_back({"Co-op: waiting for Steam...", kColWarn});
    }
    if (!s.lastError.empty() && s.now - s.lastErrorAt < kErrorShowS) {
        out.push_back({"Co-op: " + s.lastError, kColError});
    }
}

} // namespace mc
