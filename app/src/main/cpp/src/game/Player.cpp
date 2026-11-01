#include "game/Player.hpp"

#include <cmath>

#include "game/BulletManager.hpp"

namespace th06 {
namespace {

// player00.anm script ids (+0x400), from the decompilation's AnmIdx.hpp.
constexpr int kScriptIdle = kAnmOffsetPlayer + 0;
constexpr int kScriptMovingLeft = kAnmOffsetPlayer + 1;
constexpr int kScriptStoppingLeft = kAnmOffsetPlayer + 2;
constexpr int kScriptMovingRight = kAnmOffsetPlayer + 3;
constexpr int kScriptStoppingRight = kAnmOffsetPlayer + 4;
constexpr int kScriptPlayerBullet = kAnmOffsetPlayer + 64;
constexpr int kScriptBulletHitOffset = 32;

// Reimu A (g_CharData): orthogonal 4.0 / focused 2.0 px per frame; diagonals / sqrt(2).
constexpr float kSpeed = 4.0f;
constexpr float kSpeedFocus = 2.0f;

// Reimu A shot, power rank 1 (g_CharacterPowerBulletDataReimuARank1).
constexpr int kShotWait = 5;
constexpr float kShotSize = 12.0f;
constexpr float kShotVelocity = 12.0f;
constexpr std::uint16_t kShotDamage = 48;

constexpr float kMoveLeft = 8.0f, kMoveTop = 16.0f;
constexpr float kMoveWidth = kGameRegionWidth - 16.0f, kMoveHeight = kGameRegionHeight - 32.0f;
constexpr int kSoundShoot = 0, kSoundPichun = 4, kSoundGraze = 30;

bool overlap(const Vec3f& aTL, const Vec3f& aBR, const Vec3f& bTL, const Vec3f& bBR) {
    return !(aTL.x > bBR.x || aBR.x < bTL.x || aTL.y > bBR.y || aBR.y < bTL.y);
}

}  // namespace

void Player::reset(World& world) {
    positionCenter = {kGameRegionWidth / 2.0f, kGameRegionHeight - 64.0f, 0.49f};
    prevPosition = positionCenter;
    state = Spawning;
    invulnerabilityTimer_ = 120;
    previousSpeedX_ = 0.0f;
    bullets_ = {};
    if (world.anm->hasScript(kScriptIdle)) world.anm->setAndExecuteScript(sprite_, kScriptIdle);
    hitboxTopLeft = positionCenter - hitboxSize;
    hitboxBottomRight = positionCenter + hitboxSize;
}

float Player::angleToPlayer(const Vec3f& pos) const {
    const float relX = positionCenter.x - pos.x;
    const float relY = positionCenter.y - pos.y;
    if (relY == 0.0f && relX == 0.0f) return kZunHalfPi;  // straight down
    return std::atan2(relY, relX);
}

void Player::update(World& world, const InputState& in) {
    prevPosition = positionCenter;
    for (Bullet& b : bullets_) b.prevPosition = b.position;

    if (state == Dead) {
        if (--deathTimer_ <= 0) {
            world.bullets->removeAllBullets(false);
            positionCenter = {kGameRegionWidth / 2.0f, kGameRegionHeight - 64.0f, 0.49f};
            prevPosition = positionCenter;
            state = Spawning;
            invulnerabilityTimer_ = 240;
        }
    } else {
        focused = in.focus;
        const int dx = static_cast<int>(in.right) - static_cast<int>(in.left);
        const int dy = static_cast<int>(in.down) - static_cast<int>(in.up);
        const float ortho = focused ? kSpeedFocus : kSpeed;
        const float diag = ortho / std::sqrt(2.0f);
        float speedX = 0.0f, speedY = 0.0f;
        if (dx != 0 && dy != 0) {
            speedX = static_cast<float>(dx) * diag;
            speedY = static_cast<float>(dy) * diag;
        } else {
            speedX = static_cast<float>(dx) * ortho;
            speedY = static_cast<float>(dy) * ortho;
        }

        if (speedX < 0.0f && previousSpeedX_ >= 0.0f) world.anm->setAndExecuteScript(sprite_, kScriptMovingLeft);
        else if (speedX == 0.0f && previousSpeedX_ < 0.0f) world.anm->setAndExecuteScript(sprite_, kScriptStoppingLeft);
        if (speedX > 0.0f && previousSpeedX_ <= 0.0f) world.anm->setAndExecuteScript(sprite_, kScriptMovingRight);
        else if (speedX == 0.0f && previousSpeedX_ > 0.0f) world.anm->setAndExecuteScript(sprite_, kScriptStoppingRight);
        previousSpeedX_ = speedX;

        positionCenter.x += speedX;
        positionCenter.y += speedY;
        if (positionCenter.x < kMoveLeft) positionCenter.x = kMoveLeft;
        else if (positionCenter.x > kMoveLeft + kMoveWidth) positionCenter.x = kMoveLeft + kMoveWidth;
        if (positionCenter.y < kMoveTop) positionCenter.y = kMoveTop;
        else if (positionCenter.y > kMoveTop + kMoveHeight) positionCenter.y = kMoveTop + kMoveHeight;

        if (state == Spawning || state == Invulnerable) {
            if (--invulnerabilityTimer_ <= 0) state = Alive;
        }

        if (in.shoot) {
            if (shotTimer_ % kShotWait == 0) {
                for (Bullet& b : bullets_) {
                    if (b.state != Bullet::Inactive) continue;
                    b = Bullet{};
                    b.state = Bullet::Fired;
                    b.position = positionCenter;
                    b.prevPosition = b.position;
                    sincosmul(b.velocity, -kZunHalfPi, kShotVelocity);
                    b.size = {kShotSize, kShotSize, 0.0f};
                    b.damage = kShotDamage;
                    if (world.anm->hasScript(kScriptPlayerBullet)) world.anm->setAndExecuteScript(b.sprite, kScriptPlayerBullet);
                    world.playSound(kSoundShoot);
                    break;
                }
            }
            ++shotTimer_;
        } else {
            shotTimer_ = 0;
        }
    }
    if (in.bomb) world.unported->hit("bombs");
    hitboxTopLeft = positionCenter - hitboxSize;
    hitboxBottomRight = positionCenter + hitboxSize;

    for (Bullet& b : bullets_) {
        if (b.state == Bullet::Fired) {
            b.position += b.velocity;
            const AnmLoadedSprite* s = world.anm->sprite(b.sprite);
            const float w = s ? s->widthPx : b.size.x, h = s ? s->heightPx : b.size.y;
            if (!world.state->isInBounds(b.position.x, b.position.y, w, h)) {
                b.state = Bullet::Inactive;
                continue;
            }
            world.anm->executeScript(b.sprite);
        } else if (b.state == Bullet::Hit) {
            if (world.anm->executeScript(b.sprite) != 0) b.state = Bullet::Inactive;
        }
    }
    world.anm->executeScript(sprite_);
}

int Player::checkGraze(World& world, const Vec3f& center, const Vec3f& size) {
    const Vec3f tl{center.x - size.x / 2.0f - 20.0f, center.y - size.y / 2.0f - 20.0f, 0};
    const Vec3f br{center.x + size.x / 2.0f + 20.0f, center.y + size.y / 2.0f + 20.0f, 0};
    if (state == Dead || state == Spawning) return GrazeFalse;
    if (!overlap(hitboxTopLeft, hitboxBottomRight, tl, br)) return GrazeFalse;
    ++world.state->grazes;
    world.playSound(kSoundGraze);
    return GrazeTrue;
}

int Player::checkCollision(World& world, const Vec3f& center, const Vec3f& size) {
    const Vec3f tl{center.x - size.x / 2.0f, center.y - size.y / 2.0f, 0};
    const Vec3f br{center.x + size.x / 2.0f, center.y + size.y / 2.0f, 0};
    if (!overlap(hitboxTopLeft, hitboxBottomRight, tl, br)) return CollisionNone;
    if (state != Alive) return CollisionHit;
    die(world);
    return CollisionHit;
}

void Player::die(World& world) {
    state = Dead;
    deathTimer_ = 30;
    ++world.state->deaths;
    world.playSound(kSoundPichun);
}

int Player::calcDamageToEnemy(World& world, const Vec3f& enemyPos, const Vec3f& enemySize, bool* hitByBomb) {
    const Vec3f etl{enemyPos.x - enemySize.x / 2.0f, enemyPos.y - enemySize.y / 2.0f, 0};
    const Vec3f ebr{enemySize.x / 2.0f + enemyPos.x, enemySize.y / 2.0f + enemyPos.y, 0};
    if (hitByBomb) *hitByBomb = false;
    int damage = 0;
    for (Bullet& b : bullets_) {
        if (b.state != Bullet::Fired) continue;
        const Vec3f btl{b.position.x - b.size.x / 2.0f, b.position.y - b.size.y / 2.0f, 0};
        const Vec3f bbr{b.size.x / 2.0f + b.position.x, b.size.y / 2.0f + b.position.y, 0};
        if (btl.y > ebr.y || btl.x > ebr.x || bbr.y < etl.y || bbr.x < etl.x) continue;
        damage += b.damage;
        b.state = Bullet::Hit;
        if (world.anm->hasScript(b.sprite.anmFileIndex + kScriptBulletHitOffset)) {
            world.anm->setAndExecuteScript(b.sprite, b.sprite.anmFileIndex + kScriptBulletHitOffset);
        } else {
            b.state = Bullet::Inactive;
        }
    }
    return damage;
}

void Player::draw(World& world, SpriteSink& sink, float alpha) {
    for (Bullet& b : bullets_) {
        if (b.state == Bullet::Inactive) continue;
        const Vec3f p = lerp(b.prevPosition, b.position, alpha);
        b.sprite.pos = {p.x, p.y, 0.495f};
        world.anm->drawWorld(sink, b.sprite, kGameRegionX, kGameRegionY);
    }
    if (state == Dead) return;
    // Invulnerable: blink every 4 frames.
    if ((state == Spawning || state == Invulnerable) && ((invulnerabilityTimer_ / 4) & 1)) return;
    const Vec3f p = lerp(prevPosition, positionCenter, alpha);
    sprite_.pos = {kGameRegionX + p.x, kGameRegionY + p.y, 0.49f};
    world.anm->drawScreen(sink, sprite_);
}

}  // namespace th06
