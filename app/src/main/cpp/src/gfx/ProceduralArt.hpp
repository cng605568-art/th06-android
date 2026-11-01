#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"

namespace th06 {

// Generated UI art (touch controls and a 5x7 bitmap font). Contains no game assets; all
// game graphics come from the player's archives through the ANM engine.

struct UiArt {
    std::uint16_t texture = 0;
    SpriteRegion disc;     // filled circle, 64x64
    SpriteRegion ring;     // circle outline, 64x64
    SpriteRegion glow;     // soft radial falloff, 64x64
    SpriteRegion white;    // solid white texels
    SpriteRegion chevron;  // triangle pointing up, 32x32
    float atlasWidth = 0;
    float atlasHeight = 0;

    SpriteRegion glyph(char c) const;  // 6x8 region; the 5x7 glyph sits in its top-left
};

UiArt registerUiArt(TextureLibrary& lib);

// Text in the built-in font. 6 px advance per character at scale 1; lower case renders
// as upper case; unknown characters render as blanks.
void drawText(SpriteSink& sink, const UiArt& ui, float x, float y, std::string_view text,
              float scale, std::uint32_t color);
float textWidth(std::string_view text, float scale);

// Writes ASCII text in the built-in font straight into an image (x, y = top-left of the
// first glyph; `scale` whole-pixel magnification). Characters outside the font are skipped.
void rasterizeText(Image& img, int x, int y, std::string_view text, int scale, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b);

// Validates the glyph table (used by unit tests).
bool fontSelfTest(std::string* error);

}  // namespace th06
