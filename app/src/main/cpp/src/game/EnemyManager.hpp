#pragma once

// Enemies and the ECL virtual machine.
//
// Each frame the stage timeline (EclFile timeline 0) spawns enemies at fixed frame
// numbers. Every enemy runs an ECL sub on its own EclContext: all instructions whose
// `time` equals the context clock execute, then the clock advances by one. Subs call
// other subs through an 8-entry saved-context stack, react to interrupts and life/timer
// callbacks, move the enemy, fire bullet patterns through the BulletManager and drive the
// enemy's ANM VMs. Semantics follow the th06 decompilation (EclManager.cpp,
// EnemyManager.cpp), including its quirks: e.g. float arguments are variable references
// when their value truncates to a variable id, and returning from an interrupt skips one
// instruction, exactly as the 2002 executable does.

#include <array>
#include <cstdint>
#include <memory>

#include "core/HeapArray.hpp"

#include "core/ZunMath.hpp"
#include "game/BulletManager.hpp"
#include "game/Ecl.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"

namespace th06 {

inline constexpr int kAnmFileEnemy = 8;
inline constexpr int kAnmFileEnemy2 = 9;
inline constexpr int kAnmOffsetEnemy = 0x100;

struct EclContext {
    std::uint32_t currentInstr = 0;  // byte offset into the ECL file
    std::int32_t time = 0;
    std::int32_t funcSet = -1;       // repeating ex-instruction (EX_INS_REPEAT), -1 = none
    std::int32_t int0 = 0, int1 = 0, int2 = 0, int3 = 0;
    float float0 = 0, float1 = 0, float2 = 0, float3 = 0;
    std::int32_t counter0 = 0, counter1 = 0, counter2 = 0, counter3 = 0;
    std::int32_t compareRegister = 0;
    std::uint16_t subId = 0;
};

enum class EnemyMove : std::uint8_t { AxisSpeed, Velocity, Interp };
enum class EnemyInterp : std::uint8_t { Linear, Decelerate, DecelerateFast, Accelerate, AccelerateFast };
enum class EnemyDeath : std::uint8_t { DespawnNoCallback, DisableInteraction, DropItemsOnly, SetHpTo1 };

struct Enemy {
    static constexpr int kAnmSlots = 8;
    static constexpr int kMaxStackDepth = 7;

    AnmVm primaryVm;
    std::array<AnmVm, kAnmSlots> vms{};
    EclContext ctx;
    std::array<EclContext, kMaxStackDepth + 1> savedContextStack{};
    std::int32_t stackDepth = 0;
    std::int32_t deathCallbackSub = -1;
    std::array<std::int32_t, 8> interrupts{};
    std::int32_t runInterrupt = -1;
    Vec3f position;
    Vec3f prevPosition;  // render interpolation (not in the original)
    Vec3f hitboxDimensions{12.0f, 12.0f, 12.0f};
    Vec3f axisSpeed;
    float angle = 0, angularVelocity = 0, speed = 0, acceleration = 0;
    Vec3f shootOffset;
    Vec3f moveInterp;
    Vec3f moveInterpStartPos;
    std::int32_t moveInterpTimer = 0;
    std::int32_t moveInterpStartTime = 0;
    float bulletRankSpeedLow = -0.5f, bulletRankSpeedHigh = 0.5f;
    std::int16_t bulletRankAmount1Low = 0, bulletRankAmount1High = 0;
    std::int16_t bulletRankAmount2Low = 0, bulletRankAmount2High = 0;
    std::int32_t life = 1, maxLife = 1, score = 100;
    std::int32_t phaseTimer = 0;
    std::uint32_t color = 0xffffffffu;
    EnemyShooter bulletProps;
    std::int32_t shootInterval = 0;
    std::int32_t shootIntervalTimer = 0;
    std::int32_t laserStore = 0;
    // Laser slot references (pool indices, -1 = none). Like the original's raw pointers they
    // are never cleared when the laser ends, so a stale one aliases whatever laser reuses it.
    std::array<std::int16_t, 32> laserRefs{};
    std::uint8_t deathParticle1 = 0, deathParticle2 = 0, deathAnm3 = 0;  // ECL 100: explosion type, particle selector
    // Spell circles (ECL 102): effect slot indices, raw like the original's pointers (never
    // re-checked, so a reused slot gets driven too), and the radius they all grow towards.
    std::array<std::int16_t, 12> effectRefs{};
    std::int32_t effectCount = 0;
    float effectDistance = 0.0f;
    // Bat-wing trail (ex-instruction 6).
    float batAngle = 0.0f;
    std::int32_t batTimer = 0;
    bool batTimerTicked = false;
    // Bat transformation (ex-instruction 10): calls left until she turns back after a bomb.
    std::int32_t batRestoreTimer = 0;
    std::int8_t itemDrop = 0;
    std::uint8_t bossId = 0;
    std::uint8_t hitFlashTimer = 0;  // unk_e41

    // flags
    EnemyMove movementMode = EnemyMove::AxisSpeed;
    EnemyInterp moveInterpMode = EnemyInterp::Linear;
    bool shootingDisabled = false;
    bool mirrored = false;
    bool slotOccupied = false;
    bool interactable = true;
    bool collidable = true;
    bool hasBeenInBounds = false;
    bool isBoss = false;
    bool damageable = true;
    EnemyDeath deathMode = EnemyDeath::DespawnNoCallback;
    bool shouldClampPos = false;
    bool rotateAnm = false;
    bool disableCallStack = false;
    bool invisible = false;
    bool timeoutSpell = false;

    std::int16_t anmPoseCurrent = 0xff;  // 0xff default, 0 neutral, 1 left, 2 right
    std::int16_t anmPoseDefault = -1, anmPoseNeutralFromLeft = 0, anmPoseNeutralFromRight = 0;
    std::int16_t anmPoseLeft = -1, anmPoseRight = -1;
    float lowerMoveLimitX = 0, lowerMoveLimitY = 0, upperMoveLimitX = 0, upperMoveLimitY = 0;
    std::int32_t lifeCallbackThreshold = -1, lifeCallbackSub = 0;
    std::int32_t timerCallbackThreshold = -1, timerCallbackSub = 0;

    void resetRank() {
        bulletRankSpeedLow = -0.5f;
        bulletRankSpeedHigh = 0.5f;
        bulletRankAmount1Low = bulletRankAmount1High = 0;
        bulletRankAmount2Low = bulletRankAmount2High = 0;
    }
    void clampPos();
};

class EnemyManager {
  public:
    static constexpr int kMaxEnemies = 256;

    EnemyManager();

    void startStage(World& world);
    void update(World& world);
    void draw(World& world, SpriteSink& sink, float alpha);

    Enemy* spawnEnemy(World& world, int subId, const Vec3f& pos, std::int16_t life, std::int16_t itemDrop,
                      std::int32_t score);
    int activeCount() const;
    std::int32_t timelineTime() const { return timelineTime_; }
    bool timelineFinished() const { return timelineFinished_; }
    std::uint64_t instructionsExecuted() const { return instructionsExecuted_; }
    std::uint16_t randomItemSpawnIndex() const { return randomItemSpawnIndex_; }
    std::uint16_t randomItemTableIndex() const { return randomItemTableIndex_; }
    Enemy& enemy(int i) { return (*enemies_)[static_cast<std::size_t>(i)]; }
    Enemy* boss(int i) { return i >= 0 && i < 4 ? bosses_[static_cast<std::size_t>(i)] : nullptr; }

    struct SpellcardInfo {
        bool isCapturing = false;
        int isActive = 0;
        int idx = 0;
        std::int32_t captureScore = 0;
        bool usedBomb = false;
    };
    const SpellcardInfo& spellcard() const { return spellcard_; }
    // A player bomb started: the current spell card can no longer be captured, and whether
    // a card was active decides how bombs damage spell-card enemies until the next bomb.
    void failSpellcardCapture() { spellcard_.isCapturing = false; }  // the player was hit
    void onPlayerBomb() {
        spellcard_.isCapturing = false;
        spellcard_.usedBomb = spellcard_.isActive != 0;
    }

  private:
    enum class VarType { Int, Float, ReadOnly, Undefined };

    void runTimeline(World& world);
    bool runEcl(World& world, Enemy& e);  // false: the enemy deleted itself (or broke)
    void callSub(World& world, EclContext& ctx, int subId);
    bool handleLifeCallback(World& world, Enemy& e);
    bool handleTimerCallback(World& world, Enemy& e);
    void killAllNonBoss(World& world);
    void despawn(World& world, Enemy& e);
    void releaseEffects(World& world, Enemy& e);
    static std::int32_t spellcardBaseBonus(int idx);
    void driveEffects(World& world, Enemy& e);
    void batWingTrail(World& world, Enemy& e);
    void dropDeathItem(World& world, Enemy& e, bool hitByBomb);
    Laser* laserRef(World& world, Enemy& e, std::int32_t slot);

    // Variable access (EclGetVar / EclGetVarFloat / EclSetVar semantics).
    void* varCell(World& world, Enemy& e, std::int32_t id, VarType* type, void* literal);
    std::int32_t readInt(World& world, Enemy& e, std::int32_t raw);
    float readFloat(World& world, Enemy& e, float raw);
    void setVar(World& world, Enemy& e, std::int32_t outId, std::int32_t rawValue);
    void mathOp(World& world, Enemy& e, std::int16_t op, std::int32_t out, std::int32_t lhs, std::int32_t rhs);

    void moveDirTime(World& world, Enemy& e, std::uint32_t at);
    void movePosTime(World& world, Enemy& e, std::uint32_t at);
    void moveTime(World& world, Enemy& e, std::uint32_t at);
    void runExInstruction(World& world, Enemy& e, int index, std::uint32_t at);

    // Stage-specific ex-instructions and the bullet scans they share. Scans walk the 640
    // slots in order and skip bullets that are not live (inactive or despawning).
    enum class BulletSize { None, Small, Large };
    static bool liveBullet(const Bullet& b);
    static BulletSize bulletSize(World& world, const Bullet& b);
    static void recolor(World& world, Bullet& b, int color);
    static void armAcceleration(Bullet& b, float angle, std::int32_t duration);
    static EnemyShooter localRecord();
    void exPentagram(World& world, Enemy& e);
    void exTimeStop(World& world, Enemy& e, std::int32_t mode);
    void exDaggerArcs(World& world, Enemy& e);
    void batTransformation(World& world, Enemy& e);

    HeapArray<Enemy, kMaxEnemies> enemies_;
    Enemy template_;
    std::array<Enemy*, 4> bosses_{};
    std::uint32_t timelineInstr_ = 0;
    std::int32_t timelineTime_ = 0;
    std::int32_t prevTimelineTime_ = -1;
    bool timelineFinished_ = false;
    bool bossWaiting_ = false;  // the timeline is held until a boss is gone
    SpellcardInfo spellcard_;
    std::uint64_t instructionsExecuted_ = 0;
    std::uint16_t randomItemSpawnIndex_ = 0;  // every third random drop yields an item
    std::uint16_t randomItemTableIndex_ = 0;
    // Scratch cells for computed read-only variables (the original's function statics).
    float scratchPlayerAngle_ = 0, scratchPlayerDistance_ = 0;
    std::int32_t scratchShot_ = 0, scratchDifficulty_ = 0, scratchRank_ = 0;
    // Ex-instruction 2's star: one copy for every enemy, never reset (not even between
    // stages), like the original's globals.
    std::array<float, 6> starAngles_{};
    Vec3f starEnemy_, starPlayer_;
};

}  // namespace th06
