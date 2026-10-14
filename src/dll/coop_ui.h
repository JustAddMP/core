#pragma once
// The player-facing co-op UI that is not part of the game's own menus (render thread):
//   - executes the commands from the native menu entries (game/menu.h) and keeps their state up to date;
//   - the pause menu's CO-OP panel (players + ping, host / invite / join / leave / password) and the JOIN BY IP and
//     PASSWORD text boxes (the password box opens when a join is refused for its password, or to set the host's),
//     drawn by our overlay in the game's menu style, with keyboard, mouse and gamepad; while one is open the game
//     gets no input (dll/ui_input.h);
//   - notifications ("Guest joined the game", "Connected to ...", errors) as banners at the top of the screen.
#include "render/overlay.h"

namespace mc {

class CoopUi {
public:
    static void Init();
    static void Tick(float dt);
    // Geometry for this frame; nullptr when there is nothing to draw.
    static const render::DrawList* Draw();
    static bool PanelOpen();
};

} // namespace mc
