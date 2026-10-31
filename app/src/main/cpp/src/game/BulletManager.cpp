#include "game/BulletManager.hpp"

#include <cmath>

namespace th06 {
namespace {
constexpr float kCullMargin = 32.0f;
}

Bullet* BulletManager::spawn(Vec2 pos, float angle, float speed, std::uint16_t sprite,
                             float accel, float maxSpeed, float angularVel) {
    Bullet* b = pool_.spawn();
    if (!b) {
        ++dropped_;  // pool full: drop the spawn, never allocate
        return nullptr;
    }
    b->pos = pos;
    b->angle = angle;
    b->speed = speed;
    b->accel = accel;
    b->maxSpeed = maxSpeed;
    b->angularVel = angularVel;
    b->sprite = sprite;
    return b;
}

void BulletManager::ring(Vec2 pos, int count, float baseAngle, float speed, std::uint16_t sprite) {
    const float step = kTau / static_cast<float>(count);
    for (int i = 0; i < count; ++i) spawn(pos, baseAngle + step * static_cast<float>(i), speed, sprite);
}

void BulletManager::fan(Vec2 pos, int count, float centreAngle, float spread, float speed,
                        std::uint16_t sprite) {
    if (count == 1) {
        spawn(pos, centreAngle, speed, sprite);
        return;
    }
    const float step = spread / static_cast<float>(count - 1);
    for (int i = 0; i < count; ++i) {
        spawn(pos, centreAngle - spread * 0.5f + step * static_cast<float>(i), speed, sprite);
    }
}

BulletManager::TickResult BulletManager::tick(Vec2 player, float hitRadius, float grazeRadius,
                                              bool invulnerable) {
    TickResult result;
    pool_.removeIf([&](Bullet& b) {
        b.angle += b.angularVel;
        b.speed += b.accel;
        if (b.maxSpeed > 0 && b.speed > b.maxSpeed) b.speed = b.maxSpeed;
        b.pos += Vec2{std::cos(b.angle), std::sin(b.angle)} * b.speed;
        ++b.age;

        if (b.pos.x < -kCullMargin || b.pos.x > kFieldWidth + kCullMargin ||
            b.pos.y < -kCullMargin || b.pos.y > kFieldHeight + kCullMargin) {
            return true;
        }
        if (invulnerable) return false;

        const float d2 = (b.pos - player).lengthSquared();
        const float hit = hitRadius + b.radius;
        if (d2 < hit * hit) {
            result.hit = true;
            return true;
        }
        const float graze = grazeRadius + b.radius;
        if (!(b.flags & Bullet::kGrazed) && d2 < graze * graze) {
            b.flags |= Bullet::kGrazed;
            ++result.grazes;
        }
        return false;
    });
    return result;
}

void BulletManager::render(SpriteSink& sink, const std::vector<SpriteRegion>& palette) const {
    if (palette.empty()) return;
    const std::size_t n = palette.size();
    for (const Bullet& b : pool_) {
        // Sprites point up; rotate them onto the heading.
        sink.drawRegion(palette[b.sprite % n], kFieldX + b.pos.x, kFieldY + b.pos.y, 1.0f,
                        rgba(255, 255, 255), b.angle + kPi * 0.5f);
    }
}

}  // namespace th06
