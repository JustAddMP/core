#include "render/ui_font.h"

#include "core/log.h"
#include "render/font8x16.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

namespace mc::render::font {
namespace {

constexpr int kPx = 48;   // em height the glyphs are rasterised at (scaled down with mipmaps when drawn)
constexpr int kPad = 4;   // transparent border around every cell (keeps mip levels from bleeding)
constexpr int kCols = 16;
constexpr int kSolidSize = 16;

Atlas _atlas;
bool _built = false;

int NextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

bool BuildGdi(Atlas& a) {
    // Bahnschrift's named instances show up in GDI as their own families; GetTextFace tells us what we really got.
    const wchar_t* faces[] = {L"Bahnschrift SemiBold SemiConden", L"Bahnschrift SemiCondensed", L"Bahnschrift",
                              L"Segoe UI Semibold", L"Arial"};
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    wchar_t got[LF_FACESIZE] = {};
    for (const wchar_t* face : faces) {
        HFONT f = CreateFontW(-kPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        if (!f) continue;
        HGDIOBJ old = SelectObject(dc, f);
        GetTextFaceW(dc, LF_FACESIZE, got);
        if (_wcsnicmp(got, face, wcslen(got) < wcslen(face) ? wcslen(got) : wcslen(face)) == 0 && got[0]) {
            font = f;
            oldFont = old;
            break;
        }
        SelectObject(dc, old);
        DeleteObject(f);
    }
    if (!font) {
        DeleteDC(dc);
        return false;
    }
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    ABC abc[96] = {};
    const bool haveAbc = GetCharABCWidthsW(dc, 32, 126, abc) != FALSE;
    int maxBox = 0;
    for (int i = 0; i < 95; ++i) {
        if (!haveAbc) {
            SIZE sz{};
            wchar_t ch = static_cast<wchar_t>(32 + i);
            GetTextExtentPoint32W(dc, &ch, 1, &sz);
            abc[i].abcA = 0;
            abc[i].abcB = static_cast<UINT>(sz.cx);
            abc[i].abcC = 0;
        }
        maxBox = std::max(maxBox, static_cast<int>(abc[i].abcB) + std::abs(abc[i].abcA) + std::abs(abc[i].abcC));
    }
    abc[95] = abc['?' - 32];
    const int cellW = maxBox + 2 * kPad, cellH = tm.tmHeight + 2 * kPad;
    const int rows = (96 + kCols - 1) / kCols;
    a.width = NextPow2(kCols * cellW);
    a.height = NextPow2(rows * cellH + kSolidSize + 2 * kPad);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = a.width;
    bi.bmiHeader.biHeight = -a.height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        SelectObject(dc, oldFont);  // a font still selected into a DC cannot be deleted
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(a.width) * a.height * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < 96; ++i) {
        const int cx = (i % kCols) * cellW, cy = (i / kCols) * cellH;
        const wchar_t ch = i == 95 ? L'?' : static_cast<wchar_t>(32 + i);
        // Put the glyph's black box at cx + kPad.
        TextOutW(dc, cx + kPad - abc[i].abcA, cy + kPad, &ch, 1);
        Glyph& g = a.glyphs[i];
        g.u0 = static_cast<float>(cx) / a.width;
        g.v0 = static_cast<float>(cy) / a.height;
        g.u1 = static_cast<float>(cx + cellW) / a.width;
        g.v1 = static_cast<float>(cy + cellH) / a.height;
        g.w = static_cast<float>(cellW);
        g.h = static_cast<float>(cellH);
        g.xOff = static_cast<float>(abc[i].abcA - kPad);
        g.yOff = static_cast<float>(-kPad);
        g.advance = static_cast<float>(abc[i].abcA + static_cast<int>(abc[i].abcB) + abc[i].abcC);
    }
    GdiFlush();
    a.pixels.assign(static_cast<size_t>(a.width) * a.height, 0);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (size_t p = 0; p < a.pixels.size(); ++p) a.pixels[p] = src[p * 4 + 1];  // green channel = coverage
    const int sy = rows * cellH + kPad;
    for (int y = 0; y < kSolidSize; ++y)
        for (int x = 0; x < kSolidSize; ++x) a.pixels[static_cast<size_t>(sy + y) * a.width + kPad + x] = 255;
    a.solidU = (kPad + kSolidSize * 0.5f) / a.width;
    a.solidV = (sy + kSolidSize * 0.5f) / a.height;
    a.lineHeight = static_cast<float>(tm.tmHeight);
    a.ascent = static_cast<float>(tm.tmAscent);
    char face[LF_FACESIZE * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, got, -1, face, sizeof(face), nullptr, nullptr);
    a.face = face;

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    a.valid = true;
    return true;
}

void BuildBitmap(Atlas& a) {
    a.width = 256;
    a.height = 128;
    a.pixels.assign(static_cast<size_t>(a.width) * a.height, 0);
    for (int i = 0; i < kCount; ++i) {
        const int ox = (i % kCols) * (kCellW + 2) + 1, oy = (i / kCols) * (kCellH + 2) + 1;
        for (int y = 0; y < kCellH; ++y)
            for (int x = 0; x < kCellW; ++x)
                a.pixels[static_cast<size_t>(oy + y) * a.width + ox + x] = kPixels[(i * kCellH + y) * kCellW + x];
        Glyph& g = a.glyphs[i];
        g.u0 = static_cast<float>(ox) / a.width;
        g.v0 = static_cast<float>(oy) / a.height;
        g.u1 = static_cast<float>(ox + kCellW) / a.width;
        g.v1 = static_cast<float>(oy + kCellH) / a.height;
        g.w = kCellW;
        g.h = kCellH;
        g.xOff = 0;
        g.yOff = 0;
        g.advance = kCellW;
    }
    a.glyphs[95] = a.glyphs['?' - 32];
    const int sx = (kSolid % kCols) * (kCellW + 2) + 1, sy = (kSolid / kCols) * (kCellH + 2) + 1;
    a.solidU = (sx + kCellW * 0.5f) / a.width;
    a.solidV = (sy + kCellH * 0.5f) / a.height;
    a.lineHeight = kCellH;
    a.ascent = 12;
    a.face = "built-in 8x16";
    a.valid = true;
}

}  // namespace

const Atlas& Get() {
    if (!_built) {
        _built = true;
        if (!BuildGdi(_atlas)) {
            MC_LOG_WARN("overlay: GDI font unavailable; using the built-in bitmap font");
            BuildBitmap(_atlas);
        }
        MC_LOG_INFO("overlay: font '%s', atlas %dx%d, line height %.0f px", _atlas.face.c_str(), _atlas.width,
                    _atlas.height, _atlas.lineHeight);
    }
    return _atlas;
}

const Glyph& For(uint32_t cp) {
    const Atlas& a = Get();
    switch (cp) {
    case 0x2013: case 0x2014: cp = '-'; break;               // en/em dash
    case 0x2018: case 0x2019: cp = '\''; break;
    case 0x201C: case 0x201D: cp = '"'; break;
    case 0x00B7: case 0x2022: cp = '-'; break;               // middle dot, bullet
    case 0x00A0: cp = ' '; break;
    default: break;
    }
    if (cp < 32 || cp > 126) return a.glyphs[95];
    return a.glyphs[cp - 32];
}

void Decode(const std::string& s, std::vector<uint32_t>& out) {
    out.clear();
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else if (c >= 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xC0) {
            len = 2;
            cp = c & 0x1F;
        }
        for (size_t k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
}

}  // namespace mc::render::font
