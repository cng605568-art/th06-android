#include "gfx/Thtx.hpp"

#include <cstring>

namespace touhou {
namespace {

std::uint16_t rdU16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
std::uint8_t expand5(unsigned v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }
std::uint8_t expand6(unsigned v) { return static_cast<std::uint8_t>((v << 2) | (v >> 4)); }

}  // namespace

bool decodeThtx(std::span<const std::uint8_t> px, int format, int width, int height, std::vector<std::uint8_t>& rgba) {
    if (width <= 0 || height <= 0) return false;
    const int bpp = format == 1 ? 4 : format == 4 ? 3 : (format == 2 || format == 3 || format == 5) ? 2 : 0;
    if (bpp == 0) return false;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (px.size() < count * static_cast<std::size_t>(bpp)) return false;
    rgba.resize(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* s = px.data() + i * static_cast<std::size_t>(bpp);
        std::uint8_t* d = rgba.data() + i * 4;
        switch (format) {
            case 1:  // A8R8G8B8: bytes B, G, R, A
                d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
                break;
            case 2: {  // A1R5G5B5
                const unsigned v = rdU16(s);
                d[0] = expand5((v >> 10) & 31), d[1] = expand5((v >> 5) & 31), d[2] = expand5(v & 31);
                d[3] = (v & 0x8000) ? 255 : 0;
                break;
            }
            case 3: {  // R5G6B5
                const unsigned v = rdU16(s);
                d[0] = expand5((v >> 11) & 31), d[1] = expand6((v >> 5) & 63), d[2] = expand5(v & 31), d[3] = 255;
                break;
            }
            case 4:  // R8G8B8: bytes B, G, R
                d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
                break;
            case 5: {  // A4R4G4B4
                const unsigned v = rdU16(s);
                d[0] = static_cast<std::uint8_t>(((v >> 8) & 15) * 17), d[1] = static_cast<std::uint8_t>(((v >> 4) & 15) * 17);
                d[2] = static_cast<std::uint8_t>((v & 15) * 17), d[3] = static_cast<std::uint8_t>(((v >> 12) & 15) * 17);
                break;
            }
        }
    }
    return true;
}

}  // namespace touhou
