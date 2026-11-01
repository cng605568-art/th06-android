#pragma once

// The player: movement with the shot type's speeds and the original clamping, the 1.25 px
// hitbox and graze box, the player.anm body animations, two option orbs that slide in when
// focusing, death and respawn invulnerability, and the shot system: an 80-slot pool fed by a
// 30-frame fire cycle from the power tier's bullet list (normal shots, homing amulets,
// accelerating missiles that explode with decaying damage, and Marisa B's lasers).
//
// The bullet lists come from the player's own game executable (game/ShotData). Without it
// a single basic forward shot is used.

#include <array>
#include <cstdint>

#include "core/ZunMath.hpp"
#include "game/Input.hpp"
#include "game/ShotData.hpp"
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
    enum class OrbState : std::uint8_t { Hidden, Unfocused, Focusing, Focused, Unfocusing };
    enum class ShotState : std::uint8_t { Inactive, Fired, Collided };

    static constexpr int kMaxBullets = 80;
    static constexpr int kFireCycle = 30;

    struct Bullet {
        AnmVm sprite;
        Vec3f position;
        Vec3f prevPosition;
        Vec3f velocity;
        Vec3f size;
        float angle = 0, speed = 0;
        std::int32_t lifetime = 0;
        std::uint16_t damage = 0;
        std::int16_t script = 0;  // the script it was fired with (hit animation = script + 32)
        ShotKind kind = ShotNormal;
        ShotState state = ShotState::Inactive;
        std::int8_t laserTimer = 0;  // lasers: which laser timer drives it
        std::int8_t orb = 0;         // lasers: which orb it hangs from
        float offsetX = 0;
    };

    void reset(World& world);
    void update(World& world, const InputState& input);
    // Fired shots, the body and the orbs (drawn under the enemies).
    void drawLow(World& world, SpriteSink& sink, float alpha);
    // Shots playing their hit animation (drawn over the enemies).
    void drawHits(World& world, SpriteSink& sink, float alpha);

    float angleToPlayer(const Vec3f& pos) const;
    int checkGraze(World& world, const Vec3f& center, const Vec3f& size);
    int checkCollision(World& world, const Vec3f& center, const Vec3f& size);
    // Laser hit and graze test. center/size describe the laser's box in its own unrotated
    // frame (laser along +x from origin); the player is rotated into that frame.
    void checkLaser(World& world, const Vec3f& center, const Vec3f& size, const Vec3f& origin, float angle,
                    bool canGraze);
    // Damage of every shot touching the enemy box this frame; touched shots switch to their
    // hit animation (missiles keep exploding with decaying damage, lasers keep burning).
    int calcDamageToEnemy(World& world, const Vec3f& enemyPos, const Vec3f& enemySize, bool* hitByBomb);
    // Homing target for the next frame: the eligible enemy lowest on the screen.
    void considerHomingTarget(const Vec3f& enemyPos);

    // Inspection (tests, HUD).
    const Bullet& bullet(int i) const { return bullets_[static_cast<std::size_t>(i)]; }
    int activeBulletCount() const;
    OrbState orbState() const { return orbState_; }
    const Vec3f& orbPosition(int i) const { return orbPositions_[static_cast<std::size_t>(i)]; }
    int fireTimer() const { return fireTimer_; }
    int laserTimer(int i) const { return laserTimers_[static_cast<std::size_t>(i)]; }
    const Vec3f& homingTarget() const { return homingTarget_; }
    int shotTier(const World& world) const;

    Vec3f positionCenter;
    Vec3f prevPosition;
    State state = Spawning;
    bool focused = false;

  private:
    void die(World& world);
    void dropItemsOnDeath(World& world);
    void respawn(World& world);
    void move(World& world, const InputState& in);
    void updateOrbs();
    void updateShots(World& world);
    void spawnShots(World& world, int t);
    void fire(World& world, Bullet& b, const ShotEntry& e);
    const ShotType& shotType(const World& world) const;
    const ShotTier& tierFor(const World& world) const;

    AnmVm sprite_;
    std::array<AnmVm, 2> orbVms_{};
    std::array<Bullet, kMaxBullets> bullets_{};
    std::array<Vec3f, 2> orbPositions_{};
    OrbState orbState_ = OrbState::Hidden;
    int orbCounter_ = 0;
    Vec3f hitboxSize{1.25f, 1.25f, 5.0f};
    Vec3f hitboxTopLeft, hitboxBottomRight;
    float previousSpeedX_ = 0.0f;
    std::int32_t invulnerabilityTimer_ = 0;
    std::int32_t deathTimer_ = 0;
    std::int32_t deathbombWindow_ = 0;  // frames after a hit until the death drops happen
    std::int32_t bulletGrace_ = 0;      // frames after respawning during which bullets are cleared
    int fireTimer_ = -1;                // position in the 30-frame fire cycle, -1 = idle
    std::array<int, 2> laserTimers_{};
    Vec3f homingTarget_{-999.0f, -999.0f, 0.0f};
    std::uint8_t laserParticleCounter_ = 0;
};

}  // namespace th06
