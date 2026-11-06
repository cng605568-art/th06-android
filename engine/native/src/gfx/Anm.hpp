#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace touhou {

// Sprite table of a 2002-format ANM file (version 0).
//
// Header (0x40 bytes, little-endian), field names after the th06 decompilation:
//   0x00 i32 numSprites     0x04 i32 numScripts    0x08 u32 textureSlot
//   0x0C i32 width          0x10 i32 height        0x14 u32 format
//   0x18 u32 colorKey       0x1C u32 nameOffset    0x20 u32 (sprite index offset)
//   0x24 u32 alphaNameOffset (0 = none)             0x28 u32 version
//   0x2C u32 -              0x30 u32 textureOffset 0x34 u32 hasData
//   0x38 u32 nextOffset     0x3C u32 -
// Then u32 spriteOffsets[numSprites], then {u32 id, u32 offset} scripts[numScripts].
// Sprite: {u32 id, f32 x, f32 y, f32 w, f32 h} in texture pixels.
// The texture itself is a separate image file in the archive named at nameOffset;
// a name starting with '@' means "render target", not a file.
struct AnmSprite {
    std::uint32_t id = 0;
    float x = 0, y = 0, w = 0, h = 0;
};

// One animation script. Instructions are {i16 time, u8 opcode, u8 argBytes, args...};
// opcode 1 (set sprite) and 16 (random sprite) take a sprite ID as their first i32.
// The engine uses a script's first sprite: e.g. bullet colours are that sprite + colour,
// exactly as the original engine offsets the script's sprite by the bullet's colour.
struct AnmScript {
    std::uint32_t id = 0;
    std::uint32_t offset = 0;       // byte offset of the first instruction in the file
    std::int32_t firstSprite = -1;  // -1: the script never sets a sprite
};

struct AnmFile {
    int textureWidth = 0;
    int textureHeight = 0;
    std::uint32_t format = 0;
    std::uint32_t colorKey = 0;
    std::uint32_t version = 0;
    std::string imageName;
    std::string alphaName;
    std::vector<AnmSprite> sprites;
    std::vector<AnmScript> scripts;

    // Index into `sprites` of the sprite with this ID, or -1.
    int spriteIndexById(std::int64_t id) const;
    // First sprite ID set by the script with this ID, or -1.
    std::int32_t scriptFirstSprite(std::uint32_t scriptId) const;
};

std::optional<AnmFile> parseAnm(std::span<const std::uint8_t> bytes, std::string* error);

}  // namespace touhou
