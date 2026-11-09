#include "gfx/TextRenderer.hpp"

#include <algorithm>
#include <string>

#include "gfx/ProceduralArt.hpp"

namespace touhou {

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

float TextRenderer::measureUtf8(std::string_view utf8, const TextStyle& style) {
    if (utf8.empty()) return 0.0f;
    constexpr int kScratch = 2048;
    Image scratch(kScratch, 16);
    return renderUtf8(utf8, style, 0.0f, static_cast<float>(kScratch), TextAlign::Left, scratch);
}

namespace {

// The nearest ASCII for the punctuation and letters translations use (0: none).
const char* asciiFor(char32_t cp) {
    switch (cp) {
        case 0x00A0: return " ";
        case 0x3000: return "  ";  // ideographic space: a full-width cell
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2026: return "...";
        case 0x30FB: case 0x00B7: return ".";
        case 0xFF5E: case 0x301C: return "~";
        default: break;
    }
    // Latin-1 and Latin Extended-A letters with diacritics: the base letter.
    static constexpr const char* kLatin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) {
        static thread_local char one[2] = {0, 0};
        one[0] = kLatin1[cp - 0xC0];
        return one;
    }
    static constexpr const char* kExtA =
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    static_assert(std::char_traits<char>::length(kExtA) == 0x80);
    static_assert(std::char_traits<char>::length(kLatin1) == 0x40);
    if (cp >= 0x100 && cp <= 0x17F) {
        static thread_local char one[2] = {0, 0};
        one[0] = kExtA[cp - 0x100];
        return one;
    }
    return nullptr;
}

}  // namespace

std::string asciiFallback(std::string_view utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (int k = 1; k < n && i + static_cast<std::size_t>(k) < utf8.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + static_cast<std::size_t>(k)]) & 0x3F);
        }
        i += static_cast<std::size_t>(n);
        if (cp >= 0x20 && cp < 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (const char* a = asciiFor(cp)) {
            out += a;
        } else {
            out.push_back(' ');
        }
    }
    return out;
}

float FallbackTextRenderer::measureUtf8(std::string_view utf8, const TextStyle&) {
    return 6.0f * static_cast<float>(asciiFallback(utf8).size());
}

// ASCII in the 5x7 font, 6 texels a character (other characters count as one blank cell).
float FallbackTextRenderer::renderUtf8(std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                                       Image& band) {
    const std::string ascii = asciiFallback(utf8);
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

}  // namespace touhou
