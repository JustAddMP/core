#pragma once
// Minimal D3D11 overlay drawn into the game's backbuffer from the Present hook (before the real Present): the
// co-op status panel (top-left) plus whatever the UI layer queued in a DrawList (toasts, the pause-menu co-op panel,
// the join-by-IP box). Own glyph atlas (render/ui_font.h) and shaders (render/overlay.hlsl, compiled by build.cmd);
// no third-party UI. The game's pipeline state is saved before drawing and restored afterwards.
#include <cstdint>
#include <string>
#include <vector>

struct IDXGISwapChain;

namespace mc::render {

struct OverlayLine {
    std::string text;  // UTF-8
    uint32_t color;    // 0xAARRGGBB
};

// Screen-space quads in backbuffer pixels (origin top-left). Filled on the render thread each frame.
class DrawList {
public:
    void Clear() { _verts.clear(); }
    bool Empty() const { return _verts.empty(); }

    void Rect(float x, float y, float w, float h, uint32_t argb);
    void RectH(float x, float y, float w, float h, uint32_t argbLeft, uint32_t argbRight);  // horizontal gradient
    // Draws UTF-8 text with its line top at y and a line height of px pixels; returns the advance width.
    float Text(float x, float y, const std::string& s, float px, uint32_t argb);
    static float TextWidth(const std::string& s, float px);

    struct Vertex {
        float x, y, u, v;
        uint32_t color;  // R8G8B8A8 byte order
    };
    const std::vector<Vertex>& Vertices() const { return _verts; }

private:
    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c0,
              uint32_t c1);
    std::vector<Vertex> _verts;
};

class Overlay {
public:
    // Draws the status lines (top-left panel; may be empty) and then `ui` (may be null). Creates its resources on
    // first use (and again if the game's device changes). Disables itself (logged once) if anything fails.
    static void Draw(IDXGISwapChain* swapChain, const std::vector<OverlayLine>& status, const DrawList* ui);
    // Drops our backbuffer reference and its view. Draw already does this after every frame (no reference survives
    // into the game's Present / ResizeBuffers / swapchain release); kept for ResizeBuffers as a safety net.
    static void ReleaseTargets();
    // Releases everything (shaders, buffers, font texture, states): device lost, or the process is exiting. The next
    // Draw creates them again if needed.
    static void ReleaseAll();
    // Reads the backbuffer size from the swapchain (cheap; call once per frame before laying out UI).
    static void Track(IDXGISwapChain* swapChain);
    // Last tracked backbuffer size (0 before the first Track).
    static void BackbufferSize(unsigned* width, unsigned* height);
};

} // namespace mc::render
