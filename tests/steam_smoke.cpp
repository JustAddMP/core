// steam_smoke: verifies the flat-API bindings against a real steam_api64.dll.
// Usage: steam_smoke.exe <path\to\steam_api64.dll>
// Uses a COPY of the game's dll (never run against the game folder). Sets SteamAppId to GameInfo::steamAppId (the
// linked descriptor: core's tests use 480, Spacewar), initialises Steam itself (the in-game dll never does), prints
// identity, creates a friends-only lobby, leaves, shuts down. Needs Steam running; not part of the automatic tests.
#include <windows.h>

#include <cstdio>
#include <string>

#include "mc/game_info.h"
#include "net/steam_flat.h"
#include "net/steam_lobby.h"
#include "net/steam_transport.h"

using namespace mc::net;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("%s  %-38s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_fail;
}
void PrintLog(int level, const char* msg) { std::printf("    [net %d] %s\n", level, msg); }
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: steam_smoke <path to steam_api64.dll>\n");
        return 2;
    }
    SetLogSink(PrintLog);
    const std::wstring appId = std::to_wstring(mc::GetGameInfo().steamAppId);
    SetEnvironmentVariableW(L"SteamAppId", appId.c_str());
    SetEnvironmentVariableW(L"SteamGameId", appId.c_str());

    HMODULE mod = LoadLibraryW(argv[1]);
    if (!mod) {
        std::printf("FAIL  LoadLibrary(%ls): error %lu\n", argv[1], GetLastError());
        return 1;
    }
    Check(steam::Load(mod), "resolve flat-API exports");
    if (g_fail) return 1;
    steam::Api& api = steam::Get();

    bool running = api.IsSteamRunning();
    std::printf("      SteamAPI_IsSteamRunning: %s\n", running ? "yes" : "no");
    if (!api.Init()) {
        std::printf("FAIL  SteamAPI_Init returned false (Steam running: %s). Not working around it.\n",
                    running ? "yes" : "no");
        return 1;
    }
    Check(true, "SteamAPI_Init");
    Check(steam::AcquireInterfaces(), "acquire interfaces",
          "User020 Friends017 MatchMaking009 Utils009 Networking006");
    if (g_fail) {
        api.Shutdown();
        return 1;
    }
    uint64_t me = api.User_GetSteamID(api.user);
    const char* persona = api.Friends_GetPersonaName(api.friends);
    std::printf("      persona: %s\n      SteamID: %llu\n      AppID:   %u\n", persona ? persona : "(null)",
                static_cast<unsigned long long>(me), api.Utils_GetAppID(api.utils));
    Check(me != 0 && (me >> 56) == 1, "SteamID is an individual account");

    // Informational: is the newer ISteamNetworkingMessages reachable through the client at all?
    void* msgs = api.FindOrCreateUserInterface(api.GetHSteamUser(), "SteamNetworkingMessages002");
    std::printf("      info: SteamNetworkingMessages002 via FindOrCreateUserInterface: %s (not used)\n",
                msgs ? "available" : "not available");

    {
        SteamTransport transport;
        bool ok = transport.Init() && transport.Host(0);
        Check(ok, "SteamTransport init + host",
              "local " + std::to_string(transport.LocalId()));
        transport.Close();
    }

    {
        SteamLobby lobby;
        Check(lobby.Init("smoke-test", 1), "SteamLobby init (callbacks registered)");
        std::string failure;
        uint64_t created = 0;
        lobby.onCreated = [&](uint64_t id) { created = id; };
        lobby.onFailed = [&](const std::string& r) { failure = r; };
        bool started = lobby.Create(4);
        DWORD t0 = GetTickCount();
        while (started && !created && failure.empty() && GetTickCount() - t0 < 20000) {
            api.RunCallbacks();  // in game the game does this on its own thread
            lobby.Poll();
            Sleep(20);
        }
        char d[200];
        std::snprintf(d, sizeof(d), "lobby %llu in %lu ms%s%s", static_cast<unsigned long long>(created),
                      GetTickCount() - t0, failure.empty() ? "" : ", error: ", failure.c_str());
        Check(created != 0, "create friends-only lobby", d);
        if (created) {
            std::string host = lobby.GetData("mc_host");
            std::string ver = lobby.GetData("mc_version");
            Check(host == std::to_string(me) && ver == "smoke-test", "lobby data round-trip",
                  "mc_host=" + host + " mc_version=" + ver);
            Check(lobby.IsOwner() && lobby.IsMember(me) && lobby.Members().size() == 1, "lobby owner/members",
                  std::to_string(lobby.Members().size()) + " member(s)");
            std::printf("      rich presence connect = \"+connect_lobby %llu\"\n", static_cast<unsigned long long>(created));
            uint64_t parsed = SteamLobby::ParseConnectLobby(("+connect_lobby " + std::to_string(created)).c_str());
            Check(parsed == created, "+connect_lobby parse");
            lobby.Leave();
            for (int i = 0; i < 10; ++i) {
                api.RunCallbacks();
                lobby.Poll();
                Sleep(20);
            }
            Check(lobby.GetState() == SteamLobby::State::Idle, "leave lobby");
        }
    }

    steam::UnregisterAll();
    api.Shutdown();
    std::printf("      SteamAPI_Shutdown done\n\n%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
