#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Math.hpp"
#include "core/ObjectPool.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {

// Playfield geometry of the original game (640x480 screen, 384x448 field at (32,16)).
inline constexpr float kScreenWidth = 640.0f;
inline constexpr float kScreenHeight = 480.0f;
inline constexpr float kFieldX = 32.0f;
inline constexpr float kFieldY = 16.0f;
inline constexpr float kFieldWidth = 384.0f;
inline constexpr float kFieldHeight = 448.0f;

struct Bullet {
    Vec2 pos;
    float speed = 0;
    float angle = 0;       // heading, radians (0 = +x, y down)
    float accel = 0;       // speed change per tick
    float maxSpeed = 0;    // cap for accel (0 = none)
    float angularVel = 0;  // heading change per tick
    float radius = 3.0f;   // hitbox radius
    std::uint16_t sprite = 0;
    std::uint16_t flags = 0;
    std::int32_t age = 0;

    static constexpr std::uint16_t kGrazed = 1;
};

class BulletManager {
  public:
    // 8192 x 40-byte bullets + index tables = ~450 KiB, allocated once with the Game.
    static constexpr std::size_t kCapacity = 8192;

    struct TickResult {
        int grazes = 0;
        bool hit = false;
    };

    Bullet* spawn(Vec2 pos, float angle, float speed, std::uint16_t sprite, float accel = 0,
                  float maxSpeed = 0, float angularVel = 0);
    void ring(Vec2 pos, int count, float baseAngle, float speed, std::uint16_t sprite);
    void fan(Vec2 pos, int count, float centreAngle, float spread, float speed,
             std::uint16_t sprite);

    // Moves every bullet one tick, culls the ones that left the field, and tests them
    // against the player in the same pass over the pool.
    TickResult tick(Vec2 player, float hitRadius, float grazeRadius, bool invulnerable);

    void render(SpriteSink& sink, const std::vector<SpriteRegion>& palette) const;
    void clear() { pool_.clear(); }

    std::size_t size() const { return pool_.size(); }
    std::uint64_t dropped() const { return dropped_; }

  private:
    ObjectPool<Bullet, kCapacity> pool_;
    std::uint64_t dropped_ = 0;
};

}  // namespace th06
