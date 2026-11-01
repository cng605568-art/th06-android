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
