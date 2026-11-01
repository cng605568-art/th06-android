#include "game/Player.hpp"

#include <algorithm>
#include <cmath>

#include "game/BulletManager.hpp"
#include "game/EffectManager.hpp"
#include "game/EnemyManager.hpp"
#include "game/Gui.hpp"
#include "game/ItemManager.hpp"
#include "game/Resources.hpp"

namespace th06 {
namespace {

// player00.anm / player01.anm script roles (+0x400).
constexpr int kScriptIdle = kAnmOffsetPlayer + 0;
constexpr int kScriptMovingLeft = kAnmOffsetPlayer + 1;
constexpr int kScriptStoppingLeft = kAnmOffsetPlayer + 2;
constexpr int kScriptMovingRight = kAnmOffsetPlayer + 3;
constexpr int kScriptStoppingRight = kAnmOffsetPlayer + 4;
constexpr int kScriptMainShot = kAnmOffsetPlayer + 64;
constexpr int kScriptOrbLeft = kAnmOffsetPlayer + 128;
constexpr int kScriptOrbRight = kAnmOffsetPlayer + 129;
constexpr int kScriptHitOffset = 32;

constexpr float kMoveLeft = 8.0f, kMoveTop = 16.0f;
constexpr float kMoveWidth = kGameRegionWidth - 16.0f, kMoveHeight = kGameRegionHeight - 32.0f;
constexpr int kSoundShoot = 0, kSoundPichun = 4;
constexpr int kOrbPowerMin = 8;
constexpr int kOrbTransition = 8;
constexpr float kOrbX = 24.0f, kOrbFocusX = 8.0f, kOrbFocusY = -32.0f;
constexpr int kHomingSteerFrames = 40;
constexpr float kHomingMaxSpeed = 10.0f;
constexpr float kMissileLift = 0.3f;
constexpr int kDamageCap = 70;

// Without the game executable: one straight shot every 5 frames.
const ShotType& basicShotType(int shotType) {
    static const auto make = [](float speed, float focus) {
        ShotType t;
        t.speed = speed;
        t.speedFocus = focus;
        ShotEntry e;
        e.width = e.height = 12.0f;
        e.angle = -kZunHalfPi;
        e.speed = 12.0f;
        e.damage = 48;
        e.every = 5;
        e.script = kScriptMainShot;
        e.sound = true;
        const std::int32_t thresholds[9] = {8, 16, 32, 48, 64, 80, 96, 127, 999};
        for (int i = 0; i < 9; ++i) {
            t.tiers[static_cast<std::size_t>(i)].threshold = thresholds[i];
            t.tiers[static_cast<std::size_t>(i)].entries = {e};
        }
        return t;
    };
    static const ShotType reimu = make(4.0f, 2.0f), marisa = make(5.0f, 2.5f);
    return shotType >= 2 ? marisa : reimu;
}

bool overlap(const Vec3f& aTL, const Vec3f& aBR, const Vec3f& bTL, const Vec3f& bBR) {
    return !(aTL.x > bBR.x || aBR.x < bTL.x || aTL.y > bBR.y || aBR.y < bTL.y);
}

float length2d(const Vec3f& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Missiles grow their hitbox on first contact (by the script they were fired with).
float missileBlastSize(int localScript) {
    switch (localScript) {
        case 65: return 32.0f;
        case 66: return 42.0f;
        default: return 48.0f;
    }
}

}  // namespace

const ShotType& Player::shotType(const World& world) const {
    const int type = world.state->shotType;
    if (world.shots && world.shots->loaded) return world.shots->types[static_cast<std::size_t>(type & 3)];
    return basicShotType(type);
}

int Player::shotTier(const World& world) const {
    const ShotType& t = shotType(world);
    int tier = 0;
    while (tier < 8 && world.state->power >= t.tiers[static_cast<std::size_t>(tier)].threshold) ++tier;
    return tier;
}

const ShotTier& Player::tierFor(const World& world) const {
    return shotType(world).tiers[static_cast<std::size_t>(shotTier(world))];
}

void Player::reset(World& world) {
    positionCenter = {kGameRegionWidth / 2.0f, kGameRegionHeight - 64.0f, 0.49f};
    prevPosition = positionCenter;
    state = Spawning;
    invulnerabilityTimer_ = 0;
    previousSpeedX_ = 0.0f;
    bullets_ = {};
    fireTimer_ = -1;
    laserTimers_ = {};
    orbState_ = OrbState::Hidden;
    orbCounter_ = 0;
    orbPositions_ = {positionCenter, positionCenter};
    homingTarget_ = {-999.0f, -999.0f, 0.0f};
    bulletGrace_ = 0;
    respawnCountdown_ = 8;
    previousBombInput_ = false;
    bomb_.reset();
    world.state->bombActive = false;
    world.anm->setAndExecuteScript(sprite_, kScriptIdle);
    world.anm->setAndExecuteScript(orbVms_[0], kScriptOrbLeft);
    world.anm->setAndExecuteScript(orbVms_[1], kScriptOrbRight);
    hitboxTopLeft = positionCenter - hitboxSize;
    hitboxBottomRight = positionCenter + hitboxSize;
}

float Player::angleToPlayer(const Vec3f& pos) const {
    const float relX = positionCenter.x - pos.x;
    const float relY = positionCenter.y - pos.y;
    if (relY == 0.0f && relX == 0.0f) return kZunHalfPi;  // straight down
    return std::atan2(relY, relX);
}

int Player::activeBulletCount() const {
    int n = 0;
    for (const Bullet& b : bullets_) n += b.state != ShotState::Inactive;
    return n;
}

void Player::update(World& world, const InputState& in) {
    prevPosition = positionCenter;
    for (Bullet& b : bullets_) b.prevPosition = b.position;
    GameState& gs = *world.state;
    // Time stop freezes the whole player: no input, shots, bomb progress, death sequence or
    // invulnerability countdown. The bomb's damage regions stay live (they are not cleared).
    if (gs.timeStop != 0) {
        bomb_.holdFrame();
        return;
    }

    // Bomb: run it, or start one on a fresh press (also inside the deathbomb window).
    bomb_.clearBoxes();
    const bool talking = world.gui && world.gui->msgActive();  // no shooting or bombing during dialogue
    const bool bombPressed = in.bomb && !previousBombInput_;
    previousBombInput_ = in.bomb;
    if (gs.bombActive) {
        bomb_.step(world, *this);
    } else if (respawnCountdown_ != 0 && gs.bombs > 0 && bombPressed && !talking) {
        bomb_.start(world, *this);
    }

    // Death, respawn and invulnerability.
    if (state == Dead) {
        if (respawnCountdown_ != 0 && --respawnCountdown_ == 0) dropItemsOnDeath(world);
        if (--deathTimer_ <= 0) respawn(world);
    }
    if (state == Spawning) {
        state = Invulnerable;
        invulnerabilityTimer_ = 240;
        respawnCountdown_ = 6;
        bomb_.resetMoveMultiplier();
    } else if (state == Invulnerable) {
        if (--invulnerabilityTimer_ <= 0) {
            state = Alive;
            invulnerabilityTimer_ = 0;
            sprite_.colorAdd = false;
            sprite_.color = 0xffffffffu;
        } else if (invulnerabilityTimer_ % 8 < 2) {  // blink
            sprite_.colorAdd = true;
            sprite_.color = 0xff404040u;
        } else {
            sprite_.colorAdd = false;
            sprite_.color = 0xffffffffu;
        }
    }
    if (bulletGrace_ > 0 && state != Dead) {
        --bulletGrace_;
        world.bullets->removeAllBullets(world, false);
    }

    if (state == Alive || state == Invulnerable) {
        move(world, in);
        if (in.shoot && fireTimer_ < 0 && !talking) fireTimer_ = 0;
    }
    world.anm->executeScript(sprite_);
    updateShots(world);
    if (orbState_ != OrbState::Hidden) {
        world.anm->executeScript(orbVms_[0]);
        world.anm->executeScript(orbVms_[1]);
    }
    homingTarget_ = {-999.0f, -999.0f, 0.0f};

    if (fireTimer_ >= 0) {
        spawnShots(world, fireTimer_);
        ++fireTimer_;
        if (fireTimer_ >= kFireCycle || state == Dead || state == Spawning) fireTimer_ = -1;
    }
}

void Player::move(World& world, const InputState& in) {
    focused = in.focus;
    const int dx = in.right ? 1 : in.left ? -1 : 0;
    const int dy = in.up ? -1 : in.down ? 1 : 0;
    const ShotType& type = shotType(world);
    const float ortho = (focused ? type.speedFocus : type.speed) * bomb_.moveMultiplier();
    const float diag = ortho / std::sqrt(2.0f);
    float speedX, speedY;
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
    positionCenter.x = std::clamp(positionCenter.x, kMoveLeft, kMoveLeft + kMoveWidth);
    positionCenter.y = std::clamp(positionCenter.y, kMoveTop, kMoveTop + kMoveHeight);
    hitboxTopLeft = positionCenter - hitboxSize;
    hitboxBottomRight = positionCenter + hitboxSize;

    if (world.state->power < kOrbPowerMin) orbState_ = OrbState::Hidden;
    else if (orbState_ == OrbState::Hidden) orbState_ = OrbState::Unfocused;
    updateOrbs();
}

void Player::updateOrbs() {
    float x = 0.0f, y = 0.0f;
    // Each state either settles or hands over to the opposite transition within the frame.
    for (int guard = 0; guard < 4; ++guard) {
        bool again = false;
        switch (orbState_) {
            case OrbState::Hidden:
                orbCounter_ = 0;
                x = y = 0.0f;
                break;
            case OrbState::Unfocused:
                x = kOrbX;
                y = 0.0f;
                orbCounter_ = 0;
                if (focused) {
                    orbState_ = OrbState::Focusing;
                    again = true;
                }
                break;
            case OrbState::Focused:
                x = kOrbFocusX;
                y = kOrbFocusY;
                orbCounter_ = 0;
                if (!focused) {
                    orbState_ = OrbState::Unfocusing;
                    again = true;
                }
                break;
            case OrbState::Focusing: {
                ++orbCounter_;
                const float s = static_cast<float>(orbCounter_) / kOrbTransition;
                x = kOrbX - (kOrbX - kOrbFocusX) * s * s;
                y = kOrbFocusY * s;
                if (orbCounter_ >= kOrbTransition) orbState_ = OrbState::Focused;
                if (!focused) {
                    orbState_ = OrbState::Unfocusing;
                    orbCounter_ = kOrbTransition - orbCounter_;
                    again = true;
                }
                break;
            }
            case OrbState::Unfocusing: {
                ++orbCounter_;
                const float u = static_cast<float>(orbCounter_) / kOrbTransition;
                x = kOrbFocusX + (kOrbX - kOrbFocusX) * u * u;
                y = kOrbFocusY - kOrbFocusY * u;
                if (orbCounter_ >= kOrbTransition) orbState_ = OrbState::Unfocused;
                if (focused) {
                    orbState_ = OrbState::Focusing;
                    orbCounter_ = kOrbTransition - orbCounter_;
                    again = true;
                }
                break;
            }
        }
        if (!again) break;
    }
    orbPositions_[0] = {positionCenter.x - x, positionCenter.y + y, positionCenter.z};
    orbPositions_[1] = {positionCenter.x + x, positionCenter.y + y, positionCenter.z};
}

void Player::updateShots(World& world) {
    for (int& t : laserTimers_) {
        if (t != 0) --t;
    }
    for (Bullet& b : bullets_) {
        if (b.state == ShotState::Inactive) continue;
        switch (b.kind) {
            case ShotHoming:
                if (b.state != ShotState::Fired) break;
                if (homingTarget_.x > -100.0f && b.lifetime < kHomingSteerFrames) {
                    Vec3f d = homingTarget_ - b.position;
                    d.z = 0.0f;
                    float l = length2d(d) / (b.speed / 4.0f);
                    if (l < 1.0f) l = 1.0f;
                    Vec3f v = b.velocity + d / l;
                    v.z = 0.0f;
                    const float len = length2d(v);
                    float speed = std::min(len, kHomingMaxSpeed);
                    if (speed < 1.0f) speed = 1.0f;
                    if (len > 0.0f) b.velocity = v * (speed / len);
                    b.speed = speed;
                } else if (b.speed < kHomingMaxSpeed) {
                    const float len = length2d(b.velocity);
                    b.speed += 1.0f / 3.0f;
                    if (len > 0.0f) b.velocity = b.velocity * (b.speed / len);
                }
                break;
            case ShotMissile:
                if (b.state == ShotState::Fired) b.velocity.y -= kMissileLift;
                break;
            case ShotLaser: {
                const int timer = laserTimers_[static_cast<std::size_t>(b.laserTimer & 1)];
                if (timer == 70 || timer == 1) b.sprite.pendingInterrupt = 1;
                const Vec3f& o = orbPositions_[static_cast<std::size_t>(b.orb & 1)];
                b.position.x = o.x + b.offsetX;
                b.position.y = o.y / 2.0f;
                b.sprite.scaleY = o.y / 14.0f;
                b.size.y = o.y;
                break;
            }
            default:
                break;
        }
        b.position += b.velocity;
        if (b.kind != ShotLaser) {
            const AnmLoadedSprite* s = world.anm->sprite(b.sprite);
            const float w = s ? s->widthPx : b.size.x, h = s ? s->heightPx : b.size.y;
            if (!world.state->isInBounds(b.position.x, b.position.y, w, h)) {
                b.state = ShotState::Inactive;
                continue;
            }
        }
        if (world.anm->executeScript(b.sprite) != 0) {
            b.state = ShotState::Inactive;
            continue;
        }
        ++b.lifetime;
    }
}

void Player::fire(World& world, Bullet& b, const ShotEntry& e) {
    b = Bullet{};
    world.anm->setAndExecuteScript(b.sprite, e.script);
    const Vec3f origin = e.origin == ShotFromPlayer ? positionCenter : orbPositions_[e.origin == ShotFromLeftOrb ? 0 : 1];
    b.position = {origin.x + e.offsetX, origin.y + e.offsetY, 0.495f};
    b.prevPosition = b.position;
    b.size = {e.width, e.height, 0.0f};
    b.angle = e.angle;
    b.speed = e.speed;
    b.velocity = {std::cos(e.angle) * e.speed, std::sin(e.angle) * e.speed, 0.0f};
    b.lifetime = 0;
    b.kind = e.kind;
    b.damage = e.damage;
    b.script = e.script;
    if (e.kind == ShotLaser) {
        b.laserTimer = static_cast<std::int8_t>(e.phase & 1);
        b.orb = static_cast<std::int8_t>(e.origin == ShotFromRightOrb ? 1 : 0);
        b.offsetX = e.offsetX;
        laserTimers_[static_cast<std::size_t>(e.phase & 1)] = e.every;
    }
    if (e.sound) world.playSound(kSoundShoot);
    b.state = ShotState::Fired;
}

void Player::spawnShots(World& world, int t) {
    if (world.state->bombActive && world.state->shotType == 3) return;  // Marisa B cannot shoot while bombing
    const std::vector<ShotEntry>& entries = tierFor(world).entries;
    std::size_t e = 0;
    int slot = 0;
    while (e < entries.size()) {
        while (slot < kMaxBullets && bullets_[static_cast<std::size_t>(slot)].state != ShotState::Inactive) ++slot;
        if (slot >= kMaxBullets) return;  // pool exhausted: the remaining entries wait
        // Try entries against this free slot until one fires.
        for (; e < entries.size(); ++e) {
            const ShotEntry& entry = entries[e];
            bool fires;
            if (entry.kind == ShotLaser) fires = laserTimers_[static_cast<std::size_t>(entry.phase & 1)] == 0;
            else fires = t % entry.every == entry.phase;
            if (!fires) continue;
            fire(world, bullets_[static_cast<std::size_t>(slot)], entry);
            ++e;
            break;
        }
    }
}

int Player::checkGraze(World& world, const Vec3f& center, const Vec3f& size) {
    const Vec3f tl{center.x - size.x / 2.0f - 20.0f, center.y - size.y / 2.0f - 20.0f, 0};
    const Vec3f br{center.x + size.x / 2.0f + 20.0f, center.y + size.y / 2.0f + 20.0f, 0};
    if (bomb_.cancels(tl, br)) return CanceledByBomb;
    if (state == Dead || state == Spawning) return GrazeFalse;
    if (!overlap(hitboxTopLeft, hitboxBottomRight, tl, br)) return GrazeFalse;
    addGraze(world, (positionCenter + center) / 2.0f);
    return GrazeTrue;
}

int Player::checkCollision(World& world, const Vec3f& center, const Vec3f& size) {
    const Vec3f tl{center.x - size.x / 2.0f, center.y - size.y / 2.0f, 0};
    const Vec3f br{center.x + size.x / 2.0f, center.y + size.y / 2.0f, 0};
    if (bomb_.cancels(tl, br)) return CollisionCanceledByBomb;
    if (!overlap(hitboxTopLeft, hitboxBottomRight, tl, br)) return CollisionNone;
    if (state != Alive) return CollisionHit;
    die(world);
    return CollisionHit;
}

void Player::die(World& world) {
    state = Dead;
    if (world.enemies) world.enemies->failSpellcardCapture();
    deathTimer_ = 30;
    invulnerabilityTimer_ = 0;
    laserTimers_ = {2, 2};  // ends both lasers
    ++world.state->deaths;
    spawnEffect(world, EffectFlash, positionCenter, 1, 0xff4040ffu);
    spawnEffect(world, EffectParticleGreen, positionCenter, 16);
    world.playSound(kSoundPichun);
}

void Player::dropItemsOnDeath(World& world) {
    GameState& gs = *world.state;
    gs.fullPowerChain = 0;
    if (world.items) {
        if (gs.livesRemaining > 0) {
            world.items->spawn(world, positionCenter, ItemPowerBig, ItemState::DeathScatter);
            for (int i = 0; i < 5; ++i) world.items->spawn(world, positionCenter, ItemPowerSmall, ItemState::DeathScatter);
        } else {
            for (int i = 0; i < 5; ++i) world.items->spawn(world, positionCenter, ItemFullPower, ItemState::DeathScatter);
        }
    }
    if (gs.livesRemaining > 0) {
        gs.power = std::max(gs.power - 16, 0);
    } else {
        gs.power = 0;
        gs.extendIndex = -1;
    }
    gs.decreaseSubrank(1600);
}

void Player::respawn(World& world) {
    GameState& gs = *world.state;
    if (--gs.livesRemaining < 0) {
        // Game over: the continue screen is not ported, so play simply goes on.
        world.unported->hit("game over / continue screen");
        gs.livesRemaining = 2;
        gs.extendIndex = 0;
    }
    gs.bombs = gs.startingBombs;
    positionCenter = {kGameRegionWidth / 2.0f, kGameRegionHeight - 64.0f, 0.49f};
    prevPosition = positionCenter;
    hitboxTopLeft = positionCenter - hitboxSize;
    hitboxBottomRight = positionCenter + hitboxSize;
    state = Spawning;
    respawnCountdown_ = 0;  // no bombs while spawning
    bomb_.resetMoveMultiplier();
    bulletGrace_ = 90;
}

void Player::checkLaser(World& world, const Vec3f& center, const Vec3f& size, const Vec3f& origin, float angle,
                        bool canGraze) {
    const float c = std::cos(angle), sn = std::sin(angle);
    const Vec3f d = positionCenter - origin;
    const Vec3f q{origin.x + c * d.x + sn * d.y, origin.y + c * d.y - sn * d.x, 0.0f};
    const Vec3f ptl{q.x - hitboxSize.x, q.y - hitboxSize.y, 0.0f};
    const Vec3f pbr{q.x + hitboxSize.x, q.y + hitboxSize.y, 0.0f};
    Vec3f ltl{center.x - size.x / 2.0f, center.y - size.y / 2.0f, 0.0f};
    Vec3f lbr{center.x + size.x / 2.0f, center.y + size.y / 2.0f, 0.0f};
    if (overlap(ptl, pbr, ltl, lbr)) {
        if (state == Alive) die(world);
        return;
    }
    if (!canGraze) return;
    ltl = ltl - Vec3f{48.0f, 48.0f, 0.0f};
    lbr = lbr + Vec3f{48.0f, 48.0f, 0.0f};
    if (!overlap(ptl, pbr, ltl, lbr) || state == Dead || state == Spawning) return;
    addGraze(world, positionCenter);
}

int Player::calcDamageToEnemy(World& world, const Vec3f& enemyPos, const Vec3f& enemySize, bool* hitByBomb) {
    if (hitByBomb) *hitByBomb = false;
    const Vec3f etl{enemyPos.x - enemySize.x / 2.0f, enemyPos.y - enemySize.y / 2.0f, 0};
    const Vec3f ebr{enemyPos.x + enemySize.x / 2.0f, enemyPos.y + enemySize.y / 2.0f, 0};
    const bool bombing = world.state->bombActive;
    int damage = 0;
    for (Bullet& b : bullets_) {
        if (b.state == ShotState::Inactive) continue;
        if (b.state == ShotState::Collided && b.kind != ShotMissile) continue;
        const Vec3f btl{b.position.x - b.size.x / 2.0f, b.position.y - b.size.y / 2.0f, 0};
        const Vec3f bbr{b.position.x + b.size.x / 2.0f, b.position.y + b.size.y / 2.0f, 0};
        if (!overlap(btl, bbr, etl, ebr)) continue;
        damage += bombing ? std::max(b.damage / 3, 1) : b.damage;
        if (b.kind == ShotMissile) {
            b.damage = static_cast<std::uint16_t>(std::max(b.damage / 4, 1));
            if (b.sprite.anmFileIndex == b.script) {
                const float blast = missileBlastSize(b.script - kAnmOffsetPlayer);
                b.size = {blast, blast, 0.0f};
            }
            if (b.lifetime % 6 == 0) spawnEffect(world, EffectParticleRed, b.position, 1);
        }
        if (b.kind != ShotLaser) {
            if (b.state == ShotState::Fired) {
                world.anm->setAndExecuteScript(b.sprite, b.script + kScriptHitOffset);
                spawnEffect(world, EffectParticleRed, b.position, 1);
                b.position.z = 0.1f;
            }
            b.state = ShotState::Collided;
            b.velocity = b.velocity / 8.0f;
        } else if (++laserParticleCounter_ % 8 == 0) {
            spawnEffect(world, EffectParticleRed, {b.position.x, enemyPos.y, enemyPos.z}, 1);
        }
    }
    damage += bomb_.damageEnemy(world, enemyPos, etl, ebr, hitByBomb, laserParticleCounter_);
    (void)kDamageCap;  // the per-enemy cap is applied by the enemy manager
    return damage;
}

void Player::considerHomingTarget(const Vec3f& enemyPos) {
    if (homingTarget_.y < enemyPos.y) homingTarget_ = enemyPos;
}

void Player::drawLow(World& world, SpriteSink& sink, float alpha, const SpriteRegion& white) {
    auto drawShot = [&](Bullet& b) {
        const Vec3f p = lerp(b.prevPosition, b.position, alpha);
        b.sprite.pos = {p.x, p.y, b.position.z};
        if (b.sprite.autoRotate != 0) {
            float a = b.angle + kZunPi;
            while (a > kZunPi) a -= kZunTwoPi;
            while (a <= -kZunPi) a += kZunTwoPi;
            b.sprite.rotation.z = kZunHalfPi - a;
        }
        world.anm->drawWorld(sink, b.sprite, world.state->playfieldX, world.state->playfieldY);
    };
    for (Bullet& b : bullets_) {
        if (b.state == ShotState::Fired) drawShot(b);
    }
    const Vec3f p = lerp(prevPosition, positionCenter, alpha);
    bomb_.draw(world, sink, white, alpha, p);
    if (state == Dead) return;
    sprite_.pos = {world.state->playfieldX + p.x, world.state->playfieldY + p.y, 0.49f};
    world.anm->drawScreen(sink, sprite_);
    if (orbState_ != OrbState::Hidden && (state == Alive || state == Invulnerable)) {
        const Vec3f shift = p - positionCenter;  // orbs follow the interpolated body
        for (int i = 0; i < 2; ++i) {
            AnmVm& vm = orbVms_[static_cast<std::size_t>(i)];
            const Vec3f& o = orbPositions_[static_cast<std::size_t>(i)];
            vm.pos = {o.x + shift.x, o.y + shift.y, 0.491f};
            world.anm->drawWorld(sink, vm, world.state->playfieldX, world.state->playfieldY);
        }
    }
}

void Player::drawHits(World& world, SpriteSink& sink, float alpha) {
    for (Bullet& b : bullets_) {
        if (b.state != ShotState::Collided) continue;
        const Vec3f p = lerp(b.prevPosition, b.position, alpha);
        b.sprite.pos = {p.x, p.y, 0.4f};
        world.anm->drawWorld(sink, b.sprite, world.state->playfieldX, world.state->playfieldY);
    }
}

}  // namespace th06
