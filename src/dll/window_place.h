#pragma once
// Side-by-side test instances: `--window left|right` (ini `window = left|right`) moves the game window onto that
// half of the work area of the monitor it is on (full height, title bar kept). Unset = the window is left alone.
// The game sets its own window size at startup and again later (seen while loading and when the window is activated),
// so the placement is checked twice a second for the whole run and re-applied when the window has been off target
// for a second. A test option: it keeps the window there even if it is dragged away.
namespace mc::window_place {

void Init(void* hwnd);  // reads the config; once, after the game window exists
void Tick(float dt);    // render thread, every frame
void OnResize();        // after IDXGISwapChain::ResizeBuffers

}  // namespace mc::window_place
