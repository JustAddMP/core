// Game descriptor for core's own tests and test binaries (out\mc_test_launcher.exe, out\mc_test_proxy.dll,
// out\mc_test.dll): a made-up game, so nothing here can touch a real game install.
#include "mc/game_info.h"

namespace mc {
namespace {

constexpr GameBuild kBuilds[] = {
    {0x12345678u, "test build"},
};

constexpr GameInfo kInfo = {
    .modName = "McTest",
    .modVersion = "0.0.1",
    .launcherName = L"mc_test_launcher.exe",
    .dllName = L"mc_test.dll",
    .iniName = L"McTest.ini",
    .logPrefix = L"mctest",
    .envPrefix = "MCTEST_",
    .defaultPort = 27960,
    .steamStatus = "core test",
    .gameName = "Core Test Game",
    .gameExe = L"mc_test_game.exe",
    .steamAppId = 480,  // Spacewar, Valve's test app
    .steamInstallDir = L"Spacewar",
    .gameArgs = L"",
    .builds = kBuilds,
    .buildCount = sizeof(kBuilds) / sizeof(kBuilds[0]),
    .proxyBuiltName = L"mc_test_proxy.dll",
    .proxyPointerName = L"mc_test_path.txt",
    .proxyMarker = "McTest-proxy-dll:v1 (core test proxy)",
};

}  // namespace

const GameInfo& GetGameInfo() { return kInfo; }

}  // namespace mc
