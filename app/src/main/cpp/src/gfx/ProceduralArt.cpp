#include "gfx/ProceduralArt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace th06 {
namespace {

// ---- 5x7 font ------------------------------------------------------------------------

struct Glyph {
    char ch;
    const char* rows[7];
};

constexpr Glyph kGlyphs[] = {
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
    {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
    {'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}},
    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####"}},
    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
    {'N', {"#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # "}},
    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
    {':', {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}},
    {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
    {',', {"     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   "}},
    {'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
    {'+', {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}},
    {'/', {"     ", "    #", "   # ", "  #  ", " #   ", "#    ", "     "}},
    {'(', {"   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "}},
    {')', {" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "}},
    {'%', {"##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"}},
    {'!', {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "}},
    {'?', {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}},
    {'>', {" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "}},
    {'<', {"   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "}},
    {'=', {"     ", "     ", "#####", "     ", "#####", "     ", "     "}},
    {'"', {" # # ", " # # ", " # # ", "     ", "     ", "     ", "     "}},
};

// Font cells are 8x10 with the 5x7 glyph top-left; a glyph is sampled as a 6x8 region,
// so linear filtering at its edges only ever touches empty texels, never a neighbour.
// The font also starts 8 rows below the 64 px shapes for the same reason.
constexpr int kFontX = 0, kFontY = 72, kCellW = 8, kCellH = 10, kFontCols = 16;
constexpr int kGlyphW = 6, kGlyphH = 8;
constexpr int kAtlasW = 256, kAtlasH = 136;

char canonical(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    return c;
}

// ---- tiny anti-aliased rasteriser ----------------------------------------------------

struct Rgba {
    float r, g, b, a;
};

// Shades a w x h block at (ox, oy) with 4x4 supersampling. The shader receives
// coordinates in block pixels (0..w, 0..h) and returns colour with coverage in .a.
template <class Shader>
void paint(Image& img, int ox, int oy, int w, int h, Shader&& shader) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const Rgba c = shader(x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f);
                    r += c.r * c.a;
                    g += c.g * c.a;
                    b += c.b * c.a;
                    a += c.a;
                }
            }
            std::uint8_t* p = img.pixel(ox + x, oy + y);
            if (a <= 0.0f) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            p[0] = static_cast<std::uint8_t>(std::clamp(r / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = static_cast<std::uint8_t>(std::clamp(g / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = static_cast<std::uint8_t>(std::clamp(b / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[3] = static_cast<std::uint8_t>(std::clamp(a / 16.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

float dist(float x, float y, float cx, float cy) { return std::hypot(x - cx, y - cy); }

float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

Image buildUiAtlas() {
    Image img(kAtlasW, kAtlasH);
    const Rgba white{1, 1, 1, 1}, none{1, 1, 1, 0};

    paint(img, 0, 0, 64, 64, [&](float x, float y) { return dist(x, y, 32, 32) <= 30.0f ? white : none; });
    paint(img, 64, 0, 64, 64, [&](float x, float y) {
        const float d = dist(x, y, 32, 32);
        return (d <= 30.0f && d >= 25.0f) ? white : none;
    });
    paint(img, 128, 0, 64, 64, [&](float x, float y) {
        const float t = std::clamp(1.0f - dist(x, y, 32, 32) / 31.0f, 0.0f, 1.0f);
        return Rgba{1, 1, 1, t * t};
    });
    paint(img, 192, 0, 8, 8, [&](float, float) { return white; });
    paint(img, 224, 0, 32, 32, [&](float x, float y) {
        const bool in = edge(16, 4, 28, 26, x, y) >= 0 && edge(28, 26, 4, 26, x, y) >= 0 &&
                        edge(4, 26, 16, 4, x, y) >= 0;
        return in ? white : none;
    });

    // Font: ASCII 32..127 in 8x8 cells, 16 per row.
    for (const Glyph& g : kGlyphs) {
        const int index = static_cast<unsigned char>(g.ch) - 32;
        const int cx = kFontX + (index % kFontCols) * kCellW;
        const int cy = kFontY + (index / kFontCols) * kCellH;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g.rows[row][col] != '#') continue;
                std::uint8_t* p = img.pixel(cx + col, cy + row);
                p[0] = p[1] = p[2] = p[3] = 255;
            }
        }
    }
    // Every UI shape is white: make transparent texels white too, so linear filtering
    // at the edges blends toward white instead of producing dark fringes.
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] == 0) img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = 255;
    }
    return img;
}

}  // namespace

SpriteRegion UiArt::glyph(char c) const {
    c = canonical(c);
    int index = static_cast<unsigned char>(c) - 32;
    if (index < 0 || index >= 96) index = 0;
    const float x = static_cast<float>(kFontX + (index % kFontCols) * kCellW);
    const float y = static_cast<float>(kFontY + (index / kFontCols) * kCellH);
    return SpriteRegion{texture, x / atlasWidth, y / atlasHeight, (x + kGlyphW) / atlasWidth,
                        (y + kGlyphH) / atlasHeight, kGlyphW, kGlyphH};
}

UiArt registerUiArt(TextureLibrary& lib) {
    UiArt ui;
    ui.texture = lib.add("ui-atlas (generated)", kAtlasW, kAtlasH,
                         [] { return std::optional<Image>(buildUiAtlas()); }, TextureFilter::Linear);
    ui.atlasWidth = kAtlasW;
    ui.atlasHeight = kAtlasH;
    ui.disc = lib.region(ui.texture, 0, 0, 64, 64);
    ui.ring = lib.region(ui.texture, 64, 0, 64, 64);
    ui.glow = lib.region(ui.texture, 128, 0, 64, 64);
    ui.white = lib.region(ui.texture, 192, 0, 8, 8);
    ui.chevron = lib.region(ui.texture, 224, 0, 32, 32);
    return ui;
}

void drawText(SpriteSink& sink, const UiArt& ui, float x, float y, std::string_view text,
              float scale, std::uint32_t color) {
    const float advance = 6.0f * scale;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ' ') continue;
        const SpriteRegion g = ui.glyph(text[i]);
        SpriteInstance s;
        s.w = kGlyphW * scale;
        s.h = kGlyphH * scale;
        s.x = x + static_cast<float>(i) * advance + s.w * 0.5f;
        s.y = y + s.h * 0.5f;
        s.u0 = g.u0;
        s.v0 = g.v0;
        s.u1 = g.u1;
        s.v1 = g.v1;
        s.color = color;
        sink.draw(ui.texture, Blend::Alpha, s);
    }
}

float textWidth(std::string_view text, float scale) {
    return text.empty() ? 0.0f : (static_cast<float>(text.size()) * 6.0f - 1.0f) * scale;
}

bool fontSelfTest(std::string* error) {
    for (const Glyph& g : kGlyphs) {
        for (const char* row : g.rows) {
            if (std::strlen(row) != 5) {
                if (error) *error = std::string("glyph '") + g.ch + "' has a row that is not 5 wide";
                return false;
            }
        }
        for (const Glyph& other : kGlyphs) {
            if (&other != &g && other.ch == g.ch) {
                if (error) *error = std::string("duplicate glyph '") + g.ch + "'";
                return false;
            }
        }
    }
    return true;
}

}  // namespace th06
