#pragma once
// Glyph atlas for the overlay, rasterised once on the CPU with GDI from a Windows font that resembles the game's
// menu font (Bahnschrift, a DIN-style face shipped with Windows 10+; fallbacks Segoe UI / Arial). If GDI fails the
// built-in 8x16 bitmap font (render/font8x16.h) is used. The atlas is 8-bit coverage; it also holds a solid
// white block used to draw rectangles with the same texture.
#include <cstdint>
#include <string>
#include <vector>

namespace mc::render::font {

struct Glyph {
    float u0, v0, u1, v1;  // atlas texture coordinates
    float w, h;            // quad size in atlas pixels
    float xOff, yOff;      // quad offset from the pen position (y from the line top), atlas pixels
    float advance;         // pen advance, atlas pixels
};

struct Atlas {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // width * height coverage
    float lineHeight = 0;         // atlas pixels (ascent + descent)
    float ascent = 0;
    float solidU = 0, solidV = 0;  // a texel inside the solid block
    Glyph glyphs[96];              // ASCII 32..127 (127 = fallback '?')
    std::string face;              // what was used
    bool valid = false;
};

// Built on first call (thread-safe enough: called from the render thread only).
const Atlas& Get();

// Glyph for a Unicode code point (ASCII; a few typographic characters map to ASCII; others to '?').
const Glyph& For(uint32_t codepoint);

// Decodes UTF-8 into code points.
void Decode(const std::string& s, std::vector<uint32_t>& out);

}  // namespace mc::render::font
