#pragma once

// Text the game renders into textures at run time: stage and song names, bomb and spell
// card names, dialogue. The strings are Shift-JIS bytes from the player's game files (or
// the engine's own bomb names). The original draws them with MS Gothic bold at double
// resolution and filters them down into a 16-texel band; a platform renderer reproduces
// that look with the fonts it has (Android: the system CJK font through the activity).

#include <cstdint>
#include <span>
#include <string_view>

#include "gfx/Image.hpp"

namespace th06 {

struct TextStyle {
    int fontSize = 15;              // the original's font size F (glyph cell 2F at double resolution)
    std::uint32_t rgb = 0xffffff;   // as it should appear on screen (0xRRGGBB)
    std::uint32_t shadow = 0x000000;
};

enum class TextAlign { Left, Centre, Right };

// Translated text (UTF-8) in the game's text pipeline is marked with this leading byte, which
// never starts Shift-JIS text; everything else is the game's own Shift-JIS.
inline constexpr std::uint8_t kUtf8TextMarker = 0x01;
inline bool isUtf8Text(std::span<const std::uint8_t> t) { return !t.empty() && t[0] == kUtf8TextMarker; }

class TextRenderer {
  public:
    virtual ~TextRenderer() = default;
    // UTF-8 text (translations): a proportional font, aligned within [left, left + width)
    // and narrowed to fit that width. Returns the width drawn in texels (0: nothing).
    virtual float renderUtf8(std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                             Image& band) = 0;
    // Draws `sjis` into `band` (cleared, band.width = texture width, 16 rows), starting
    // `startX` texels from the left. One byte is (F + 1) / 2 texels wide, so a full-width
    // character takes F + 1. Returns false if nothing could be rendered.
    virtual bool render(std::span<const std::uint8_t> sjis, const TextStyle& style, float startX, Image& band) = 0;
};

// Built-in fallback: ASCII bytes in the port's 5x7 font, other characters left blank.
class FallbackTextRenderer final : public TextRenderer {
  public:
    bool render(std::span<const std::uint8_t> sjis, const TextStyle& style, float startX, Image& band) override;
    float renderUtf8(std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                     Image& band) override;
};

// The original's look applied to a freshly rendered band: 5-bit colour channels, opaque
// text over a transparent background (antialiased edges darkened toward the black the
// text was drawn on), and a top-to-bottom darkening (red and green to 50 %, blue to 75 %).
void finishTextBand(Image& band);

// Byte width of the Shift-JIS character starting at `lead` (1 or 2).
inline int sjisCharBytes(std::uint8_t lead) { return (lead >= 0x81 && lead <= 0x9f) || (lead >= 0xe0 && lead <= 0xfc) ? 2 : 1; }

}  // namespace th06
