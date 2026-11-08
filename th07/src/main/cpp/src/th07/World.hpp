#pragma once

// Shared state of a running Perfect Cherry Blossom game, passed to every subsystem instead
// of the original's globals: the game manager's values (score, lives, bombs, power, the
// cherry gauges, rank, counters) and pointers to the stage's subsystems.
//
// Lives, bombs, power, misses and bombs used are floats, as the original keeps them in its
// anti-tamper block; every change of them makes the original's bookkeeping draws from the
// gameplay RNG (two u32 draws), which a replay-exact port must keep. The checks themselves
// are not ported.

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "audio/Sound.hpp"
#include "core/ZunMath.hpp"

namespace touhou::th07 {

class AnmLibrary;
class Background;
class BulletManager;
class Effects;
class Player;
class SoundPlayer;
struct ExeTables;

inline constexpr float kPlayfieldX = 32.0f, kPlayfieldY = 16.0f;
inline constexpr float kPlayfieldW = 384.0f, kPlayfieldH = 448.0f;

enum Difficulty : int { Easy = 0, Normal = 1, Hard = 2, Lunatic = 3, Extra = 4, Phantasm = 5 };

// The bullets and items as the player sees them (implemented by the bullet manager).
class BulletField {
  public:
    virtual ~BulletField() = default;
    // Every enemy bullet starts its despawn, no items (lasers without protection too).
    virtual void clearWithoutItems() = 0;
    // Every enemy bullet becomes a point-bullet item homing in (reaching full power).
    virtual void turnIntoItems() = 0;
    // Perfect Square: every active bullet's velocity, acceleration, turn and speed zeroed.
    virtual void freeze() = 0;
    virtual int liveBullets() const = 0;
};

// Screen shake jobs (the screen-effect pass).
class ShakeSink {
  public:
    virtual ~ShakeSink() = default;
    virtual void shake(int duration, float from, float to) = 0;
};

// Items as the player sees them (implemented by the item manager).
class ItemField {
  public:
    virtual ~ItemField() = default;
    enum Type : int {
        SmallPower = 0, Point = 1, BigPower = 2, Bomb = 3, FullPower = 4, Extend = 5,
        PointBullet = 6, Cherry = 7, SmallCherry = 8, Star = 9,
    };
    enum State : int { Normal = 0, AutoCollect = 1, Scatter = 2 };
    virtual void spawn(const Vec3f& pos, int type, int state) = 0;
    // Every item homes in (velocity (0, -0.5)), without the full-value flag.
    virtual void collectAll() = 0;
    // Items homing in return to falling with velocity (0, -0.9).
    virtual void stopHoming() = 0;
};

// Score popups and status messages (implemented by the GUI / ASCII manager).
class PopupSink {
  public:
    virtual ~PopupSink() = default;
    enum Status : int { FullPowerMode, SupernaturalBorder, CherryMax, BorderBonus, Bonus, SpellCardBonus };
    virtual void status(Status s, std::uint32_t value) = 0;
    // A score popup at a playfield position (colour 0xAARRGGBB; small font for point bullets).
    virtual void score(const Vec3f& pos, std::uint32_t value, std::uint32_t color, bool small) = 0;
    virtual void powerUp(const Vec3f& pos) = 0;  // the power-up glyph
};

// The GUI and dialogue as the enemies and timelines see them (implemented by the GUI; the
// defaults do nothing, for tests and while the GUI is missing).
class StageGui {
  public:
    virtual ~StageGui() = default;
    // Dialogue (msgN.dat): ECL timeline opcode 8 starts script `number`; "current" means a
    // message is up (it pauses subrank, play time and the boss bar); "wait" is timeline
    // opcode 9's query.
    virtual void startDialogue(int number) { (void)number; }
    virtual bool dialogueCurrent() const { return false; }
    virtual bool dialogueWait() const { return false; }
    // Spell cards: the card's banner (portrait index, -1 none) and its end animation.
    virtual void spellCardStart(int card, int portrait, const std::string& name) { (void)card, (void)portrait, (void)name; }
    virtual void spellCardEnd() {}
    // The player's bomb banner: portrait sprite (global index) and the bomb's name.
    virtual void bombBanner(int sprite, const std::string& name) { (void)sprite, (void)name; }
    virtual void bombBannerEnd() {}
    virtual void bonus(std::uint32_t points) { (void)points; }           // "BONUS n"
    virtual void spellCardBonus(std::uint32_t points) { (void)points; }  // "Spell Card Bonus!"
};

// What the boss HUD shows (written by the enemies, read by the GUI).
struct BossHud {
    bool present = false;  // also gates timeline spawns
    float bar = 0;         // life / maximum life of boss slot 0
    std::array<float, 8> segmentA{}, segmentB{};
    std::array<std::int32_t, 8> segmentColor{};
    std::int32_t lifeMarkers = 0;
    std::int32_t spellSeconds = 0;
    std::array<float, 4> markerX{-999, -999, -999, -999};
    std::array<bool, 4> markerTint{};
};

struct GameState {
    static constexpr int kMaxLives = 8, kMaxBombs = 8, kMaxPower = 128;
    static constexpr std::int32_t kCherryPlusFull = 50000;
    static constexpr std::int32_t kCherryMaxCap = 9999990;

    // Who is playing what.
    int difficulty = Normal;
    int shotIndex = 0;  // character * 2 + type
    int character() const { return shotIndex / 2; }
    int stage = 0;      // 1-6, 7 Extra, 8 Phantasm (stage - 1 before the set-up)
    bool practice = false;
    bool replay = false;
    int lifeCount = 2;  // the configuration copy's life setting (0-4)
    bool slowMode = false;

    // The guarded values (read as integers by truncation).
    float lives = 2, bombs = 3, power = 0, misses = 0, bombsUsed = 0;
    int livesI() const { return static_cast<int>(lives); }
    int bombsI() const { return static_cast<int>(bombs); }
    int powerI() const { return static_cast<int>(power); }

    // Score (internal units: a tenth of the displayed value).
    std::uint32_t score = 0, guiScore = 0, rollSpeed = 0;
    std::uint32_t hiScore = 100000;
    int continues = 0, hiScoreContinues = 0;

    // The cherry values, logical (stored minus the random base).
    std::int32_t cherryBase = 0;
    std::int32_t cherry = 0, cherryMax = 200000, cherryPlus = 0;

    std::int32_t rank = 16, minRank = 10, maxRank = 32, subRank = 0;

    std::uint32_t stageGraze = 0, totalGraze = 0;
    std::uint32_t stagePointItems = 0, extendPoints = 0;
    int extendsGranted = 0;  // -1: no more point-item extends (after the last stage)
    std::int32_t nextExtend = 50;
    int fullPowerChain = 0;
    int spellsCaptured = 0;

    // Spell card capture (set by the boss's ECL, read by grazes and failures).
    bool capturing = false;
    std::int32_t captureValue = 0, grazeBonus = 0;

    bool bombActive = false;
    bool spellCardActive = false;
    bool bombUsedInCard = false;
    bool dialogue = false;
    bool timeStop = false;  // never set in 1.00b
    std::uint32_t stageFrame = 0;
    std::uint32_t eventFlags = 0;  // replay event flags of this frame
    // The completion counter of the result screen (+1 per enemy-manager pass without
    // dialogue, +1800 per "set boss life markers").
    std::uint32_t completion = 0;
    BossHud boss;

    void setDifficulty(int d);
    void increaseSubrank(std::int32_t amount);
    void decreaseSubrank(std::int32_t amount);
    // Point-item extend threshold for the current number of extends granted.
    std::int32_t extendThreshold() const;
    void addScore(std::uint32_t displayPoints) { score += displayPoints / 10; }
    // Each frame (game manager step 8): the HUD score rolls towards the real score.
    void rollScore();
};

struct World {
    GameState* state = nullptr;
    ZunRng* rng = nullptr;
    AnmLibrary* anm = nullptr;
    const ExeTables* tables = nullptr;
    SoundSink* sound = nullptr;
    Player* player = nullptr;
    Effects* effects = nullptr;
    Background* background = nullptr;
    BulletField* bullets = nullptr;
    BulletManager* bulletManager = nullptr;  // the concrete manager (ECL)
    ItemField* items = nullptr;
    PopupSink* popups = nullptr;
    ShakeSink* shakes = nullptr;
    StageGui* gui = nullptr;
    // Hooks the game manager provides: a dialogue started (all enemies leave, no score), the
    // dialogue plays preloaded music slot n, the music fades out over some seconds.
    std::function<void()> onDialogueStart;
    std::function<void(int)> onMusic;
    std::function<void(float)> onMusicFade;

    void playSound(int id) const {
        if (sound && id >= 0) sound->playSound(id);
    }

    // The anti-tamper bookkeeping: "regenerate" (2 u32 draws in range 100000) after every
    // set or add of lives, power, misses or bombs used, and every add of bombs.
    void regenerate() const;
    // The 5 + 3 draws made when an Alive player is hit, before the death.
    void reroll() const;
    void setLives(float v) const;
    void addLives(float d) const;
    void setPower(float v) const;
    void addPower(float d) const;
    void addBombs(float d) const;
    void setBombs(float v) const { state->bombs = v; }  // no draws
    void addMisses(float d) const;
    void addBombsUsed(float d) const;
    void setMisses(float v) const;
    void setBombsUsed(float v) const;

    // Cherry primitives (spec: add with gauge, add, raise, raise max).
    void cherryAddWithGauge(std::int32_t a) const;
    void cherryAdd(std::int32_t a) const;
    void cherryRaise(std::int32_t a) const;
    void cherryRaiseMax(std::int32_t a) const;
    // One extend: a life below 8, else a bomb below 8, else nothing. True if something was
    // granted (the extend sound and +200 subrank follow).
    bool grantExtend() const;
    // After a point item: grants every extend the point counter has reached.
    void checkPointExtends() const;
};

}  // namespace touhou::th07
