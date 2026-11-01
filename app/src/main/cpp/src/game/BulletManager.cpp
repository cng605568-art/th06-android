#include "game/BulletManager.hpp"

#include <cmath>

#include "game/ItemManager.hpp"
#include "game/Player.hpp"

namespace th06 {
namespace {

enum SpawnEffectColor : int { SpawnWhite, SpawnRed, SpawnBlue, SpawnGreen, SpawnYellow };

constexpr int kSpawnEffects16[16] = {SpawnWhite, SpawnRed,   SpawnRed,   SpawnRed,    SpawnRed,    SpawnBlue,
                                     SpawnBlue,  SpawnBlue,  SpawnBlue,  SpawnGreen,  SpawnGreen,  SpawnGreen,
                                     SpawnYellow, SpawnYellow, SpawnYellow, SpawnWhite};
constexpr int kSpawnEffects8[8] = {SpawnWhite, SpawnRed, SpawnRed, SpawnBlue, SpawnBlue, SpawnGreen, SpawnYellow, SpawnWhite};

constexpr float kSizeTiny = 8.0f, kSizeSmall = 16.0f, kSizeLarge = 32.0f;
constexpr int kAnmSpriteSpawnBigBall = kAnmOffsetBullet3 + 140;  // + spawn-effect colour: laser origin glow
constexpr int kLaserGrazeInterval = 12;
constexpr float kLaserCullOffset = 640.0f;
constexpr float kLaserStarSpacing = 32.0f;

// etama3 script ids (+0x200) and etama4 script ids (+0x29a).
enum : int {
    B3Pellet = 0, B3RingBall, B3Rice, B3Ball, B3Kunai, B3Shard, B3BigBall, B3Fireball, B3Dagger, B3LineLaser,
    B3BeamLaser, B3SpawnDonutSmall, B3SpawnDonutMedium, B3SpawnDonutBig, B3SpawnPelletFast, B3SpawnPelletNormal,
    B3SpawnPelletSlow, B3SpawnBigBallFast, B3SpawnBigBallNormal, B3SpawnBigBallSlow, B3SpawnBigBallHuge,
};
enum : int { B4Bubble = 0, B4SpawnBubbleNormal, B4SpawnBubbleSlow };

struct BulletTypeInfo {
    int bullet, fast, normal, slow, donut;
};
constexpr int b3(int s) { return kAnmOffsetBullet3 + s; }
constexpr int b4(int s) { return kAnmOffsetBullet4 + s; }
constexpr BulletTypeInfo kBulletTypeInfos[BulletManager::kBulletTypes] = {
    {b3(B3Pellet), b3(B3SpawnPelletFast), b3(B3SpawnPelletNormal), b3(B3SpawnPelletSlow), b3(B3SpawnDonutSmall)},
    {b3(B3RingBall), b3(B3SpawnBigBallFast), b3(B3SpawnBigBallNormal), b3(B3SpawnBigBallSlow), b3(B3SpawnDonutMedium)},
    {b3(B3Rice), b3(B3SpawnBigBallFast), b3(B3SpawnBigBallNormal), b3(B3SpawnBigBallSlow), b3(B3SpawnDonutMedium)},
    {b3(B3Ball), b3(B3SpawnBigBallFast), b3(B3SpawnBigBallNormal), b3(B3SpawnBigBallSlow), b3(B3SpawnDonutMedium)},
    {b3(B3Kunai), b3(B3SpawnBigBallFast), b3(B3SpawnBigBallNormal), b3(B3SpawnBigBallSlow), b3(B3SpawnDonutMedium)},
    {b3(B3Shard), b3(B3SpawnBigBallFast), b3(B3SpawnBigBallNormal), b3(B3SpawnBigBallSlow), b3(B3SpawnDonutMedium)},
    {b3(B3BigBall), b3(B3SpawnBigBallHuge), b3(B3SpawnBigBallHuge), b3(B3SpawnBigBallHuge), b3(B3SpawnDonutBig)},
    {b3(B3Fireball), b3(B3SpawnBigBallHuge), b3(B3SpawnBigBallHuge), b3(B3SpawnBigBallHuge), b3(B3SpawnDonutBig)},
    {b3(B3Dagger), b3(B3SpawnBigBallHuge), b3(B3SpawnBigBallHuge), b3(B3SpawnBigBallHuge), b3(B3SpawnDonutBig)},
    {b4(B4Bubble), b4(B4SpawnBubbleSlow), b4(B4SpawnBubbleSlow), b4(B4SpawnBubbleSlow), b4(B4SpawnBubbleNormal)},
};

float spriteHeight(const World& w, const AnmVm& vm) {
    const AnmLoadedSprite* s = w.anm->sprite(vm);
    return s ? s->heightPx : 0.0f;
}
float spriteWidth(const World& w, const AnmVm& vm) {
    const AnmLoadedSprite* s = w.anm->sprite(vm);
    return s ? s->widthPx : 0.0f;
}

// Offsets a spawn-effect / donut VM's sprite by the colour, by bullet size class.
void colourEffect(const World& w, AnmVm& effect, const AnmVm& bulletVm, int color) {
    const float h = spriteHeight(w, bulletVm);
    int add;
    if (h <= kSizeSmall) add = kSpawnEffects16[color & 15];
    else if (h <= kSizeLarge) add = bulletVm.anmFileIndex != b3(B3Fireball) ? kSpawnEffects8[color & 7] : SpawnRed;
    else add = color;
    w.anm->setActiveSprite(effect, effect.activeSpriteIndex + add);
}

}  // namespace

bool BulletManager::initializeTemplates(World& world, std::string* error) {
    for (int idx = 0; idx < kBulletTypes; ++idx) {
        const BulletTypeInfo& info = kBulletTypeInfos[idx];
        BulletTypeSprites& t = templates_[static_cast<std::size_t>(idx)];
        if (!world.anm->hasScript(info.bullet)) {
            if (error) *error = "bullet type " + std::to_string(idx) + " has no ANM script";
            return false;
        }
        world.anm->setAndExecuteScript(t.spriteBullet, info.bullet);
        world.anm->setAndExecuteScript(t.spriteSpawnEffectFast, info.fast);
        world.anm->setAndExecuteScript(t.spriteSpawnEffectNormal, info.normal);
        world.anm->setAndExecuteScript(t.spriteSpawnEffectSlow, info.slow);
        world.anm->setAndExecuteScript(t.spriteSpawnEffectDonut, info.donut);
        t.spriteBullet.baseSpriteIndex = t.spriteBullet.activeSpriteIndex;
        const float h = spriteHeight(world, t.spriteBullet);
        t.bulletHeight = static_cast<std::uint8_t>(h);
        float graze;
        if (h <= kSizeTiny) graze = 4.0f;
        else if (h <= kSizeSmall) graze = info.bullet == b3(B3Rice) ? 4.0f : info.bullet == b3(B3Kunai) ? 5.0f
                                              : info.bullet == b3(B3Shard) ? 4.0f : 6.0f;
        else if (h <= kSizeLarge) graze = info.bullet == b3(B3Fireball) ? 11.0f : info.bullet == b3(B3Dagger) ? 9.0f : 16.0f;
        else graze = 32.0f;
        t.grazeSize = {graze, graze, 0.0f};
    }
    return true;
}

bool BulletManager::spawnSingleBullet(World& world, const EnemyShooter& props, int bulletIdx1, int bulletIdx2,
                                      float angle) {
    auto& bullets = *bullets_;
    Bullet* bullet = &bullets[static_cast<std::size_t>(nextBulletIndex_)];
    int idx = 0;
    for (; idx < kMaxBullets; ++idx) {
        nextBulletIndex_++;
        if (nextBulletIndex_ >= kMaxBullets) nextBulletIndex_ = 0;
        if (bullet->state != Inactive) {
            bullet++;
            if (nextBulletIndex_ == 0) bullet = &bullets[0];
            continue;
        }
        break;
    }
    if (idx >= kMaxBullets) {
        ++spawnFailures_;
        return false;
    }

    float bulletAngle = 0.0f;
    float bulletSpeed = props.speed1 - (props.speed1 - props.speed2) * static_cast<float>(bulletIdx2) / props.count2;
    switch (props.aimMode) {
        case FanAimed:
        case Fan:
            if (props.count1 & 1) {
                bulletAngle = static_cast<float>((bulletIdx1 + 1) / 2) * props.angle2 + bulletAngle;
            } else {
                bulletAngle = static_cast<float>(bulletIdx1 / 2) * props.angle2 + props.angle2 * 0.5f + bulletAngle;
            }
            if (bulletIdx1 & 1) bulletAngle *= -1.0f;
            if (props.aimMode == FanAimed) bulletAngle += angle;
            bulletAngle += props.angle1;
            break;
        case CircleAimed:
            bulletAngle += angle;
            [[fallthrough]];
        case Circle:
            bulletAngle += static_cast<float>(bulletIdx1) * kZunTwoPi / props.count1;
            bulletAngle += static_cast<float>(bulletIdx2) * props.angle2 + props.angle1;
            break;
        case OffsetCircleAimed:
            bulletAngle += angle;
            [[fallthrough]];
        case OffsetCircle:
            bulletAngle += kZunPi / props.count1;
            bulletAngle += static_cast<float>(bulletIdx1) * kZunTwoPi / props.count1;
            bulletAngle += props.angle1;
            break;
        case RandomAngle:
            bulletAngle = world.rng->f32InRange(props.angle1 - props.angle2) + props.angle2;
            break;
        case RandomSpeed:
            bulletSpeed = world.rng->f32InRange(props.speed1 - props.speed2) + props.speed2;
            bulletAngle += static_cast<float>(bulletIdx1) * kZunTwoPi / props.count1;
            bulletAngle += static_cast<float>(bulletIdx2) * props.angle2 + props.angle1;
            break;
        case RandomAll:
            bulletAngle = world.rng->f32InRange(props.angle1 - props.angle2) + props.angle2;
            bulletSpeed = world.rng->f32InRange(props.speed1 - props.speed2) + props.speed2;
            break;
        default:
            break;
    }

    const int type = props.sprite >= 0 && props.sprite < kBulletTypes ? props.sprite : 0;
    const BulletTypeSprites& t = templates_[static_cast<std::size_t>(type)];
    bullet->state = Fired;
    bullet->speed = bulletSpeed;
    bullet->angle = addNormalizeAngle(bulletAngle, 0.0f);
    bullet->pos = props.position;
    bullet->pos.z = 0.1f;
    bullet->prevPos = bullet->pos;
    sincosmul(bullet->velocity, bullet->angle, bulletSpeed);  // 1.02 behaviour (normalised angle)
    bullet->exFlags = static_cast<std::uint16_t>(props.flags);
    bullet->color = props.color;
    bullet->sprites.spriteBullet = t.spriteBullet;
    bullet->sprites.spriteSpawnEffectDonut = t.spriteSpawnEffectDonut;
    bullet->sprites.grazeSize = t.grazeSize;
    bullet->sprites.bulletHeight = t.bulletHeight;

    if (bullet->exFlags & ExSpawnEffectShort) {
        bullet->sprites.spriteSpawnEffectFast = t.spriteSpawnEffectFast;
        colourEffect(world, bullet->sprites.spriteSpawnEffectFast, bullet->sprites.spriteBullet, props.color);
        bullet->state = SpawningFast;
    } else if (bullet->exFlags & ExSpawnEffect) {
        bullet->sprites.spriteSpawnEffectNormal = t.spriteSpawnEffectNormal;
        colourEffect(world, bullet->sprites.spriteSpawnEffectNormal, bullet->sprites.spriteBullet, props.color);
        bullet->state = SpawningNormal;
    } else if (bullet->exFlags & ExSpawnEffectLong) {
        bullet->sprites.spriteSpawnEffectSlow = t.spriteSpawnEffectSlow;
        colourEffect(world, bullet->sprites.spriteSpawnEffectSlow, bullet->sprites.spriteBullet, props.color);
        bullet->state = SpawningSlow;
    }
    world.anm->setActiveSprite(bullet->sprites.spriteBullet, bullet->sprites.spriteBullet.activeSpriteIndex + props.color);
    colourEffect(world, bullet->sprites.spriteSpawnEffectDonut, bullet->sprites.spriteBullet, props.color);

    if (bullet->exFlags & ExAcceleration) {
        sincosmul(bullet->ex4Acceleration, props.exFloats[1] <= -999.0f ? bulletAngle : props.exFloats[1], props.exFloats[0]);
        bullet->exDuration = props.exInts[0] > 0 ? props.exInts[0] : 99999;
        bullet->ex4Acceleration.z = 0.0f;
    } else if (bullet->exFlags & ExVelocity) {
        bullet->exVelSpeed = props.exFloats[0];
        bullet->exVelAngle = props.exFloats[1];
        bullet->exDuration = props.exInts[0];
    }
    if (bullet->exFlags & (ExAngleAdd | ExAnglePlayer | ExAngleSet)) {
        bullet->dirChangeRotation = props.exFloats[0];
        bullet->dirChangeSpeed = props.exFloats[1] >= 0.0f ? props.exFloats[1] : bulletSpeed;
        bullet->dirChangeInterval = props.exInts[0];
        bullet->dirChangeMaxTimes = props.exInts[1];
        bullet->dirChangeNumTimes = 0;
    }
    if (bullet->exFlags & (ExBounceTblr | ExBounceTlr)) {
        bullet->dirChangeSpeed = props.exFloats[0] >= 0.0f ? props.exFloats[0] : bulletSpeed;
        bullet->dirChangeMaxTimes = props.exInts[0];
        bullet->dirChangeNumTimes = 0;
    }
    return true;
}

void BulletManager::spawnBulletPattern(World& world, const EnemyShooter& props) {
    const float angle = world.player->angleToPlayer(props.position);
    for (int idx1 = 0; idx1 < props.count2; ++idx1) {
        for (int idx2 = 0; idx2 < props.count1; ++idx2) {
            if (!spawnSingleBullet(world, props, idx2, idx1, angle)) goto out;
        }
    }
out:
    if (props.flags & ExSpawnSound) world.playSound(props.sfx);
}

void BulletManager::update(World& world) {
    if (world.items) world.items->update(world);
    int count = 0;
    for (Bullet& b : *bullets_) {
        if (b.state == Inactive) continue;
        ++count;
        b.prevPos = b.pos;
        float bulletSpeed;
        switch (b.state) {
            case SpawningFast:
                b.pos += b.velocity / 2.0f;
                if (world.anm->executeScript(b.sprites.spriteSpawnEffectFast) == 0) break;
                goto fired;
            case SpawningNormal:
                b.pos += b.velocity / 2.5f;
                if (world.anm->executeScript(b.sprites.spriteSpawnEffectNormal) == 0) break;
                goto fired;
            case SpawningSlow:
                b.pos += b.velocity / 3.0f;
                if (world.anm->executeScript(b.sprites.spriteSpawnEffectSlow) == 0) break;
            fired:
                b.state = Fired;
                b.timer = 0;
                [[fallthrough]];
            case Fired: {
                if (b.exFlags != 0) {
                    if (b.exFlags & ExSpeedup) {
                        if (b.timer <= 16) {
                            bulletSpeed = 5.0f - static_cast<float>(b.timer) * 5.0f / 16.0f;
                            sincosmul(b.velocity, b.angle, bulletSpeed + b.speed);
                        } else {
                            b.exFlags ^= ExSpeedup;
                        }
                    } else if (b.exFlags & ExAcceleration) {
                        if (b.timer >= b.exDuration) {
                            b.exFlags &= ~ExAcceleration;
                        } else {
                            b.velocity += b.ex4Acceleration;
                            b.angle = std::atan2(b.velocity.y, b.velocity.x);
                        }
                    } else if (b.exFlags & ExVelocity) {
                        if (b.timer >= b.exDuration) {
                            b.exFlags &= ~ExVelocity;
                        } else {
                            b.angle = addNormalizeAngle(b.angle, b.exVelAngle);
                            b.speed += b.exVelSpeed;
                            sincosmul(b.velocity, b.angle, b.speed);
                        }
                    }
                    auto dirChange = [&](std::uint16_t flag, auto newAngle) {
                        if (b.timer >= b.dirChangeInterval * (b.dirChangeNumTimes + 1)) {
                            if (++b.dirChangeNumTimes >= b.dirChangeMaxTimes) b.exFlags &= ~flag;
                            b.angle = newAngle();
                            b.speed = b.dirChangeSpeed;
                            bulletSpeed = b.speed;
                        } else {
                            bulletSpeed = b.speed - ((static_cast<float>(b.timer) -
                                                      static_cast<float>(b.dirChangeInterval * b.dirChangeNumTimes)) *
                                                     b.speed) / static_cast<float>(b.dirChangeInterval);
                        }
                        sincosmul(b.velocity, b.angle, bulletSpeed);
                    };
                    const float w = spriteWidth(world, b.sprites.spriteBullet);
                    const float h = spriteHeight(world, b.sprites.spriteBullet);
                    if (b.exFlags & ExAngleAdd) {
                        dirChange(ExAngleAdd, [&] { return b.angle + b.dirChangeRotation; });
                    } else if (b.exFlags & ExAngleSet) {
                        dirChange(ExAngleSet, [&] { return b.dirChangeRotation; });
                    } else if (b.exFlags & ExAnglePlayer) {
                        dirChange(ExAnglePlayer, [&] { return world.player->angleToPlayer(b.pos) + b.dirChangeRotation; });
                    } else if (b.exFlags & (ExBounceTblr | ExBounceTlr)) {
                        const bool all = (b.exFlags & ExBounceTblr) != 0;
                        if (!world.state->isInBounds(b.pos.x, b.pos.y, w, h)) {
                            if (b.pos.x < 0.0f || b.pos.x >= kGameRegionWidth) {
                                b.angle = addNormalizeAngle(-b.angle - kZunPi, 0.0f);
                            }
                            if (b.pos.y < 0.0f || (all && b.pos.y >= kGameRegionHeight)) b.angle = -b.angle;
                            b.speed = b.dirChangeSpeed;
                            sincosmul(b.velocity, b.angle, b.speed);
                            if (++b.dirChangeNumTimes >= b.dirChangeMaxTimes) {
                                b.exFlags &= all ? ~ExBounceTblr : ~ExBounceTlr;
                            }
                        }
                    }
                }

                b.pos += b.velocity;
                const float w = spriteWidth(world, b.sprites.spriteBullet);
                const float h = spriteHeight(world, b.sprites.spriteBullet);
                if (!world.state->isInBounds(b.pos.x, b.pos.y, w, h)) {
                    const bool mayReturn = (b.exFlags & (ExAngleAdd | ExAngleSet | ExAnglePlayer | ExBounceTblr | ExBounceTlr)) != 0;
                    if (!mayReturn && b.outOfBoundsTime == 0) {
                        b = Bullet{};
                        continue;
                    }
                    if (++b.outOfBoundsTime >= 256) {
                        b = Bullet{};
                        continue;
                    }
                } else {
                    b.outOfBoundsTime = 0;
                }

                bool checkHit = b.isGrazed != 0;
                if (!b.isGrazed) {
                    const int graze = world.player->checkGraze(world, b.pos, b.sprites.grazeSize);
                    if (graze == Player::GrazeTrue) {
                        b.isGrazed = 1;
                        checkHit = true;
                    } else if (graze == Player::CanceledByBomb) {
                        b.state = Despawning;
                        if (world.items) world.items->spawn(world, b.pos, ItemStar, ItemState::Magnet);
                    }
                }
                if (checkHit) {
                    const int hit = world.player->checkCollision(world, b.pos, b.sprites.grazeSize);
                    if (hit != Player::CollisionNone) b.state = Despawning;
                }
                world.anm->executeScript(b.sprites.spriteBullet);
                break;
            }
            case Despawning:
                b.pos += b.velocity / 2.0f;
                if (world.anm->executeScript(b.sprites.spriteSpawnEffectDonut) != 0) {
                    b = Bullet{};
                    continue;
                }
                break;
            default:
                break;
        }
        b.timer++;
    }
    if (count > peak_) peak_ = count;
    updateLasers(world);
}

void BulletManager::draw(World& world, SpriteSink& sink, float alpha) {
    // Lasers: beam, then the glow at its origin while the tail is near it.
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        const float start = l.prevStartOffset + (l.startOffset - l.prevStartOffset) * alpha;
        const float end = l.prevEndOffset + (l.endOffset - l.prevEndOffset) * alpha;
        const float c = std::cos(l.angle), sn = std::sin(l.angle);
        const float mid = start + (end - start) / 2.0f;
        l.vm.pos = {l.pos.x + c * mid, l.pos.y + sn * mid, 0.0f};
        world.anm->drawWorld(sink, l.vm, kGameRegionX, kGameRegionY);
        if (start < 16.0f || l.speed == 0.0f) {
            AnmVm& g = l.glowVm;
            g.pos = {l.pos.x + c * start, l.pos.y + sn * start, 0.0f};
            g.color = (l.vm.color & 0x00ffffffu) | 0xff000000u;
            g.colorAdd = true;
            g.additive = true;
            g.rotation = {};
            g.anchor = AnmAnchor::Center;
            float scale = l.width / 10.0f * (16.0f - start) / 16.0f;
            if (scale < 0.0f) scale = l.width / 10.0f;
            g.scaleX = g.scaleY = scale;
            world.anm->drawWorld(sink, g, kGameRegionX, kGameRegionY);
        }
    }

    if (world.items) world.items->draw(world, sink, alpha);

    // Bullets in four passes so small bullets end up on top of big ones: taller than 16 px;
    // 16 px balls and ring balls; other 16 px bullets; smaller ones.
    auto passOf = [](const Bullet& b) {
        const int h = b.sprites.bulletHeight;
        if (h > 16) return 0;
        if (h == 16) {
            const int script = b.sprites.spriteBullet.anmFileIndex;
            return (script == b3(B3RingBall) || script == b3(B3Ball)) ? 1 : 2;
        }
        return 3;
    };
    for (int pass = 0; pass < 4; ++pass) {
        for (Bullet& b : *bullets_) {
            if (b.state == Inactive || passOf(b) != pass) continue;
            AnmVm* vm;
            switch (b.state) {
                case SpawningFast: vm = &b.sprites.spriteSpawnEffectFast; break;
                case SpawningNormal: vm = &b.sprites.spriteSpawnEffectNormal; break;
                case SpawningSlow: vm = &b.sprites.spriteSpawnEffectSlow; break;
                case Despawning: vm = &b.sprites.spriteSpawnEffectDonut; break;
                default: vm = &b.sprites.spriteBullet; break;
            }
            const Vec3f p = lerp(b.prevPos, b.pos, alpha);
            vm->pos = {p.x, p.y, 0.0f};
            vm->color = colorCombineAlpha(0xffffffffu, vm->color);
            if (vm->autoRotate != 0) vm->rotation.z = kZunHalfPi - b.angle;
            world.anm->drawWorld(sink, *vm, kGameRegionX, kGameRegionY);
        }
    }
}

void BulletManager::laserStars(World& world, const Laser& l) {
    if (!world.items) return;
    const float c = std::cos(l.angle), sn = std::sin(l.angle);
    for (float d = l.startOffset; d < l.endOffset; d += kLaserStarSpacing) {
        world.items->spawn(world, {l.pos.x + c * d, l.pos.y + sn * d, l.pos.z}, ItemStar, ItemState::Magnet);
    }
}

void BulletManager::removeAllBullets(World& world, bool convert) {
    for (Bullet& b : *bullets_) {
        if (b.state == Inactive || b.state == Despawning) continue;
        if (convert) {
            if (world.items) world.items->spawn(world, b.pos, ItemStar, ItemState::Magnet);
            b = Bullet{};
        } else {
            b.state = Despawning;
        }
    }
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        if (l.state < LaserDespawning) {
            l.state = LaserDespawning;
            l.timer = 0;
            if (convert) laserStars(world, l);
        }
    }
    for (Laser& l : lasers_) {
        if (l.inUse) l.hitboxEndDelay = 0;
    }
}

std::int32_t BulletManager::despawnBullets(World& world, std::int32_t maxBonusScore, bool awardPoints) {
    std::int32_t total = 0;
    std::int32_t bulletScore = 2000;
    for (Bullet& b : *bullets_) {
        if (b.state == Inactive) continue;
        if (awardPoints && world.items) world.items->spawn(world, b.pos, ItemStar, ItemState::Magnet);
        total += bulletScore;
        bulletScore += 10;
        if (bulletScore > maxBonusScore) bulletScore = maxBonusScore;
        b.state = Despawning;
    }
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        if (l.state < LaserDespawning) {
            l.state = LaserDespawning;
            l.timer = 0;
            if (awardPoints) {
                if (world.items) world.items->spawn(world, l.pos, ItemStar, ItemState::Magnet);
                laserStars(world, l);
            }
        }
    }
    for (Laser& l : lasers_) {
        if (l.inUse) l.hitboxEndDelay = 0;
    }
    world.state->score += static_cast<std::uint32_t>(total);
    if (total != 0) world.unported->hit("score popups and banners");
    return total;
}

int BulletManager::spawnLaser(World& world, const LaserProps& p) {
    for (int i = 0; i < kMaxLasers; ++i) {
        Laser& l = lasers_[static_cast<std::size_t>(i)];
        if (l.inUse) continue;
        world.anm->setAndExecuteScript(l.vm, kAnmScriptBullet3LineLaser + p.sprite);
        world.anm->setActiveSprite(l.vm, l.vm.activeSpriteIndex + p.color);
        world.anm->initializeAndSetSprite(l.glowVm, kAnmSpriteSpawnBigBall + kSpawnEffects16[p.color & 15]);
        l.glowVm.additive = true;
        l.pos = p.position;
        l.color = p.color;
        l.inUse = true;
        l.angle = p.angle;
        if (p.aimed) l.angle += world.player->angleToPlayer(p.position);
        l.flags = p.flags;
        l.timer = 0;
        l.startOffset = l.prevStartOffset = p.startOffset;
        l.endOffset = l.prevEndOffset = p.endOffset;
        l.startLength = p.startLength;
        l.width = p.width;
        l.speed = p.speed;
        l.startTime = p.startTime;
        l.duration = p.duration;
        l.despawnDuration = p.despawnDuration;
        l.hitboxStartTime = p.hitboxStartTime;
        l.hitboxEndDelay = p.hitboxEndDelay;
        l.state = p.startTime == 0 ? LaserActive : LaserStartDelay;
        return i;
    }
    return -1;
}

void BulletManager::updateLasers(World& world) {
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        l.prevStartOffset = l.startOffset;
        l.prevEndOffset = l.endOffset;

        l.endOffset += l.speed;
        if (l.endOffset - l.startOffset > l.startLength) l.startOffset = l.endOffset - l.startLength;
        if (l.startOffset < 0.0f) l.startOffset = 0.0f;

        // Hitbox in the laser's own frame: along +x from the origin, half the visual width thick.
        Vec3f center{l.pos.x + l.startOffset + (l.endOffset - l.startOffset) / 2.0f, l.pos.y, 0.0f};
        Vec3f size{l.endOffset - l.startOffset, l.width / 2.0f, 0.0f};
        const AnmLoadedSprite* sprite = world.anm->sprite(l.vm);
        const float spriteW = sprite ? sprite->widthPx : 16.0f;
        const float spriteH = sprite ? sprite->heightPx : 16.0f;
        l.vm.scaleX = l.width / spriteW;
        l.vm.scaleY = (l.endOffset - l.startOffset) / spriteH;
        l.vm.rotation.z = kZunHalfPi - l.angle;

        auto test = [&] {
            world.player->checkLaser(world, center, size, l.pos, l.angle, l.timer % kLaserGrazeInterval == 0);
        };
        auto fadeAlpha = [&] {
            if (l.startTime == 0) return 255;
            const int a = static_cast<int>(static_cast<float>(l.timer) * 255.0f / static_cast<float>(l.startTime));
            return a > 255 ? 255 : a;
        };
        // The thinning beam during warning and despawn: note the original stores the half
        // width into the hitbox *length*, shrinking the hitbox to a speck at the midpoint.
        auto setThickness = [&](float w) {
            l.vm.scaleX = w / 16.0f;
            size.x = w / 2.0f;
        };

        bool tick = false;  // reached the end of the state machine without freeing the laser
        if (l.state == LaserStartDelay) {
            if (l.flags & kLaserFlagFadeInOut) {
                l.vm.color = static_cast<std::uint32_t>(fadeAlpha()) << 24;
            } else {
                const std::int32_t ramp = std::min(l.startTime, 30);
                setThickness(l.timer > l.startTime - ramp
                                 ? static_cast<float>(l.timer) * l.width / static_cast<float>(l.startTime)
                                 : 1.2f);
            }
            if (l.timer >= l.hitboxStartTime) test();
            if (l.timer < l.startTime) {
                tick = true;
            } else {
                l.timer = 0;
                l.state = LaserActive;
            }
        }
        if (!tick && l.state == LaserActive) {
            test();
            if (l.timer < l.duration) {
                tick = true;
            } else {
                l.timer = 0;
                l.state = LaserDespawning;
                if (l.despawnDuration == 0) {
                    l.inUse = false;
                    continue;
                }
            }
        }
        if (!tick && l.state == LaserDespawning) {
            if (l.flags & kLaserFlagFadeInOut) {
                l.vm.color = static_cast<std::uint32_t>(fadeAlpha()) << 24;
            } else if (l.despawnDuration > 0) {
                setThickness(l.width - static_cast<float>(l.timer) * l.width / static_cast<float>(l.despawnDuration));
            }
            if (l.timer < l.hitboxEndDelay) test();
            if (l.timer >= l.despawnDuration) {
                l.inUse = false;
                continue;
            }
        }
        if (l.startOffset >= kLaserCullOffset) l.inUse = false;  // still ticks this frame
        ++l.timer;
        world.anm->executeScript(l.vm);
    }
}

int BulletManager::activeLaserCount() const {
    int n = 0;
    for (const Laser& l : lasers_) n += l.inUse;
    return n;
}

int BulletManager::activeCount() const {
    int n = 0;
    for (const Bullet& b : *bullets_) n += b.state != Inactive;
    return n;
}

}  // namespace th06
