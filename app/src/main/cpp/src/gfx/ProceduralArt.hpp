#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"

namespace th06 {

// Generated art. Contains no game assets: it is the UI (touch controls, a 5x7 bitmap
// font) plus stand-ins used only when no game data is mounted, so the APK is testable
// on its own.

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

struct FallbackGameArt {
    std::vector<SpriteRegion> bullets;  // 16x16 round bullets, 8 colours
    SpriteRegion player;                // 32x48
    SpriteRegion shot;                  // 8x24
    SpriteRegion spawner;               // 48x48 magic-circle ring
};

UiArt registerUiArt(TextureLibrary& lib);
FallbackGameArt registerFallbackGameArt(TextureLibrary& lib);

// Text in the built-in font. 6 px advance per character at scale 1; lower case renders
// as upper case; unknown characters render as blanks.
void drawText(SpriteSink& sink, const UiArt& ui, float x, float y, std::string_view text,
              float scale, std::uint32_t color);
float textWidth(std::string_view text, float scale);

// Validates the glyph table (used by unit tests).
bool fontSelfTest(std::string* error);

}  // namespace th06
