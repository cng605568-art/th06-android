#include "gfx/Anm.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace th06 {
namespace {

constexpr std::size_t kHeaderSize = 0x40;
constexpr std::int32_t kMaxSprites = 4096;
constexpr std::int32_t kMaxScripts = 4096;

std::uint32_t u32At(std::span<const std::uint8_t> b, std::size_t off) {
    return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

float f32At(std::span<const std::uint8_t> b, std::size_t off) {
    return std::bit_cast<float>(u32At(b, off));
}

bool cString(std::span<const std::uint8_t> b, std::uint32_t off, std::string& out) {
    if (off == 0 || off >= b.size()) return false;
    out.clear();
    for (std::size_t i = off; i < b.size() && out.size() < 260; ++i) {
        if (b[i] == 0) return !out.empty();
        out.push_back(static_cast<char>(b[i]));
    }
    return false;
}

}  // namespace

std::optional<AnmFile> parseAnm(std::span<const std::uint8_t> b, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<AnmFile> {
        if (error) *error = "ANM: " + why;
        return std::nullopt;
    };
    if (b.size() < kHeaderSize) return fail("file too small");

    const auto numSprites = static_cast<std::int32_t>(u32At(b, 0x00));
    const auto numScripts = static_cast<std::int32_t>(u32At(b, 0x04));
    if (numSprites < 0 || numSprites > kMaxSprites || numScripts < 0 || numScripts > kMaxScripts) {
        return fail("implausible sprite/script counts");
    }
    const std::size_t tablesEnd = kHeaderSize + 4u * numSprites + 8u * numScripts;
    if (tablesEnd > b.size()) return fail("sprite/script tables run past end of file");

    AnmFile anm;
    anm.textureWidth = static_cast<std::int32_t>(u32At(b, 0x0C));
    anm.textureHeight = static_cast<std::int32_t>(u32At(b, 0x10));
    anm.format = u32At(b, 0x14);
    anm.colorKey = u32At(b, 0x18);
    anm.version = u32At(b, 0x28);
    if (anm.textureWidth < 0 || anm.textureHeight < 0 || anm.textureWidth > 16384 ||
        anm.textureHeight > 16384) {
        return fail("implausible texture size");
    }
    if (!cString(b, u32At(b, 0x1C), anm.imageName)) return fail("missing texture name");
    if (const std::uint32_t alphaOff = u32At(b, 0x24); alphaOff != 0) {
        if (!cString(b, alphaOff, anm.alphaName)) return fail("bad alpha texture name");
    }

    anm.sprites.reserve(static_cast<std::size_t>(numSprites));
    for (std::int32_t i = 0; i < numSprites; ++i) {
        const std::uint32_t off = u32At(b, kHeaderSize + 4u * i);
        if (off < kHeaderSize || static_cast<std::size_t>(off) + 20 > b.size()) {
            return fail("sprite " + std::to_string(i) + " out of range");
        }
        AnmSprite s;
        s.id = u32At(b, off);
        s.x = f32At(b, off + 4);
        s.y = f32At(b, off + 8);
        s.w = f32At(b, off + 12);
        s.h = f32At(b, off + 16);
        if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.w) ||
            !std::isfinite(s.h) || s.w < 0 || s.h < 0) {
            return fail("sprite " + std::to_string(i) + " has invalid geometry");
        }
        anm.sprites.push_back(s);
    }

    const std::size_t scriptsAt = kHeaderSize + 4u * numSprites;
    for (std::int32_t i = 0; i < numScripts; ++i) {
        anm.scriptIds.push_back(u32At(b, scriptsAt + 8u * i));
    }
    return anm;
}

}  // namespace th06
