#pragma once

// Enemy bullets of the 2002 game: a fixed array of 640 slots (no heap traffic during
// play), ten bullet types whose sprites come from etama3.anm / etama4.anm scripts, the
// nine aim modes of the ECL bullet opcodes, and the "ex" movement modifiers.
// Spawning, motion, despawning, graze and hit tests follow the original engine
// (th06 decompilation, BulletManager.cpp).

#include <array>

#include "core/HeapArray.hpp"
#include <cstdint>
#include <memory>
#include <string>

#include "core/ZunMath.hpp"
#include "th06/World.hpp"
#include "gfx/AnmManager.hpp"

namespace touhou::th06 {

inline constexpr int kAnmFileBullet3 = 6;
inline constexpr int kAnmFileBullet4 = 7;
inline constexpr int kAnmOffsetBullet3 = 0x200;
inline constexpr int kAnmOffsetBullet4 = 0x29a;
inline constexpr int kAnmScriptBullet3LineLaser = kAnmOffsetBullet3 + 9;

enum BulletAimMode : std::uint16_t {
    FanAimed, Fan, CircleAimed, Circle, OffsetCircleAimed, OffsetCircle, RandomAngle, RandomSpeed, RandomAll,
};

enum BulletState : std::uint16_t { Inactive, Fired, SpawningFast, SpawningNormal, SpawningSlow, Despawning };

enum BulletExFlags : std::uint32_t {
    ExSpeedup = 0x1,
    ExSpawnEffectShort = 0x2,
    ExSpawnEffect = 0x4,
    ExSpawnEffectLong = 0x8,
    ExAcceleration = 0x10,
    ExVelocity = 0x20,
    ExAngleAdd = 0x40,
    ExAnglePlayer = 0x80,
    ExAngleSet = 0x100,
    ExSpawnSound = 0x200,
    ExBounceTblr = 0x400,
    ExBounceTlr = 0x800,
};

// Parameters of one bullet pattern (EnemyShooter in the original).
struct EnemyShooter {
    std::int16_t sprite = 0;  // bullet type 0..9
    std::int16_t color = 0;
    Vec3f position;
    float angle1 = 0, angle2 = 0;
    float speed1 = 0, speed2 = 0;
    float exFloats[4] = {};  // laser: startOffset, endOffset, startLength, width
    std::int32_t exInts[5] = {};  // laser: startTime, duration, despawnDuration, hitboxStartTime, hitboxEndDelay
    std::int16_t count1 = 0, count2 = 0;
    std::uint16_t aimMode = 0;
    std::uint32_t flags = 0;
    std::int32_t sfx = -1;
};

struct BulletTypeSprites {
    AnmVm spriteBullet;
    AnmVm spriteSpawnEffectFast;
    AnmVm spriteSpawnEffectNormal;
    AnmVm spriteSpawnEffectSlow;
    AnmVm spriteSpawnEffectDonut;
    Vec3f grazeSize;
    std::uint8_t bulletHeight = 0;
};

struct Bullet {
    BulletTypeSprites sprites;
    Vec3f pos;
    Vec3f prevPos;  // for render interpolation (not part of the original)
    Vec3f velocity;
    Vec3f ex4Acceleration;
    float speed = 0, exVelSpeed = 0, dirChangeSpeed = 0;
    float angle = 0, exVelAngle = 0, dirChangeRotation = 0;
    std::int32_t timer = 0;
    std::int32_t exDuration = 0, dirChangeInterval = 0, dirChangeNumTimes = 0, dirChangeMaxTimes = 0;
    std::uint16_t exFlags = 0;
    std::int16_t color = 0;
    std::uint16_t state = Inactive;
    std::uint16_t outOfBoundsTime = 0;
    std::uint8_t isGrazed = 0;
};

// Enemy lasers (ECL LASER_CREATE). A laser grows from its origin along `angle`: the tip
// (endOffset) advances by `speed` each frame and the tail (startOffset) follows once the
// laser is `startLength` long. It goes through a warning phase (thin line, START_DELAY),
// the active phase and a shrinking despawn phase.
enum LaserState : std::uint8_t { LaserStartDelay = 0, LaserActive = 1, LaserDespawning = 2 };
inline constexpr std::uint16_t kLaserFlagFadeInOut = 0x1;

struct LaserProps {
    std::int16_t sprite = 0;  // 0 = line laser, 1 = beam
    std::int16_t color = 0;
    Vec3f position;
    float angle = 0, speed = 0, startOffset = 0, endOffset = 0, startLength = 0, width = 0;
    std::int32_t startTime = 0, duration = 0, despawnDuration = 0, hitboxStartTime = 0, hitboxEndDelay = 0;
    std::uint16_t flags = 0;
    bool aimed = false;
};

struct Laser {
    AnmVm vm;      // the beam
    AnmVm glowVm;  // glow at the origin
    Vec3f pos;
    float angle = 0, startOffset = 0, endOffset = 0, startLength = 0, width = 0, speed = 0;
    float prevStartOffset = 0, prevEndOffset = 0;  // render interpolation (not part of the original)
    std::int32_t startTime = 0, hitboxStartTime = 0, duration = 0, despawnDuration = 0, hitboxEndDelay = 0;
    std::int32_t timer = 0;
    std::uint16_t flags = 0;
    std::int16_t color = 0;
    std::uint8_t state = LaserStartDelay;
    bool inUse = false;
};

class BulletManager {
  public:
    static constexpr int kMaxBullets = 640;
    static constexpr int kBulletTypes = 10;
    static constexpr int kMaxLasers = 64;

    bool initializeTemplates(World& world, std::string* error);

    void spawnBulletPattern(World& world, const EnemyShooter& props);
    bool spawnSingleBullet(World& world, const EnemyShooter& props, int idx1, int idx2, float angleToPlayer);
    // Lowest free laser slot, or -1 when all 64 are in use.
    int spawnLaser(World& world, const LaserProps& props);

    // Items first, then bullets, then lasers (the original's order within one frame).
    void update(World& world);
    // Lasers, then items, then bullets in four size passes.
    void draw(World& world, SpriteSink& sink, float alpha);

    // convert: every bullet becomes a homing star item at once (else it plays its cancel
    // animation); lasers start despawning (leaving a trail of stars when converting).
    void removeAllBullets(World& world, bool convert);
    // Spell card end / boss defeat: cancels everything for a growing per-bullet bonus.
    std::int32_t despawnBullets(World& world, std::int32_t maxBonusScore, bool awardPoints);

    int activeCount() const;
    int activeLaserCount() const;
    int peakCount() const { return peak_; }
    std::uint64_t spawnFailures() const { return spawnFailures_; }
    Bullet& at(int i) { return (*bullets_)[static_cast<std::size_t>(i)]; }
    Laser& laser(int i) { return lasers_[static_cast<std::size_t>(i)]; }

  private:
    void updateLasers(World& world);
    void laserStars(World& world, const Laser& l);

    std::array<BulletTypeSprites, kBulletTypes> templates_{};
    HeapArray<Bullet, kMaxBullets> bullets_;
    std::array<Laser, kMaxLasers> lasers_{};
    int nextBulletIndex_ = 0;
    int peak_ = 0;
    std::uint64_t spawnFailures_ = 0;
};

}  // namespace touhou::th06
