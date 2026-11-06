#include "gfx/Image.hpp"

#include <algorithm>
#include <cstring>

#include <bcdec.h>
#include <stb_image.h>

namespace touhou {
namespace {

constexpr int kMaxDimension = 16384;

std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
           (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
}

enum class Codec { BC1, BC2, BC3, BC7, RGBA8, BGRA8, Masked32 };

int maskShift(std::uint32_t mask) {
    if (!mask) return -1;
    int s = 0;
    while (!(mask & 1u)) {
        mask >>= 1;
        ++s;
    }
    return s;
}

}  // namespace

std::optional<Image> decodeImage(std::span<const std::uint8_t> bytes, std::string* error) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
        return decodeDds(bytes, error);
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h,
                                            &channels, 4);
    if (!pixels) {
        if (error) *error = std::string("image decode failed: ") + stbi_failure_reason();
        return std::nullopt;
    }
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return img;
}

std::optional<Image> decodeDds(std::span<const std::uint8_t> bytes, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<Image> {
        if (error) *error = std::string("DDS: ") + why;
        return std::nullopt;
    };
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0 ||
        le32(bytes.data() + 4) != 124) {
        return fail("bad header");
    }
    const std::uint8_t* h = bytes.data();
    const int height = static_cast<int>(le32(h + 12));
    const int width = static_cast<int>(le32(h + 16));
    const std::uint32_t pfFlags = le32(h + 80);
    const std::uint32_t code = le32(h + 84);
    const std::uint32_t bitCount = le32(h + 88);
    const std::uint32_t masks[4] = {le32(h + 92), le32(h + 96), le32(h + 100), le32(h + 104)};
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        return fail("bad dimensions");
    }

    std::size_t dataOffset = 128;
    Codec codec;
    constexpr std::uint32_t kFourCC = 0x4, kRgb = 0x40;
    if ((pfFlags & kFourCC) && code == fourcc('D', 'X', '1', '0')) {
        if (bytes.size() < 148) return fail("truncated DX10 header");
        dataOffset = 148;
        switch (le32(h + 128)) {  // DXGI_FORMAT
            case 71: case 72: codec = Codec::BC1; break;
            case 74: case 75: codec = Codec::BC2; break;
            case 77: case 78: codec = Codec::BC3; break;
            case 98: case 99: codec = Codec::BC7; break;
            case 28: case 29: codec = Codec::RGBA8; break;
            case 87: case 91: codec = Codec::BGRA8; break;
            default: return fail("unsupported DXGI format");
        }
    } else if (pfFlags & kFourCC) {
        if (code == fourcc('D', 'X', 'T', '1')) codec = Codec::BC1;
        else if (code == fourcc('D', 'X', 'T', '2') || code == fourcc('D', 'X', 'T', '3')) codec = Codec::BC2;
        else if (code == fourcc('D', 'X', 'T', '4') || code == fourcc('D', 'X', 'T', '5')) codec = Codec::BC3;
        else return fail("unsupported FourCC");
    } else if ((pfFlags & kRgb) && bitCount == 32) {
        codec = Codec::Masked32;
    } else {
        return fail("unsupported pixel format");
    }

    Image img(width, height);
    const std::uint8_t* src = bytes.data() + dataOffset;
    const std::size_t avail = bytes.size() - dataOffset;

    if (codec == Codec::RGBA8 || codec == Codec::BGRA8 || codec == Codec::Masked32) {
        const std::size_t need = static_cast<std::size_t>(width) * height * 4;
        if (avail < need) return fail("truncated pixel data");
        int shifts[4];
        for (int i = 0; i < 4; ++i) shifts[i] = maskShift(masks[i]);
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            const std::uint8_t* p = src + i * 4;
            std::uint8_t* d = img.rgba.data() + i * 4;
            if (codec == Codec::RGBA8) {
                std::memcpy(d, p, 4);
            } else if (codec == Codec::BGRA8) {
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = p[3];
            } else {
                const std::uint32_t v = le32(p);
                for (int c = 0; c < 4; ++c) {
                    d[c] = shifts[c] < 0 ? (c == 3 ? 255 : 0)
                                         : static_cast<std::uint8_t>((v & masks[c]) >> shifts[c]);
                }
            }
        }
        return img;
    }

    const int blockBytes = (codec == Codec::BC1) ? BCDEC_BC1_BLOCK_SIZE : BCDEC_BC7_BLOCK_SIZE;
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (avail < static_cast<std::size_t>(bw) * bh * blockBytes) return fail("truncated blocks");

    std::uint8_t block[4 * 4 * 4];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* b = src + (static_cast<std::size_t>(by) * bw + bx) * blockBytes;
            switch (codec) {
                case Codec::BC1: bcdec_bc1(b, block, 16); break;
                case Codec::BC2: bcdec_bc2(b, block, 16); break;
                case Codec::BC3: bcdec_bc3(b, block, 16); break;
                case Codec::BC7: bcdec_bc7(b, block, 16); break;
                default: break;
            }
            for (int y = 0; y < 4 && by * 4 + y < height; ++y) {
                const int cols = std::min(4, width - bx * 4);
                std::memcpy(img.pixel(bx * 4, by * 4 + y), block + y * 16,
                            static_cast<std::size_t>(cols) * 4);
            }
        }
    }
    return img;
}

void applyColorKey(Image& image, std::uint32_t argb) {
    if (argb == 0) return;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        std::uint8_t* p = image.rgba.data() + i;
        const std::uint32_t v = (static_cast<std::uint32_t>(p[3]) << 24) | (p[0] << 16) |
                                (p[1] << 8) | p[2];
        if (v == argb) p[0] = p[1] = p[2] = p[3] = 0;
    }
}

void applyAlphaFromMask(Image& image, const Image& mask) {
    if (image.empty() || mask.empty()) return;
    for (int y = 0; y < image.height; ++y) {
        const int my = y * mask.height / image.height;
        for (int x = 0; x < image.width; ++x) {
            const int mx = x * mask.width / image.width;
            image.pixel(x, y)[3] = mask.pixel(mx, my)[2];
        }
    }
}

void quantizeToD3DFormat(Image& image, std::uint32_t anmFormat) {
    // Expand an n-bit channel back to 8 bits the way D3D samples it (bit replication).
    auto bits = [](std::uint8_t v, int n) -> std::uint8_t {
        const unsigned q = static_cast<unsigned>(v) >> (8 - n);
        unsigned out = q << (8 - n);
        for (int shift = 8 - 2 * n; shift > -n; shift -= n) out |= shift >= 0 ? q << shift : q >> -shift;
        return static_cast<std::uint8_t>(out);
    };
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        std::uint8_t* p = image.rgba.data() + i;
        switch (anmFormat) {
            case 2:  // A1R5G5B5
                p[0] = bits(p[0], 5);
                p[1] = bits(p[1], 5);
                p[2] = bits(p[2], 5);
                p[3] = p[3] >= 128 ? 255 : 0;
                break;
            case 3:  // R5G6B5
                p[0] = bits(p[0], 5);
                p[1] = bits(p[1], 6);
                p[2] = bits(p[2], 5);
                p[3] = 255;
                break;
            case 4:  // R8G8B8
                p[3] = 255;
                break;
            case 5:  // A4R4G4B4
                p[0] = bits(p[0], 4);
                p[1] = bits(p[1], 4);
                p[2] = bits(p[2], 4);
                p[3] = bits(p[3], 4);
                break;
            default:  // 0 unknown / 1 A8R8G8B8: full precision
                break;
        }
    }
}

Image padTo(const Image& image, int width, int height) {
    Image out(std::max(width, image.width), std::max(height, image.height));
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(out.pixel(0, y), image.pixel(0, y), static_cast<std::size_t>(image.width) * 4);
    }
    return out;
}

}  // namespace touhou
