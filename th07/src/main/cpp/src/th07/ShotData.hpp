#pragma once

// The player's shot files (data/plyNNx.sht and the focused plyNNxs.sht): movement speeds,
// collision box sizes, item rules and, per power level, the list of shots the fire cycle
// spawns. Each entry's four "kinds" (small integers in the file, which the original
// overwrites with code pointers) are kept as integers here.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace touhou::th07 {

struct ShotEntry {
    std::int16_t interval = 0;  // period (ordinary), lifetime (option laser), trail length (player laser)
    std::int16_t phase = 0;     // fire phase, or the laser slot
    float offsetX = 0, offsetY = 0;
    float boxW = 0, boxH = 0;
    float angle = 0, speed = 0;
    std::int16_t damage = 0;
    std::int8_t origin = 0;    // 0 player, 1 left option, 2 right option
    std::int8_t behaviour = 0;  // 0 ordinary, 3 explosive, 4/5 beam
    std::int16_t script = 0;    // global ANM script; the hit animation is +32
    std::int16_t sound = -1;
    std::uint32_t fireKind = 0, updateKind = 0, drawKind = 0, hitKind = 0;
    bool beam() const { return behaviour == 4 || behaviour == 5; }
    bool explosive() const { return behaviour == 3; }
};

struct ShotFile {
    float initialBombs = 0;
    std::int32_t deathbombWindow = 0;
    float hitbox = 0, grazeBox = 0;
    float autoCollectSpeed = 0;
    float itemBox = 0;
    float cherryPenalty = 0;
    float pocLine = 0;
    float speed = 0, focusSpeed = 0, diagonalSpeed = 0, focusDiagonalSpeed = 0;
    struct Level {
        std::int32_t threshold = 0;
        std::shared_ptr<const std::vector<ShotEntry>> entries;  // shared when levels share a list
    };
    std::vector<Level> levels;

    bool parse(std::span<const std::uint8_t> bytes, std::string* error);
    // The level for a power value: the first whose threshold is greater than the power.
    const std::vector<ShotEntry>& entriesFor(int power) const;
};

// The two files of a shot type (only the unfocused header is used, as in the original).
struct ShotType {
    ShotFile unfocused, focused;
    const ShotFile& header() const { return unfocused; }
};

// "data/ply00a.sht" etc. for shot index 0-5 (focused: "...s.sht").
std::string shotFileName(int shotIndex, bool focused);
std::string playerAnmName(int shotIndex);

}  // namespace touhou::th07
