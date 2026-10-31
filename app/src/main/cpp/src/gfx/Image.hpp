#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace th06 {

// Decoded RGBA8 image, top row first.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    Image() = default;
    Image(int w, int h) : width(w), height(h), rgba(static_cast<std::size_t>(w) * h * 4, 0) {}

    bool empty() const { return width <= 0 || height <= 0; }
    std::uint8_t* pixel(int x, int y) {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
    const std::uint8_t* pixel(int x, int y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

// PNG / JPEG / BMP / TGA (stb_image) and DDS (BC1/BC2/BC3/BC7 and uncompressed 32-bit).
std::optional<Image> decodeImage(std::span<const std::uint8_t> bytes, std::string* error);
std::optional<Image> decodeDds(std::span<const std::uint8_t> bytes, std::string* error);

// Direct3D-style colour key: pixels equal to `argb` (alpha included) become transparent.
// 0 disables, as in D3DX.
void applyColorKey(Image& image, std::uint32_t argb);

// The 2002 game ships some alpha channels as separate greyscale images; the engine copies
// the mask's blue channel into the texture's alpha. Masks of another size are resampled.
void applyAlphaFromMask(Image& image, const Image& mask);

// Places `image` in the top-left of a larger transparent canvas (D3DX_FILTER_NONE padding
// of textures to the size recorded in the ANM header).
Image padTo(const Image& image, int width, int height);

}  // namespace th06
