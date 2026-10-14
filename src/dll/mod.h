#pragma once
// The mod's per-frame entry point. Everything game-facing runs inside Tick, on the game's render thread.

namespace mc {

class Mod {
public:
    static void Init();          // called once from the init thread, before the Present hook goes live
    static void Tick(float dt);  // called once per frame from the Present hook
    static void OnResize(unsigned width, unsigned height);
    static void OnBeforeResize();  // drop backbuffer references (overlay)
};

} // namespace mc
