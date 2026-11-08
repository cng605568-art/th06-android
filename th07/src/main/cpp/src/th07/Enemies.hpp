#pragma once

// Perfect Cherry Blossom's enemy manager: 480 enemies driven by the stage's ECL scripts, the
// stage timelines that spawn them, collision with the player and its shots, damage, death
// and item drops, the life/timer/death callbacks, bosses and spell cards, and the 24
// boss-specific "ex" routines. Logic priority 10 (after the player, before effects and
// bullets); draws at orders 5 (layers 0-1) and 7 (layers 2-3), around the player's shots.
//
// Everything here is simulation and follows the original's order of work exactly: every
// random draw (directly, or through bullets, effects and ANM scripts it starts) and every
// replay event flag happens where and as often as in the original.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "core/ZunMath.hpp"
#include "th07/Anm.hpp"
#include "th07/Bullets.hpp"
#include "th07/Ecl.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th07 {

class BgmPlayer;
struct Effect;

struct TrailNode {
    Vec3f pos{-999.0f, 0.0f, 0.0f}, velocity;
    float angle = 0;
};

struct Enemy {
    enum MoveMode : std::uint8_t { Axis = 0, Polar = 1, Interpolate = 2, Orbit = 3 };

    // Animation: the primary VM and two secondary ones (off = file index -1).
    AnmVm vm;
    std::array<AnmVm, 2> vms2{};
    std::array<bool, 2> vm2On{};

    // Script.
    EclContext ctx;
    std::array<EclContext, 16> stack{};
    std::int32_t depth = 0;
    std::int32_t deathSub = -1;
    std::array<std::int32_t, 32> interrupts{};
    std::int32_t pendingInterrupt = -1;

    // Kinematics.
    Vec3f pos, velocity, prevPos, delta;
    Vec3f hitbox{12, 12, 12}, box2;  // box2: the secondary damage box ("graze size")
    float angle = 0, angularVelocity = 0, orbitAngle = 0, orbitAngularVelocity = 0;
    float speed = 0, accel = 0, orbitRadius = 0, orbitRadialVelocity = 0;
    Vec3f shootOffset;
    Vec3f moveDelta, moveOrigin;  // interpolation delta; interpolation origin / orbit centre
    Timer moveTimer = zeroTimer();
    std::int32_t moveDuration = 0;
    std::uint8_t moveMode = Axis;
    std::uint8_t easing = 0;
    float boundsLowX = 0, boundsLowY = 0, boundsHighX = 0, boundsHighY = 0;
    bool bounds = false;

    // Bullet rank parameters.
    float rankSpeedLow = -0.15f, rankSpeedHigh = 0.15f;
    std::int16_t rankAmount1Low = 0, rankAmount1High = 0, rankAmount2Low = 0, rankAmount2High = 0;

    std::int32_t life = 1, maxLife = 0, score = 100;
    Timer timer{0, 0.0f, -999};
    std::uint32_t color = 0;

    Shooter shooter;  // the stored pattern (sound 7, per-bullet sound 25 in the template)
    std::int32_t shootInterval = 0;
    Timer shootTimer{0, 0.0f, -999};

    std::array<std::int16_t, 32> lasers{};  // laser slots, -1 none
    std::int32_t laserIndex = 0;

    std::int32_t itemDrop = 0;  // signed-byte item code: >= 0 a type, -1 random, else none
    std::int8_t death1 = 0;
    std::uint8_t death2 = 0, death3 = 0;
    std::uint8_t bossId = 0;
    std::uint8_t tint = 0;

    // Flags.
    bool bulletsDisabled = false, mirror = false, active = true, canDie = true, contact = true, damageable = true;
    bool noCollision = false, hittable = true, projectile = false, boss = false;
    std::uint8_t deathType = 0;
    bool inBounds = false, autoRotate = false, noStack = false, survival = false, oobDisabled = false;
    bool moveDisabled = false, circleFrozen = false, invisibleBomb = false, freezeBomb = false;

    std::uint16_t spellDelay = 0;
    std::uint8_t poseState = 0;
    std::array<std::int16_t, 5> pose{-1, 0, 0, -1, -1};  // default, after left, after right, left, right
    std::uint8_t drawLayer = 0;

    std::int32_t damageThisFrame = 0;

    // Spell circles (effect 13) and the spell card's shrinking circle (special effect 25).
    std::array<Effect*, 24> circles{};
    std::int32_t circleCount = 0;
    float circleRadius = 0;
    Effect* special = nullptr;

    // Callbacks.
    std::array<std::int32_t, 4> lifeThresholds{-1, -1, -1, -1};
    std::array<std::int32_t, 4> lifeSubs{};
    std::int32_t timerThreshold = -1, timerSub = 0;
    std::int32_t periodicSub = -1;
    EclArgs periodicArgs;
    Timer periodicInterval = zeroTimer(), periodicCounter = zeroTimer();

    // Trail.
    std::array<TrailNode, 96> trail{};
    std::uint8_t trailFlags = 0;  // 1 on, 2 shrink, 4 fade, 8 triangle strip, 16 hide the primary sprite
    std::int16_t trailCount = 0, trailInterval = 0, nodeStep = 0;

    Timer invincible = zeroTimer();
};

// The spell card record (the capture state the player also touches lives in GameState:
// capturing, captureValue, grazeBonus, bombUsedInCard).
struct SpellCard {
    std::uint32_t active = 0;  // 0 none, 1 running, >= 2 timed out
    std::int32_t number = 0;
    std::int32_t drain = 0;
    std::uint32_t base = 0;
};

class EnemyManager {
  public:
    static constexpr int kEnemies = 480;
    static constexpr int kBossSlots = 8;
    static constexpr int kAnmSlot = 15;
    static constexpr int kScriptBase = 0x900;

    EnemyManager();
    ~EnemyManager();

    // Stage start (manager creation, then the stage's ECL): loads data/stg<n>enm.anm into
    // ANM slot 15 and data/ecldata<n>.ecl, and draws the random-drop counters.
    bool stageStart(const World& w, const Vfs& vfs, TextureLibrary& textures, int stage, std::string* error);
    // The same with the enemy ANM already loaded and the ECL given as bytes (tests).
    bool stageStartWith(const World& w, std::span<const std::uint8_t> ecl, std::string* error);
    void release(AnmLibrary& anm);

    void update(const World& w);
    void drawLow(SpriteSink& sink, const AnmDrawState& state);   // order 5: draw layers 0-1
    void drawHigh(SpriteSink& sink, const AnmDrawState& state);  // order 7: draw layers 2-3

    // A dialogue starts: every enemy leaves at once (no score, no items, no death sub).
    void removeAll();

    // Music hooks of ex routines 19 and 20 (optional).
    void setMusic(BgmPlayer* music) { music_ = music; }

    // ---- inspection (tests, GUI) ----
    Enemy& enemy(int i) { return enemies_[static_cast<std::size_t>(i)]; }
    const Enemy& enemy(int i) const { return enemies_[static_cast<std::size_t>(i)]; }
    int liveCount() const { return liveCount_; }
    int bossSlot(int slot) const { return bosses_[static_cast<std::size_t>(slot)]; }
    const SpellCard& spellCard() const { return spell_; }
    const EclFile& ecl() const { return ecl_; }
    EclFile& ecl() { return ecl_; }
    std::uint16_t dropCounter() const { return dropCounter_; }
    std::uint16_t dropIndex() const { return dropIndex_; }

    // ---- spawning (timelines, ECL, ex routine 17) ----
    // Returns the spawned enemy, or the spare slot when all 480 are in use (nothing spawns).
    Enemy& spawn(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score, bool mirror);
    Enemy& spawnWithContext(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score,
                            const EclArgs& args);

    // ---- the variable space (spec 2.2/2.3) ----
    std::int32_t readInt(Enemy& e, std::int32_t id);
    float readFloat(Enemy& e, float raw);  // raw: the f32 argument word; its truncation names the id
    static bool intWritable(std::int32_t id);
    static bool floatWritable(std::int32_t id);
    bool writeInt(Enemy& e, std::int32_t id, std::int32_t v);  // false: not a writable int
    bool writeFloat(Enemy& e, std::int32_t id, float v);       // false: not a writable float

    // One VM step (spec 5.1); false when the script deleted the enemy (opcode 1).
    bool runVm(Enemy& e);

  private:
    friend struct EclOps;

    const World& w() const { return *w_; }
    int indexOf(const Enemy& e) const { return static_cast<int>(&e - enemies_.data()); }
    bool isSpare(const Enemy& e) const { return &e == &enemies_[kEnemies]; }
    Enemy* bossAt(std::int32_t slot);
    bool dialogueCurrent() const;
    float angleToPlayer(const Vec3f& from) const;
    void resetTemplate();

    // The VM (Ecl.cpp).
    void startSub(Enemy& e, std::int32_t sub);
    void pushContext(Enemy& e);
    enum class Exec { Next, Jumped, Restart, Delete };
    Exec execute(Enemy& e, std::uint32_t& cur);
    void movementModeUpdate(Enemy& e);
    void poseUpdate(Enemy& e);
    void runInterpolators(Enemy& e);
    float interpValue(Enemy& e, EclInterp& s, float t);
    void startScript(AnmVm& vm, std::int32_t script);
    void fireStored(Enemy& e);

    // The manager (Enemies.cpp).
    void runTimelines();
    void timelineSpawn(const std::uint8_t* rec, int op);
    void updateEnemy(Enemy& e, int index);
    void clampToBounds(Enemy& e);
    void movementStep(Enemy& e);
    bool onScreen(const Vec3f& p, float w, float h) const;
    void contactTest(Enemy& e, const Vec3f& centre, const Vec3f& box);
    void shotDamage(Enemy& e);
    void applyDamage(Enemy& e, int d);
    bool lifeCallback(Enemy& e);
    bool timerCallback(Enemy& e);
    void death(Enemy& e, int index);
    void itemDropAndBonus(Enemy& e);
    void despawn(Enemy& e);
    void resetForCallback(Enemy& e);
    void killAllSimple();
    std::uint32_t killAllScoring(std::uint32_t max, std::uint32_t total);
    void releaseCircles(Enemy& e);
    void updateCircles(Enemy& e);
    void spellCardStart(Enemy& e, const std::uint8_t* ins);
    void spellCardEnd();
    void setSpellActive(std::uint32_t v);
    void drawEnemy(SpriteSink& sink, Enemy& e, const AnmDrawState& state);
    void drawLists(SpriteSink& sink, const AnmDrawState& state, int first);

    // Ex routines (EclEx.cpp).
    void exRoutine(Enemy& e, int routine, const std::uint8_t* ins);

    const World* w_ = nullptr;
    BgmPlayer* music_ = nullptr;
    EclFile ecl_;
    std::unique_ptr<std::array<Enemy, kEnemies + 1>> storage_ = std::make_unique<std::array<Enemy, kEnemies + 1>>();
    std::array<Enemy, kEnemies + 1>& enemies_ = *storage_;  // + the spare slot
    std::unique_ptr<Enemy> template_ = std::make_unique<Enemy>();
    std::array<std::int32_t, kBossSlots> bosses_{-1, -1, -1, -1, -1, -1, -1, -1};
    std::uint16_t dropCounter_ = 0, dropIndex_ = 0;
    int liveCount_ = 0;
    SpellCard spell_;
    Timer spellTimer_ = zeroTimer();
    std::int32_t deadStore_ = 0;  // opcode 147
    struct Timeline {
        bool started = false;
        std::uint32_t pos = 0;
        Timer clock = zeroTimer();
    };
    std::array<Timeline, EclFile::kTimelines> timelines_{};
    Timer timelineTimer_ = zeroTimer();
    std::array<std::array<std::int16_t, kEnemies>, 4> drawLists_{};
    std::array<int, 4> drawCount_{};
    std::uint32_t difficultyBit_ = 2;
    int stage_ = 1;
    // Loop-carried flags of the enemy pass (the original keeps them across enemies).
    bool bombFlag_ = false, tookDamage_ = false;
    bool loaded_ = false;
    bool anmOwned_ = false;
};

}  // namespace touhou::th07
