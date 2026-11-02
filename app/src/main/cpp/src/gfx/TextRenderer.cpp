#include "gfx/TextRenderer.hpp"

#include <algorithm>
#include <string>

#include "gfx/ProceduralArt.hpp"

namespace th06 {

bool FallbackTextRenderer::render(std::span<const std::uint8_t> sjis, const TextStyle& style, float startX, Image& band) {
    const float cell = static_cast<float>(style.fontSize + 1) / 2.0f;  // texels per byte
    const auto r = static_cast<std::uint8_t>(style.rgb >> 16), g = static_cast<std::uint8_t>(style.rgb >> 8),
               b = static_cast<std::uint8_t>(style.rgb);
    bool any = false;
    float x = startX;
    for (std::size_t i = 0; i < sjis.size();) {
        const int n = sjisCharBytes(sjis[i]);
        if (n == 1 && sjis[i] >= 0x20 && sjis[i] < 0x7f) {
            const std::string c(1, static_cast<char>(sjis[i]));
            rasterizeText(band, static_cast<int>(x) + 2, 5, c, 1, 0, 0, 0);  // shadow
            rasterizeText(band, static_cast<int>(x) + 1, 4, c, 1, r, g, b);
            any = true;
        }
        x += cell * static_cast<float>(n);
        i += static_cast<std::size_t>(n);
    }
    return any;
}

// ASCII in the 5x7 font, 6 texels a character (other characters count as one blank cell).
float FallbackTextRenderer::renderUtf8(std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                                       Image& band) {
    std::string ascii;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if ((c & 0xC0) == 0x80) continue;  // continuation bytes
        ascii.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : ' ');
    }
    const float advance = 6.0f;
    const float full = advance * static_cast<float>(ascii.size());
    const float drawn = std::min(full, width);
    float x = align == TextAlign::Right ? left + width - drawn : align == TextAlign::Centre ? left + (width - drawn) / 2.0f : left;
    const std::size_t fit = static_cast<std::size_t>(drawn / advance);
    const auto r = static_cast<std::uint8_t>(style.rgb >> 16), g = static_cast<std::uint8_t>(style.rgb >> 8),
               b = static_cast<std::uint8_t>(style.rgb);
    const std::string shown = ascii.substr(0, fit);
    if (shown.find_first_not_of(' ') == std::string::npos) return 0.0f;
    rasterizeText(band, static_cast<int>(x) + 1, 5, shown, 1, 0, 0, 0);
    rasterizeText(band, static_cast<int>(x), 4, shown, 1, r, g, b);
    return drawn;
}

void finishTextBand(Image& band) {
    for (int y = 0; y < band.height; ++y) {
        const float k = band.height > 1 ? static_cast<float>(y) / static_cast<float>(band.height - 1) : 0.0f;
        const float rg = 1.0f - k / 2.0f, bl = 1.0f - k / 4.0f;
        for (int x = 0; x < band.width; ++x) {
            std::uint8_t* p = band.pixel(x, y);
            if (p[3] < 16) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            const float a = static_cast<float>(p[3]) / 255.0f;
            auto quantise = [](float v) { return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v), 0, 255) & 0xf8); };
            p[0] = quantise(static_cast<float>(p[0]) * a * rg);
            p[1] = quantise(static_cast<float>(p[1]) * a * rg);
            p[2] = quantise(static_cast<float>(p[2]) * a * bl);
            p[3] = 255;
        }
    }
}

}  // namespace th06
