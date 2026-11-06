#pragma once

// Player shot data, read at runtime from the player's own game executable
// (東方紅魔郷.exe, version 1.02h, recognised by its SHA-256). This repository contains no
// values from it: only where the tables sit in that executable and how they are laid out,
// the same kind of format knowledge used to read the .dat archives.
//
// Per shot type (0 Reimu A, 1 Reimu B, 2 Marisa A, 3 Marisa B): movement speeds, and nine
// power tiers, each a list of bullet entries (where a bullet starts, its hitbox, direction,
// speed, damage, on which frames of the 30-frame fire cycle it fires, its ANM script and
// whether it plays the shot sound).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace touhou::th06 {

enum ShotKind : std::uint8_t { ShotNormal = 0, ShotHoming = 1, ShotMissile = 2, ShotLaser = 3 };
enum ShotOrigin : std::uint8_t { ShotFromPlayer = 0, ShotFromLeftOrb = 1, ShotFromRightOrb = 2 };

struct ShotEntry {
    ShotKind kind = ShotNormal;
    ShotOrigin origin = ShotFromPlayer;
    float offsetX = 0, offsetY = 0;
    float width = 0, height = 0;
    float angle = 0;  // radians
    float speed = 0;
    std::uint16_t damage = 0;
    std::int16_t every = 1;   // fires when t mod every == phase (lasers: period)
    std::int16_t phase = 0;   // (lasers: which laser timer, 0 left / 1 right)
    std::int16_t script = 0;  // global ANM script index
    bool sound = false;
};

struct ShotTier {
    std::int32_t threshold = 0;  // power at which the next tier begins
    std::vector<ShotEntry> entries;
};

struct ShotType {
    float speed = 4.0f, speedFocus = 2.0f;
    std::array<ShotTier, 9> tiers;
};

struct ShotData {
    std::array<ShotType, 4> types;
    bool loaded = false;
};

inline constexpr const char* kTh06Exe102hSha256 = "9f76483c46256804792399296619c1274363c31cd8f1775fafb55106fb852245";

// Reads the shot tables from an executable image. requireKnownVersion = false skips the
// SHA-256 check (tests with synthetic images). Returns false and explains why otherwise.
bool loadShotData(std::span<const std::uint8_t> exe, ShotData& out, std::string* error, bool requireKnownVersion = true);

// Where the tables live in version 1.02h (virtual addresses) and their record sizes.
namespace exe102h {
inline constexpr std::uint32_t kCharacterData = 0x00476728;  // 4 x 0x18
inline constexpr std::uint32_t kPowerData[4] = {0x00476f78, 0x00477770, 0x00477ed8, 0x00478468};  // 9 x 0xC each
inline constexpr std::uint32_t kCharacterDataSize = 0x18;
inline constexpr std::uint32_t kPowerDataSize = 0x0c;
inline constexpr std::uint32_t kBulletDataSize = 0x24;
}  // namespace exe102h

}  // namespace touhou::th06
