#pragma once

// Shared state of a running stage, passed to every subsystem instead of the original
// engine's globals (g_GameManager, g_Player, g_BulletManager, ...).

#include <cstdint>
#include <map>
#include <string>

#include "core/Log.hpp"
#include "core/ZunMath.hpp"

namespace th06 {

class AnmManager;
class BulletManager;
class EnemyManager;
class EclFile;
class Player;

// Playfield of the original: 384x448 at screen (32, 16) inside 640x480.
inline constexpr float kGameRegionX = 32.0f;
inline constexpr float kGameRegionY = 16.0f;
inline constexpr float kGameRegionWidth = 384.0f;
inline constexpr float kGameRegionHeight = 448.0f;

enum Difficulty : int { Easy = 0, Normal = 1, Hard = 2, Lunatic = 3, Extra = 4 };

// SoundIdx of the original (0..31); see audio/SoundPlayer for the file/volume table.
class SoundSink {
  public:
    virtual ~SoundSink() = default;
    virtual void playSound(int soundIdx) = 0;
};

// Records game features the data asked for that this port does not implement yet, so
// gaps are visible (log + HUD) instead of silently ignored.
class UnportedFeatures {
  public:
    void hit(const char* feature) {
        auto [it, inserted] = counts_.try_emplace(feature, 0);
        if (inserted) log::warn("Not ported yet: {}", feature);
        ++it->second;
    }
    const std::map<std::string, std::uint64_t>& counts() const { return counts_; }

  private:
    std::map<std::string, std::uint64_t> counts_;
};

struct GameState {
    int difficulty = Normal;
    std::int32_t rank = 16;  // starting rank for every difficulty (DifficultyInfo table)
    std::int32_t minRank = 10, maxRank = 32;  // Normal; see setDifficulty
    std::int32_t subRank = 0;
    std::uint32_t score = 0;
    std::uint32_t frame = 0;
    std::int32_t power = 0;
    std::int32_t livesRemaining = 2;
    bool bossPresent = false;
    std::uint32_t grazes = 0;
    std::uint32_t deaths = 0;

    bool isInBounds(float x, float y, float width, float height) const {
        if (width / 2.0f + x < 0.0f) return false;
        if (x - width / 2.0f > kGameRegionWidth) return false;
        if (height / 2.0f + y < 0.0f) return false;
        if (y - height / 2.0f > kGameRegionHeight) return false;
        return true;
    }
    void setDifficulty(int d) {
        // g_DifficultyInfo: {rank, minRank, maxRank}
        static constexpr std::int32_t kInfo[5][3] = {{16, 12, 20}, {16, 10, 32}, {16, 10, 32}, {16, 10, 32}, {16, 14, 18}};
        difficulty = d < 0 || d > 4 ? Normal : d;
        rank = kInfo[difficulty][0];
        minRank = kInfo[difficulty][1];
        maxRank = kInfo[difficulty][2];
        subRank = 0;
    }
    void increaseSubrank(std::int32_t amount) {
        subRank += amount;
        while (subRank >= 100) {
            ++rank;
            subRank -= 100;
        }
        if (rank > maxRank) rank = maxRank;
    }
    void decreaseSubrank(std::int32_t amount) {
        subRank -= amount;
        while (subRank < 0) {
            --rank;
            subRank += 100;
        }
        if (rank < minRank) rank = minRank;
    }
    std::int32_t rankLerpInt(std::int32_t lo, std::int32_t hi) const { return rank * (hi - lo) / 32 + lo; }
    float rankLerpFloat(float lo, float hi) const { return static_cast<float>(rank) * (hi - lo) / 32.0f + lo; }
};

struct World {
    AnmManager* anm = nullptr;
    BulletManager* bullets = nullptr;
    EnemyManager* enemies = nullptr;
    Player* player = nullptr;
    EclFile* ecl = nullptr;  // mutable: the original lets scripts write into their own arguments
    GameState* state = nullptr;
    ZunRng* rng = nullptr;
    SoundSink* sound = nullptr;
    UnportedFeatures* unported = nullptr;

    void playSound(int idx) const {
        if (sound && idx >= 0) sound->playSound(idx);
    }
};

}  // namespace th06
