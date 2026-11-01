#pragma once

// The player, as far as stage logic needs it: movement with the original speeds and
// clamping, the 1.25 px hitbox and graze box, the player.anm animation scripts, death and
// respawn invulnerability, and the Reimu A level-1 shot (damage dealt to ECL enemies).
// Bombs, power levels, options and the other shot types are not ported yet.

#include <array>
#include <cstdint>

#include "core/ZunMath.hpp"
#include "game/Input.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"

namespace th06 {

inline constexpr int kAnmFilePlayer = 5;
inline constexpr int kAnmOffsetPlayer = 0x400;

class Player {
  public:
    enum GrazeResult : int { GrazeFalse = 0, GrazeTrue = 1, CanceledByBomb = 2 };
    enum CollisionResult : int { CollisionNone = 0, CollisionHit = 1, CollisionGraze = 2 };
    enum State : int { Alive, Spawning, Dead, Invulnerable };

    static constexpr int kMaxBullets = 80;

    struct Bullet {
        AnmVm sprite;
        Vec3f position;
        Vec3f prevPosition;
        Vec3f velocity;
        Vec3f size;
        std::uint16_t damage = 0;
        enum : std::uint8_t { Inactive, Fired, Hit } state = Inactive;
    };

    void reset(World& world);
    void update(World& world, const InputState& input);
    void draw(World& world, SpriteSink& sink, float alpha);

    float angleToPlayer(const Vec3f& pos) const;
    int checkGraze(World& world, const Vec3f& center, const Vec3f& size);
    int checkCollision(World& world, const Vec3f& center, const Vec3f& size);
    // Sum of damage of shots overlapping the enemy box; those shots switch to their hit
    // animation (CalcDamageToEnemy).
    int calcDamageToEnemy(World& world, const Vec3f& enemyPos, const Vec3f& enemySize, bool* hitByBomb);

    Vec3f positionCenter;
    Vec3f prevPosition;
    State state = Spawning;
    bool focused = false;

  private:
    void die(World& world);

    AnmVm sprite_;
    std::array<Bullet, kMaxBullets> bullets_{};
    Vec3f hitboxSize{1.25f, 1.25f, 5.0f};
    Vec3f hitboxTopLeft, hitboxBottomRight;
    float previousSpeedX_ = 0.0f;
    std::int32_t invulnerabilityTimer_ = 0;
    std::int32_t deathTimer_ = 0;
    std::uint32_t shotTimer_ = 0;
};

}  // namespace th06
