#pragma once

// Imperishable Night's enemy manager (claude/th08-specs/spec_ecl_enemies.md): 480 enemies
// driven by the stage's ECL scripts (a main context and up to four threads each), the
// timelines that spawn them, familiars (enemy "options" that follow the human/youkai side),
// collision with the player and its shots, damage, death and drops, the life/timer/death
// callbacks, bosses and spell cards, and the ex routines. Calc priority 11 (after the
// player, before effects and bullets); draws at priorities 8 and 11.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "th08/Anm.hpp"
#include "th08/Bullets.hpp"
#include "th08/Ecl.hpp"
#include "th08/World.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th08 {

struct Effect;

struct TrailNode {
    Vec3f pos{-999.0f, 0.0f, 0.0f}, velocity;
    float angle = 0;
};

struct Enemy {
    enum MoveMode : std::uint8_t { Axis = 0, Polar = 1, Interpolate = 2, Orbit = 3 };

    AnmVm vm;
    std::array<AnmVm, 2> vms2{};
    std::array<int, 2> vm2File{-1, -1};

    // Script: the main context and its stack, the threads.
    EclContext ctx;
    std::array<EclContext, 16> stack{};
    std::int16_t depth = 0;
    std::int16_t deathSub = -1;
    std::array<std::int16_t, 32> interrupts{};
    std::int16_t pendingInterrupt = -1;
    std::array<std::unique_ptr<EclThread>, 4> threads{};
    std::array<std::int32_t, 8> intLocals{};
    std::array<float, 8> floatLocals{};

    // Kinematics (P base, O parent offset, A absolute).
    Vec3f pos, offset, abs, velocity, prevPos, delta;
    Vec3f hitbox{24, 24, 24}, box2;
    float angle = 0, angularVelocity = 0, orbitAngle = 0, orbitAngularVelocity = 0;
    float speed = 0, accel = 0, orbitRadius = 0, orbitRadialVelocity = 0;
    Vec3f shootOffset;
    Vec3f moveDelta, moveOrigin;
    Timer moveTimer = zeroTimer();
    std::int32_t moveDuration = 0;
    std::uint8_t moveMode = Axis;
    std::uint8_t easing = 0;
    float boundsLowX = 0, boundsLowY = 0, boundsHighX = 0, boundsHighY = 0;
    bool bounds = false;

    float rankSpeedLow = -0.15f, rankSpeedHigh = 0.15f;
    std::array<std::int16_t, 4> rankAmounts{};

    std::int32_t life = 1, maxLife = 0, phaseLife = 0, score = 100;
    std::int32_t slot = 0;
    std::int32_t damageWord = 0;  // E+0x2E10: the player's damage accumulator
    Timer timer = zeroTimer();
    std::uint32_t color = 0;

    Shooter shooter;
    std::array<std::uint8_t, 44> deferred{};  // the deferred bullet instruction (op 107)
    std::int32_t shootInterval = 0;
    Timer shootTimer = zeroTimer();
    Shooter laserShooter;
    std::array<std::int16_t, 32> lasers{};
    std::int32_t laserIndex = 0;

    std::int32_t itemDrop = 0;
    std::int32_t extraPoints = 0, extraPower = 0;
    std::int8_t death1 = 0;
    std::uint8_t death2 = 0, death3 = 0;
    std::uint8_t bossId = 0;
    std::uint8_t tint = 0;

    // Flags (spec 4.4).
    bool active = true, boss = false, contact = true, damageable = true, noCollision = false, hittable = true;
    bool projectile = false, familiar = false, attached = false, dismissed = false, youkaiState = false;
    bool deferredBullets = false, mirror = false, bounded = false, inBounds = false, noStack = false, survival = false;
    bool oobDisabled = false, frozenWhileBombing = false, intangibleWhileBombing = false;
    std::uint8_t deathType = 0;
    bool sparkle = false, file1 = false, deathLatch = false, cannotDie = false;
    std::uint8_t lowLife = 0;
    std::uint8_t poseState = 0;
    std::array<std::int16_t, 6> pose{0, 0, 0, -1, -1, -1};  // default, after-left, after-right, left, right, extra
    std::uint8_t drawLayer = 0;
    std::uint8_t extraMask = 0;
    float noFireRadius2 = 1024.0f;

    std::int32_t damageThisFrame = 0;
    std::array<std::int32_t, 4> lifeThresholds{-1, -1, -1, -1};
    std::array<std::int32_t, 4> lifeSubs{};
    std::int32_t timerThreshold = -1, timerSub = 0;

    // Familiars (spec 9).
    Enemy* parent = nullptr;
    Enemy* prev = nullptr;
    Enemy* next = nullptr;
    std::int32_t familiarCount = 0;
    Effect* aura = nullptr;
    std::array<Effect*, 24> circles{};  // spell circles (op 128), possibly the effect sink
    std::int32_t circleCount = 0;
    float circleRadius = 0;

    std::array<TrailNode, 96> trail{};
    std::uint8_t trailFlags = 0;
    std::int16_t trailCount = 0, trailInterval = 0, nodeStep = 0;
    Timer invincible = zeroTimer();
};

// The spell card object (spec 12.1).
struct SpellCard {
    bool active = false, capturable = false, timedOut = false, noDrain = false, bombed = false;
    std::int32_t id = 0;
    Enemy* boss = nullptr;
    std::uint32_t captureValue = 0, drain = 0, displayed = 0;
    Timer timer = zeroTimer();
    std::int32_t duration = 0;
    std::string name;
};

class EnemyManager {
  public:
    static constexpr int kEnemies = 480;
    static constexpr int kBossSlots = 8;
    static constexpr int kSlotFile0 = 7;  // enemy.anm
    static constexpr int kSlotFile1 = 8;  // stgNenm.anm

    EnemyManager();
    ~EnemyManager();

    // Stage start: loads enemy.anm and stgNenm.anm and ecldataN.ecl (spell practice: the
    // "sp" file), clears the manager and draws the random-drop counters.
    bool stageStart(const World& w, const Vfs& vfs, TextureLibrary& textures, int stage, bool spellPractice, std::string* error);
    void release(AnmLibrary& anm);

    void update(const World& w);
    void drawLayers(SpriteSink& sink, const AnmDrawState& state, int first, int last);

    // ---- inspection ----
    Enemy& enemy(int i) { return enemies_[static_cast<std::size_t>(i)]; }
    const Enemy& enemy(int i) const { return enemies_[static_cast<std::size_t>(i)]; }
    int liveCount() const { return liveCount_; }
    Enemy* bossAt(int slot) const;
    const SpellCard& spellCard() const { return spell_; }
    bool bossPresent() const { return bossPresent_; }
    float bossBar() const { return bossBar_; }
    std::uint16_t dropCounter() const { return dropCounter_; }
    std::uint16_t dropIndex() const { return dropIndex_; }
    int timelinesRunning() const { return ecl_.timelineCount(); }
    bool anyBoss() const;  // any of the 8 boss slots set

    // ---- the player's hooks ----
    void playerMissed();   // death commit: the card's capture fails (0x44D150)
    void bombUsed();       // bomb start: capture fails, "bombed during this card" (0x44CBA0)
    void dissolveStart();  // the bosses can no longer be hit by shots
    void dissolveEnd();    // the bosses are finished (familiars dismissed, life 0)

    // ---- spawning ----
    Enemy& spawn(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score, bool mirror);
    Enemy& spawnWithContext(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score, const EclArgs& args);
    bool spawnFailed() const { return spawnFailed_; }

    // ---- the variable space (spec 2.2) ----
    std::int32_t readInt(Enemy& e, std::int32_t id);
    float readFloat(Enemy& e, float raw);
    static bool intWritable(std::int32_t id) { return id >= 10000 && id <= 10093 && writableInt(id); }
    static bool floatWritable(std::int32_t id) { return id >= 10016 && id <= 10095 && writableFloat(id); }
    bool writeInt(Enemy& e, std::int32_t id, std::int32_t v);
    bool writeFloat(Enemy& e, std::int32_t id, float v);

    // One VM pass (spec 5.1); false when the script deleted the enemy (opcode 1).
    bool runVm(Enemy& e);

  private:
    friend struct EclOps;
    static bool writableInt(std::int32_t id);
    static bool writableFloat(std::int32_t id);

    const World& w() const { return *w_; }
    bool isSpare(const Enemy& e) const { return &e == &enemies_[kEnemies]; }
    float angleToPlayer(const Vec3f& from) const;
    Vec3f playerPos() const;
    void resetTemplate();
    EclContext& cur() { return *cur_; }

    // The VM.
    void startSub(EclContext& c, std::int32_t sub);
    void callSub(Enemy& e, std::int32_t sub, std::uint32_t next);
    enum class Exec { Next, Jumped, Reload, Delete, ThreadEnded };
    Exec execute(Enemy& e, std::uint32_t& ip);
    void interruptEntry(Enemy& e, std::uint32_t ip);
    void endOfContext(Enemy& e, std::uint32_t ip);
    void runInterpolators(Enemy& e, EclContext& c);
    float interpValue(Enemy& e, EclInterp& s, float t);
    void movementModeUpdate(Enemy& e);
    void poseUpdate(Enemy& e);
    void startScript(Enemy& e, AnmVm& vm, std::int32_t script, bool file1);
    void bulletOp(Enemy& e, const std::uint8_t* ins);
    void freeThreads(Enemy& e);
    float exitAngle(Enemy& e);

    // The manager.
    void runTimelines();
    void timelineSpawn(const std::uint8_t* rec, int op);
    void updateEnemy(Enemy& e);
    void clampToBounds(Enemy& e);
    void movementStep(Enemy& e);
    bool onScreen(const Vec3f& p, float w, float h) const;
    void contactTest(Enemy& e);
    void shotDamage(Enemy& e);
    bool lifeCallback(Enemy& e);
    bool timerCallback(Enemy& e);
    void resetForCallback(Enemy& e);
    void death(Enemy& e);
    void drops(Enemy& e, bool autoCollect);
    void despawn(Enemy& e);
    void killAllSimple(Enemy* except);
    std::uint32_t killAllScoring(std::uint32_t max, std::uint32_t start);
    void familiarPreUpdate(Enemy& e);
    void dismissFamiliars(Enemy& e, bool bonus);
    void unlinkFromParent(Enemy& e);
    void spawnFamiliar(Enemy& parent, int op, std::int32_t sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score);
    void releaseCircles(Enemy& e);
    void updateCircles(Enemy& e);
    void spellCardStart(Enemy& e, const std::uint8_t* ins);
    void spellCardEnd();
    void spellCardUpdate();
    void exRoutine(Enemy& e, int routine, const std::uint8_t* ins);
    void drawEnemy(SpriteSink& sink, Enemy& e, const AnmDrawState& state);

    const World* w_ = nullptr;
    EclFile ecl_;
    EclGlobals globals_;
    std::vector<Enemy> enemies_;  // 480 + the spare
    std::unique_ptr<Enemy> template_;
    std::array<Enemy*, kBossSlots> bosses_{};
    std::uint16_t dropCounter_ = 0, dropIndex_ = 0;
    int liveCount_ = 0;
    bool spawnFailed_ = false;
    std::int32_t timelineGate_ = 0;
    std::array<std::int32_t, 4> signals_{-1, -1, -1, -1};
    SpellCard spell_;
    bool bossPresent_ = false;
    float bossBar_ = 0;
    struct Timeline {
        bool started = false;
        std::uint32_t pos = 0;
        Timer clock = zeroTimer();
    };
    std::array<Timeline, EclFile::kTimelines> timelines_{};
    Timer managerTimer_ = zeroTimer();
    std::array<std::vector<Enemy*>, 4> drawLists_{};
    std::uint32_t rankMask_ = 2;
    int stage_ = 0;
    bool loaded_ = false;
    bool tookDamage_ = false, bombFlag_ = false;

    // The VM pass's current context and stack (main or a thread).
    EclContext* cur_ = nullptr;
    std::array<EclContext, 16>* curStack_ = nullptr;
    std::int16_t* curDepth_ = nullptr;
    int threadCursor_ = -1;
    Enemy* running_ = nullptr;
};

}  // namespace touhou::th08
