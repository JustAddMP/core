#pragma once
// Side-by-side test instances: `--window left|right` (ini `window = left|right`) moves the game window onto that
// half of the work area of the monitor it is on (full height, title bar kept). Unset = the window is left alone.
// The game sets its own window size at startup and again later (seen while loading and when the window is activated),
// so the placement is checked twice a second for the whole run and re-applied when the window has been off target
// for a second. A test option: it keeps the window there even if it is dragged away.
//
// Test tool for a strip of a wide screen (the one-PC test, tools/onepc in the game repos): `--window-area x0,x1[,y]`
// (ini `window_area = x0,x1[,y]`) puts every top-level window this game creates at x0 (and y) the moment it is made
// (CreateWindowExW / A hooked from EarlyInit, before the game's first window: its 480x481 start window used to come
// up in the middle of the primary monitor first), wider ones made x1 - x0 wide. Later moves are the game's (and
// window = left|right's); a keeper outside the game watches those. ParseArea is pure (tests/window_area.cpp).
namespace mc::window_place {

struct Area {
    int x0 = 0, x1 = 0, y = 0;
    bool haveY = false;
    bool On() const { return x1 > x0; }
};
// "2916,4233" or "2916,4233,633" -> area; false (area off) on anything else ("" is off and true).
bool ParseArea(const char* text, Area& out);
// Where a window made at x,y with width w (CW_USEDEFAULT allowed) goes in `a` (unchanged when a is off).
void FitCreate(const Area& a, int& x, int& y, int& w);

void EarlyInit();       // the init thread, right after core::hook::Init: reads window_area, hooks window creation
void Init(void* hwnd);  // reads the config; once, after the game window exists
void Tick(float dt);    // render thread, every frame
void OnResize();        // after IDXGISwapChain::ResizeBuffers
void* Window();         // the game window given to Init (nullptr before it); for the session summary

}  // namespace mc::window_place
