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
class EffectManager;
class Gui;
class StageBackground;
class MusicPlayer;
class ItemManager;
class Player;
class ScreenShake;
class ScoreFile;
struct ShotData;

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
    static constexpr std::int32_t kMaxPower = 128;
    static constexpr std::int32_t kMaxLives = 8;
    static constexpr std::int32_t kMaxBombs = 8;

    int difficulty = Normal;
    int shotType = 0;  // 0 Reimu A, 1 Reimu B, 2 Marisa A, 3 Marisa B (character = shotType / 2)
    std::int32_t rank = 16;  // starting rank for every difficulty (DifficultyInfo table)
    std::int32_t minRank = 10, maxRank = 32;  // Normal; see setDifficulty
    std::int32_t subRank = 0;
    std::uint32_t frame = 0;
    std::int32_t power = 0;           // 0..128
    std::int32_t livesRemaining = 2;  // spare lives (the HUD's stars)
    std::int32_t bombs = 3;
    std::int32_t startingBombs = 3;
    std::int32_t startingLives = 2;  // spare lives at the start of a game (and after a continue)
    // Game over: the player ran out of lives and the continue menu is (about to be) up. Set
    // by the player's respawn; the player ship is not drawn while it is set.
    bool continueMenu = false;
    std::int32_t continuesUsed = 0;  // this game (3 credits on Easy to Lunatic, none on Extra)
    bool bossPresent = false;
    // Boss HUD inputs written by the enemies: life-bar target (life / max life), the life
    // count from ECL, and whole seconds left on the boss timer (also the capture bonus's s).
    float bossFillTarget = 0.0f;
    std::int32_t bossLifeCount = 0;
    std::int32_t bossSeconds = 0;
    bool bombActive = false;  // a player bomb is running (see game/Bomb)
    // Sakuya's time stop (ECL ex-instruction 4), non-zero = stopped. The player, bullet,
    // item, laser, GUI, background and screen-shake passes freeze; enemies (except their
    // phase timers) and effects keep running. Only a stage start clears it.
    std::int8_t timeStop = 0;
    std::uint32_t bombsUsed = 0;
    std::uint32_t deaths = 0;
    std::uint32_t spellsCaptured = 0;  // this game
    // Completion counter of the results screen: +1 per frame of play outside dialogue and
    // boss waits, +1800 per boss life (ECL "set boss life count"). 89,500 = 100 %
    // (39,600 on Extra).
    std::int64_t completion = 0;

    // Where the playfield is on screen and how big it is. Screen shake moves the origin and
    // shrinks the size; the on-screen tests use the shrunken size for the right/bottom edges.
    float playfieldX = kGameRegionX, playfieldY = kGameRegionY;
    float playfieldW = kGameRegionWidth, playfieldH = kGameRegionHeight;
    void resetPlayfield() {
        playfieldX = kGameRegionX;
        playfieldY = kGameRegionY;
        playfieldW = kGameRegionWidth;
        playfieldH = kGameRegionHeight;
    }

    // Score. `score` is the real value; `displayedScore` rolls up towards it each frame and
    // is what extends are checked against, like the HUD counter of the original.
    std::uint32_t score = 0;
    std::uint32_t displayedScore = 0;
    std::uint32_t scoreStep = 0;
    std::int32_t extendIndex = 0;  // next extend threshold; -1 = no more extends

    std::uint32_t grazes = 0;       // this stage (caps at 9999)
    std::uint32_t totalGrazes = 0;  // whole game (caps at 999999)
    std::uint32_t stagePointItems = 0;
    std::uint32_t totalPointItems = 0;
    std::int32_t fullPowerChain = 0;  // index into the full-power score table

    bool isInBounds(float x, float y, float width, float height) const {
        if (width / 2.0f + x < 0.0f) return false;
        if (x - width / 2.0f > playfieldW) return false;
        if (height / 2.0f + y < 0.0f) return false;
        if (y - height / 2.0f > playfieldH) return false;
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
        extendIndex = difficulty == Extra ? 4 : 0;
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

    // Shot tier 0..8 from power (thresholds 8, 16, 32, 48, 64, 80, 96, 127).
    int shotTier() const {
        static constexpr std::int32_t kTiers[] = {8, 16, 32, 48, 64, 80, 96, 127};
        int tier = 0;
        while (tier < 8 && power >= kTiers[tier]) ++tier;
        return tier;
    }
    // Power-up popup level: how many of 8, 16, 32, 48, 64, 80, 96, 128 the power has reached.
    int powerLevel() const {
        static constexpr std::int32_t kLevels[] = {8, 16, 32, 48, 64, 80, 96, 128};
        int level = 0;
        for (std::int32_t l : kLevels) level += power >= l;
        return level;
    }
};

struct World {
    AnmManager* anm = nullptr;
    BulletManager* bullets = nullptr;
    EnemyManager* enemies = nullptr;
    ItemManager* items = nullptr;
    const ShotData* shots = nullptr;  // from the player's game executable; null = basic shot
    Player* player = nullptr;
    EclFile* ecl = nullptr;  // mutable: the original lets scripts write into their own arguments
    GameState* state = nullptr;
    ZunRng* rng = nullptr;
    SoundSink* sound = nullptr;
    UnportedFeatures* unported = nullptr;
    ScreenShake* shake = nullptr;
    EffectManager* effects = nullptr;
    Gui* gui = nullptr;
    StageBackground* background = nullptr;
    MusicPlayer* music = nullptr;
    ScoreFile* scores = nullptr;  // score.dat records (spell history); null in tests

    void playSound(int idx) const {
        if (sound && idx >= 0) sound->playSound(idx);
    }
};

}  // namespace th06
