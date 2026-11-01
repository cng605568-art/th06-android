#pragma once

// Enemy bullets of the 2002 game: a fixed array of 640 slots (no heap traffic during
// play), ten bullet types whose sprites come from etama3.anm / etama4.anm scripts, the
// nine aim modes of the ECL bullet opcodes, and the "ex" movement modifiers.
// Spawning, motion, despawning, graze and hit tests follow the original engine
// (th06 decompilation, BulletManager.cpp).

#include <array>
#include <cstdint>
#include <memory>

#include "core/ZunMath.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"

namespace th06 {

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

class BulletManager {
  public:
    static constexpr int kMaxBullets = 640;
    static constexpr int kBulletTypes = 10;

    // Requires etama3.anm / etama4.anm loaded into the AnmManager.
    bool initializeTemplates(World& world, std::string* error);

    // SpawnBulletPattern: count2 rows x count1 bullets; stops silently when full.
    void spawnBulletPattern(World& world, const EnemyShooter& props);
    // Returns false when no slot was free.
    bool spawnSingleBullet(World& world, const EnemyShooter& props, int idx1, int idx2, float angleToPlayer);

    void update(World& world);
    void draw(World& world, SpriteSink& sink, float alpha);

    void removeAllBullets(bool turnIntoItem);  // turnIntoItem: items are not ported, bullets vanish
    std::int32_t despawnBullets(World& world, std::int32_t maxBonusScore, bool awardPoints);

    int activeCount() const;
    int peakCount() const { return peak_; }
    std::uint64_t spawnFailures() const { return spawnFailures_; }
    Bullet& at(int i) { return (*bullets_)[static_cast<std::size_t>(i)]; }

  private:
    std::array<BulletTypeSprites, kBulletTypes> templates_{};
    std::unique_ptr<std::array<Bullet, kMaxBullets>> bullets_ = std::make_unique<std::array<Bullet, kMaxBullets>>();
    int nextBulletIndex_ = 0;
    int peak_ = 0;
    std::uint64_t spawnFailures_ = 0;
};

}  // namespace th06
