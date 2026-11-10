#pragma once

// Imperishable Night's shot files (claude/th08-specs/spec_player_bullets_items.md §4):
// ply0Ta.sht (the human, unfocused) and ply0Tas.sht (the youkai, focused) per team; a solo
// character uses its own file for both. Each holds movement speeds, collision box sizes,
// item rules and, per power level, the list of shots the fire cycle spawns. The entries'
// four "kinds" (small integers in the file, which the original overwrites with code
// pointers) are kept as integers.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace touhou::th08 {

struct ShotEntry {
    std::int16_t interval = 0;  // period (periodic kinds), or the doll laser's trail-region count
    std::int16_t phase = 0;     // fire phase, or the laser slot (fire kind 4)
    float offsetX = 0, offsetY = 0;
    float boxW = 0, boxH = 0;
    float angle = 0, speed = 0;
    std::int16_t damage = 0;
    std::int16_t side = 0;     // < 0 a human shot, > 0 a youkai shot (only the sign is read)
    std::int16_t origin = 0;   // 0 the player, n >= 1 familiar n - 1
    std::int16_t shotClass = 0;  // 0/1 ordinary, 3 explosive, 4/5 beams, 6 no hit animation
    std::int16_t sprite = 0;   // ANM script = sprite + 10, hit animation = sprite + 11
    std::int16_t sound = -1;
    std::uint32_t fireKind = 0, updateKind = 0, drawKind = 0, hitKind = 0;
    bool beam() const { return shotClass == 4 || shotClass == 5; }
    bool explosive() const { return shotClass == 3; }
    bool piercing() const { return shotClass == 4 || shotClass == 5 || shotClass == 6; }
};

struct ShotFile {
    float bombsPerLife = 0;        // +0x04
    std::int32_t deathbombCap = 0;  // +0x08
    float hitbox = 0, grazeBox = 0;
    float itemHomingSpeed = 0;
    float itemBox = 0;
    float pocLine = 0;
    float speed = 0, focusSpeed = 0, diagonalSpeed = 0, focusDiagonalSpeed = 0;
    float itemFactor = 0;          // +0x34
    struct Level {
        std::int32_t threshold = 0;
        std::shared_ptr<const std::vector<ShotEntry>> entries;  // shared when levels share a list
    };
    std::vector<Level> levels;

    bool parse(std::span<const std::uint8_t> bytes, std::string* error);
    // The level for a power value: the first whose threshold is greater than the power.
    int levelFor(int power) const;
    const std::vector<ShotEntry>& entries(int level) const;
};

// The two files of a character: the unfocused one (the human's for a team) and the focused
// one (the youkai's).
struct ShotType {
    ShotFile unfocused, focused;
};

// The team (0-3) of a character id (0-3 teams, 4-11 solos).
inline int teamOf(int character) { return character < 4 ? character : (character - 4) / 2; }
inline bool isSolo(int character) { return character >= 4; }
inline bool isHumanSolo(int character) { return character >= 4 && character % 2 == 0; }
inline bool isYoukaiSolo(int character) { return character >= 4 && character % 2 == 1; }
// "ply02a.sht" etc.: the unfocused or focused file of a character.
std::string shotFileName(int character, bool focused);
std::string playerAnmName(int character);  // "player02.anm"

}  // namespace touhou::th08
