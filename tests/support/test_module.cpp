// A game module that does nothing, for out\mc_test.dll: proves core's dll layer links on its own (every symbol the
// game must provide is in mc/game_module.h) and gives proxy_test a mod dll next to the test launcher.
#include "mc/game_module.h"

namespace mc {
namespace {

class TestModule final : public GameModule {
public:
    void Init(const std::wstring&, const std::string&) override {}
    void RenderTick(float) override {}
    void OnSessionStart(net::Session&) override {}
    void OnSessionEnd() override {}
    void OnPlayerLeft(net::PlayerId) override {}
    void SessionTick(float) override {}
};

}  // namespace

GameModule& GetGameModule() {
    static TestModule module;
    return module;
}

}  // namespace mc
