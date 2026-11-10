#include "th08/Bullets.hpp"

#include <algorithm>
#include <cmath>

#include "th08/Effects.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Gui.hpp"
#include "th08/Player.hpp"

namespace touhou::th08 {

namespace {

constexpr std::uint8_t kRemap16[24] = {0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 0, 0, 1, 1, 2, 2, 3, 4, 0};

// Polar vector: sine and cosine in double precision, one rounding (x87 behaviour).
Vec3f polar(float a, float m, float z = 0.0f) {
    return {static_cast<float>(std::cos(static_cast<double>(a)) * static_cast<double>(m)),
            static_cast<float>(std::sin(static_cast<double>(a)) * static_cast<double>(m)), z};
}

float aimAngle(const World& w, const Vec3f& p) {
    const Vec3f pl = w.player ? w.player->position() : Vec3f{192.0f, 384.0f, 0.0f};
    const float dx = pl.x - p.x, dy = pl.y - p.y;
    if (dx == 0.0f && dy == 0.0f) return kZunPi / 2.0f;
    return std::atan2(dy, dx);
}

// The colour remap of spawn and cancel sprites by the bullet sprite's size (TH07's rule).
int remap(const AnmVm& bulletVm, int c) {
    const float h = bulletVm.spriteHeight;
    if (h <= 16.0f) return c >= 0 && c < 24 ? kRemap16[c] : 0;
    if (h <= 32.0f) return c >= 0 && c < 8 ? kRemap16[16 + c] : 0;
    return c;
}

// The conditional copy: only when the slot's VM ran a different script or the template is
// still running; otherwise the previous occupant's VM state is kept.
void copyVm(AnmVm& slot, const AnmVm& tpl, bool force) {
    if (force || slot.scriptIndex != tpl.scriptIndex || tpl.current != nullptr) slot = tpl;
}

float randSigned(ZunRng& rng) { return static_cast<float>(rng.u32()) / 2147483648.0f - 1.0f; }

}  // namespace

BulletManager::BulletManager()
    : bullets_(kBullets), lasers_(kLasers), items_(kItems + 1) {
    for (auto& b : buckets_) b.reserve(kBullets);
}

BulletManager::~BulletManager() = default;

// ---- stage start ---------------------------------------------------------------------------

void BulletManager::stageStart(const World& w) {
    for (Bullet& b : bullets_) b = Bullet{};
    for (Laser& l : lasers_) l = Laser{};
    for (Item& it : items_) it = Item{};
    itemHead_ = itemTail_ = -1;
    bulletCursor_ = itemCursor_ = 0;
    liveCount_ = 0;
    clearCountdown_ = 0;
    cancelItemType_ = PointBullet;
    if (!w.anm || !w.rng || !w.tables) return;
    for (int t = 0; t < 21; ++t) {
        Template& tp = templates_[static_cast<std::size_t>(t)];
        AnmVm* vms[5] = {&tp.vm, &tp.fast, &tp.normal, &tp.slow, &tp.cancel};
        const auto& scripts = w.tables->bulletScripts[static_cast<std::size_t>(t)];
        for (int k = 0; k < 5; ++k) {
            *vms[k] = AnmVm{};
            vms[k]->start(*w.anm, *w.rng, kSlotEtama, scripts[static_cast<std::size_t>(k)]);
            vms[k]->set(AnmVm::ZWriteDisable, true);
        }
        tp.vm.baseSprite = tp.vm.activeSprite;
        const float h = tp.vm.sprite ? tp.vm.sprite->pixelHeight : 0.0f;
        const int script = scripts[0];
        const bool slim = script == 2 || script == 4 || script == 5 || script == 6 || script == 106 || script == 107 || script == 108 ||
                          script == 111 || script == 112;
        if (h <= 8.0f) {
            tp.hitSize = 4, tp.bucket = 5;
        } else if (h <= 16.0f) {
            tp.hitSize = slim ? 4.0f : 6.0f, tp.bucket = slim ? 4 : 3;
        } else if (h <= 32.0f) {
            // Scripts 8 and 113-115: 5; 9, 109, 110: 8; others 10 (spec_bombs §0, gap 8).
            if (script == 8 || (script >= 113 && script <= 115)) tp.hitSize = 5, tp.bucket = 1;
            else if (script == 9 || script == 109 || script == 110) tp.hitSize = 8, tp.bucket = 1;
            else tp.hitSize = 10, tp.bucket = 2;
        } else {
            tp.hitSize = 24, tp.bucket = 0;
        }
    }
}

// ---- patterns ------------------------------------------------------------------------------

void BulletManager::fire(const World& w, const Shooter& s) {
    if (!w.anm || !w.rng) return;
    const Template& t = templates_[static_cast<std::size_t>(std::clamp<int>(s.type, 0, 20))];
    const float aim = aimAngle(w, s.pos);
    bool full = false;
    for (int j = 0; j < s.count2 && !full; ++j) {
        for (int k = 0; k < s.count1; ++k) {
            if (!initBullet(w, s, t, j, k, aim)) {
                full = true;
                break;
            }
        }
    }
    if (s.flags & 0x200) w.playSound(s.sound);
}

bool BulletManager::initBullet(const World& w, const Shooter& s, const Template& t, int j, int k, float aim) {
    int slot = -1;
    for (int probe = 0; probe < kBullets; ++probe) {
        const int i = (bulletCursor_ + probe) % kBullets;
        if (bullets_[static_cast<std::size_t>(i)].state == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return false;
    Bullet& b = bullets_[static_cast<std::size_t>(slot)];
    const float fk = static_cast<float>(k), fj = static_cast<float>(j);
    float v = s.count2 > 1 ? s.speed1 - ((s.speed1 - s.speed2) * fj) / static_cast<float>(s.count2) : s.speed1;
    float a = 0.0f;
    switch (s.aimMode) {
        case 0:
        case 1:
            if (s.count1 & 1) a = s.angle2 * static_cast<float>((k + 1) / 2);
            else a = (static_cast<float>(k / 2) * s.angle2) + (s.angle2 * 0.5f);
            if (k & 1) a = -a;
            if (s.aimMode == 0) a += aim;
            a += s.angle1;
            break;
        case 2:
        case 3:
            if (s.aimMode == 2) a = aim;
            a = a + ((fk * kZunTwoPi) / static_cast<float>(s.count1));
            a = a + ((fj * s.angle2) + s.angle1);
            break;
        case 4:
        case 5:
            if (s.aimMode == 4) a = aim;
            a = a + (kZunPi / static_cast<float>(s.count1));
            a = a + ((fk * kZunTwoPi) / static_cast<float>(s.count1));
            a = a + s.angle1;
            break;
        case 6: a = w.rng->f32InRange(s.angle1 - s.angle2) + s.angle2; break;
        case 7:
            v = w.rng->f32InRange(s.speed1 - s.speed2) + s.speed2;
            a = a + ((fk * kZunTwoPi) / static_cast<float>(s.count1));
            a = a + ((fj * s.angle2) + s.angle1);
            break;
        case 8:
            a = w.rng->f32InRange(s.angle1 - s.angle2) + s.angle2;
            v = w.rng->f32InRange(s.speed1 - s.speed2) + s.speed2;
            break;
        default: break;
    }
    b.state = 1;
    b.grazed = 0;
    b.pendingCancel = false;
    b.noCollision = false;
    b.timerA.assign(0);
    b.timerB.assign(0);
    b.speed = v;
    b.angle = addNormalizeAngle(a, 0.0f);
    b.pos = {s.pos.x, s.pos.y, 0.1f};
    b.velocity = polar(a, v * g_timing.speed, 0.0f);
    b.color = s.color;
    b.type = s.type;
    const bool fresh = b.vm.scriptIndex == 0 && b.vm.sprite == nullptr;
    copyVm(b.vm, t.vm, fresh);
    copyVm(b.cancelVm, t.cancel, fresh);
    b.hitSize = t.hitSize;
    b.bucket = t.bucket;
    b.sound = s.bulletSound;
    b.spawnDelay = 0;
    if (b.vm.activeSprite != t.vm.activeSprite + b.color) b.vm.selectSprite(t.vm.activeSprite + b.color);
    b.cancelVm.selectSprite(t.cancel.activeSprite + remap(b.vm, b.color));
    const AnmVm* spawn = nullptr;
    if (s.flags & 0x2) spawn = &t.fast, b.state = 2;
    else if (s.flags & 0x4) spawn = &t.normal, b.state = 3;
    else if (s.flags & 0x8) spawn = &t.slow, b.state = 4;
    if (spawn) {
        copyVm(b.spawnVm, *spawn, fresh);
        b.spawnVm.selectSprite(spawn->activeSprite + remap(b.vm, b.color));
        b.pos.x -= b.velocity.x * 4.0f;
        b.pos.y -= b.velocity.y * 4.0f;
    }
    b.commands = s.commands;
    b.allowed = s.flags;
    b.active = 0;
    b.cursor = 0;
    runScheduler(w, b);
    if (clearCountdown_ != 0 && !(s.flags & 0x1000)) b.state = 5;
    bulletCursor_ = (slot + 1) % kBullets;
    return true;
}

void BulletManager::runScheduler(const World& w, Bullet& b) {
    while (b.cursor < 6) {
        const BulletCommand& c = b.commands[static_cast<std::size_t>(b.cursor)];
        if (c.type == 0) return;
        if (c.immediate == 0 && b.active != 0) return;
        if (!(b.allowed & c.type)) {
            ++b.cursor;
            continue;
        }
        const float m = g_timing.speed;
        switch (c.type) {
            case 0x1:
                b.active |= 0x1;
                b.burstTimer.assign(0);
                break;
            case 0x10: {
                b.active |= 0x10;
                const float dir = c.b > -990.0f ? c.b : b.angle;
                b.accelTimer.assign(0);
                b.accelDuration = c.duration;
                b.accel = polar(dir, m * c.a);
                if (b.cursor != 0 && b.sound >= 0) w.playSound(b.sound);
                break;
            }
            case 0x20:
                b.active |= 0x20;
                b.turnRate = c.b;
                b.turnSpeed = c.a;
                b.turnTimer.assign(0);
                b.turnDuration = c.duration;
                if (b.cursor != 0 && b.sound >= 0) w.playSound(b.sound);
                break;
            case 0x40:
            case 0x80:
            case 0x100:
                b.active |= c.type;
                b.aimAngle = c.a;
                b.aimSpeed = c.b > -999.0f ? c.b : b.speed;
                b.aimTimer.assign(0);
                b.aimInterval = c.duration;
                b.aimMax = c.loops;
                b.aimCount = 0;
                break;
            case 0x400:
            case 0x800:
                b.active |= c.type;
                b.bounceSpeed = c.a >= 0.0f ? c.a : b.speed;
                b.bounceMax = c.duration;
                b.bounceCount = 0;
                break;
            case 0x2000:
                b.spawnDelay = c.duration;
                ++b.cursor;
                continue;
            default: break;  // (the new kinds 0x20000, 0x400000, 0x800000 are not specified yet)
        }
        ++b.cursor;
        return;
    }
}

bool BulletManager::inBounds(const Bullet& b) const {
    const float w = b.vm.spriteWidth, h = b.vm.spriteHeight;
    return !(b.pos.x + w / 2.0f < 0.0f || b.pos.x - w / 2.0f > kPlayfieldW || b.pos.y + h / 2.0f < 0.0f || b.pos.y - h / 2.0f > kPlayfieldH);
}

void BulletManager::applyModifiers(const World& w, Bullet& b) {
    const float m = g_timing.speed;
    if (b.active & 0x1) {
        if (b.burstTimer.current <= 16) {
            const float s = 5.0f - (b.burstTimer.asFloat() * 5.0f) / 16.0f;
            b.velocity = polar(b.angle, (s + b.speed) * m, b.velocity.z);
        } else {
            b.active &= ~0x1u;
        }
        b.burstTimer.tick();
    }
    if (b.active & 0x10) {
        if (b.accelTimer.current >= b.accelDuration) {
            b.active &= ~0x10u;
        } else {
            b.velocity.x += b.accel.x * m;
            b.velocity.y += b.accel.y * m;
            b.velocity.z += b.accel.z * m;
            if (std::fabs(b.velocity.x) > 0.0001f || std::fabs(b.velocity.y) > 0.0001f) b.angle = std::atan2(b.velocity.y, b.velocity.x);
        }
        b.accelTimer.tick();
    }
    if (b.active & 0x20) {
        if (b.turnTimer.current >= b.turnDuration) {
            b.active &= ~0x20u;
        } else {
            b.angle = addNormalizeAngle(b.angle, b.turnRate * m);
            b.speed = b.speed + b.turnSpeed * m;
            b.velocity = polar(b.angle, b.speed * m, b.velocity.z);
        }
        b.turnTimer.tick();
    }
    for (std::uint32_t bit : {0x40u, 0x100u, 0x80u}) {
        if (!(b.active & bit)) continue;
        float s;
        if (b.aimTimer.current >= b.aimInterval) {
            if (b.sound >= 0) w.playSound(b.sound);
            if (++b.aimCount >= b.aimMax) b.active &= ~bit;
            if (bit == 0x40) b.angle = b.angle + b.aimAngle;
            else if (bit == 0x100) b.angle = b.aimAngle;
            else b.angle = addNormalizeAngle(aimAngle(w, b.pos), b.aimAngle);
            b.speed = b.aimSpeed;
            s = b.speed;
            b.aimTimer.assign(0);
        } else {
            s = b.speed - (b.aimTimer.asFloat() * b.speed) / static_cast<float>(b.aimInterval);
        }
        b.velocity = polar(b.angle, s * m, b.velocity.z);
        b.aimTimer.tick();
    }
    if (b.active & 0xC00) {
        if (!inBounds(b)) {
            if (b.sound >= 0) w.playSound(b.sound);
            if (b.pos.x < 0.0f || b.pos.x >= kPlayfieldW) b.angle = addNormalizeAngle(-b.angle, -kZunPi);
            if (b.pos.y < 0.0f || (b.pos.y >= kPlayfieldH && (b.active & 0x400))) b.angle = -b.angle;
            b.speed = b.bounceSpeed;
            b.velocity = polar(b.angle, b.speed * m, b.velocity.z);
            if (++b.bounceCount >= b.bounceMax) b.active &= ~0xC00u;
        }
    }
}

// What a cancelled bullet becomes (spec 15.5): type 9 -> two time orbs, t >= 0 one item of
// type t, t < 0 nothing; all homing.
void BulletManager::cancelItems(const World& w, Bullet& b, int t) {
    if (t == 9) {
        spawnItem(w, b.pos, TimeOrb, Homing);
        spawnItem(w, b.pos, TimeOrb, Homing);
    } else if (t >= 0) {
        spawnItem(w, b.pos, t, Homing);
    }
}

// ---- the frame -----------------------------------------------------------------------------

void BulletManager::update(const World& w) {
    if (w.mode & ModeSpellFreeze) return;
    updateItems(w);
    liveCount_ = 0;
    for (auto& bk : buckets_) bk.clear();
    Player* player = w.player;
    for (int n = 0; n < kBullets; ++n) {
        const int i = n == 0 ? 0 : kBullets - n;  // visit order 0, 1535, 1534, ..., 1
        Bullet& b = bullets_[static_cast<std::size_t>(i)];
        if (b.state == 0) continue;
        ++liveCount_;
        bool normal = b.state == 1;
        if (b.state >= 2 && b.state <= 4) {
            b.timerB.decrement(1);
            const float f = b.state == 2 ? 0.5f : b.state == 3 ? 0.4f : 0.33333334f;
            b.pos.x += b.velocity.x * f;
            b.pos.y += b.velocity.y * f;
            b.pos.z += b.velocity.z * f;
            if (!(b.allowed & 0x1000) && player && player->cancelTest(b.pos)) b.pendingCancel = true;
            if (b.spawnVm.tick(*w.rng)) {
                if (b.pendingCancel && player) {
                    b.state = 5;
                    cancelItems(w, b, player->cancelItemType());
                }
                b.state = 1;  // a pending cancel still flies on [QUIRK]
                b.timerA.assign(0);
                normal = true;
            }
        } else if (b.state == 5) {
            b.pos.x += b.velocity.x * 0.5f;
            b.pos.y += b.velocity.y * 0.5f;
            b.pos.z += b.velocity.z * 0.5f;
            if (b.cancelVm.tick(*w.rng)) {
                b.state = 0;
                b.timerA.reset();
                b.timerB.reset();
                continue;
            }
        }
        if (normal) {
            runScheduler(w, b);
            if (b.active != 0) applyModifiers(w, b);
            if (b.spawnDelay != 0) --b.spawnDelay;
            if (!w.timeStop) b.pos += b.velocity;
            if (b.spawnDelay == 0) {
                if (!inBounds(b)) {
                    if (b.active & 0xDC0) {
                        if (++b.offscreen >= 128) {
                            b.state = 0;
                            b.timerA.reset();
                            b.timerB.reset();
                            continue;
                        }
                    } else if (b.offscreen == 0) {
                        b.state = 0;
                        b.timerA.reset();
                        b.timerB.reset();
                        continue;
                    } else {
                        --b.offscreen;
                    }
                } else {
                    b.offscreen = 0;
                }
            }
            if (player && !b.noCollision) {
                const Vec3f size{b.hitSize, b.hitSize, b.hitSize};
                using R = Player::HitResult;
                auto hitTest = [&]() {
                    const R r = player->bulletHit(w, b.pos, size);
                    if (r == R::Hit) b.state = 5;
                    else if (r == R::Cancelled && !(b.allowed & 0x1000)) {
                        b.state = 5;
                        cancelItems(w, b, player->cancelItemType());
                    }
                };
                if (b.grazed == 0 && b.timerB.current >= 16) {
                    const R r = player->bulletGraze(w, b.pos, size, false);
                    if (r == R::Grazed) {
                        b.grazed = 1;
                        hitTest();
                    } else if (r == R::Cancelled && !(b.allowed & 0x1000)) {
                        b.state = 5;
                        cancelItems(w, b, player->cancelItemType());
                    }
                } else {
                    hitTest();
                }
            }
            if (b.vm.current) b.vm.tick(*w.rng);
        }
        b.timerA.tick();
        b.timerB.tick();
        buckets_[static_cast<std::size_t>(std::min<int>(b.bucket, 5))].push_back(static_cast<std::int16_t>(i));
    }
    updateLasers(w);
    if (clearCountdown_ != 0) --clearCountdown_;
}

// ---- clears --------------------------------------------------------------------------------

void BulletManager::clearAll(const World& w, int q) {
    Player* player = w.player;
    for (Bullet& b : bullets_) {
        if (b.state == 0 || b.state == 5) continue;
        if (player) (void)player->cancelTest(b.pos);  // the first result is discarded [QUIRK]
        if (player && player->cancelTest(b.pos)) {
            spawnItem(w, b.pos, player->cancelItemType(), Homing);
            b = Bullet{};
        } else if (q != 4) {
            spawnItem(w, b.pos, cancelItemType_, q);
            b = Bullet{};
        } else {
            b.state = 5;
        }
    }
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        if ((l.flags & 0x4) && q != 4) continue;
        if (l.state < 2) {
            l.state = 2;
            l.timer.assign(0);
            l.width = l.targetWidth;
        }
        l.hitboxEnd = 0;
    }
    clearCountdown_ = 10;
}

std::uint32_t BulletManager::bonusClear(const World& w, std::uint32_t maxBonus) {
    std::uint32_t b = 2000, total = 0;
    Player* player = w.player;
    for (Bullet& bl : bullets_) {
        if (bl.state == 0) continue;
        if (player && player->cancelTest(bl.pos)) spawnItem(w, bl.pos, player->cancelItemType(), Homing);
        spawnItem(w, bl.pos, cancelItemType_, Homing);
        total += b;
        b = std::min(b + 20, maxBonus);
        bl = Bullet{};
    }
    if (w.state) w.state->score += total / 10;
    return total;
}

// ---- lasers --------------------------------------------------------------------------------

Laser* BulletManager::spawnLaser(const World& w, const Shooter& s, float startOffset, float endOffset, float startLength, float width,
                                 std::int32_t startTime, std::int32_t duration, std::int32_t endTime, std::int32_t hitboxStart,
                                 std::int32_t hitboxEnd, std::uint16_t flags, bool aimed) {
    for (Laser& l : lasers_) {
        if (l.inUse) continue;
        l = Laser{};
        l.inUse = true;
        l.origin = s.pos;
        l.angle = aimed ? addNormalizeAngle(aimAngle(w, s.pos), s.angle1) : s.angle1;
        l.start = startOffset;
        l.end = endOffset;
        l.startLength = startLength;
        l.width = width;
        l.speed = s.speed1;
        l.startTime = startTime;
        l.duration = duration;
        l.endTime = endTime;
        l.hitboxStart = hitboxStart;
        l.hitboxEnd = hitboxEnd;
        l.flags = flags;
        l.color = s.color;
        l.timer.assign(0);
        if (w.anm && w.rng && w.tables) {
            const auto& scripts = w.tables->bulletScripts[static_cast<std::size_t>(std::clamp<int>(s.type, 0, 20))];
            l.beam.start(*w.anm, *w.rng, kSlotEtama, scripts[0]);
            l.beam.selectSprite(l.beam.activeSprite + s.color);
            l.beam.set(AnmVm::ZWriteDisable, true);
        }
        return &l;
    }
    return &dummyLaser_;
}

void BulletManager::stopLaser(Laser& l) {
    if (l.inUse && l.state < 2) {
        l.state = 2;
        l.timer.assign(0);
        l.width = l.targetWidth;
    }
}

void BulletManager::updateLasers(const World& w) {
    Player* player = w.player;
    const float m = g_timing.speed;
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        l.end += l.speed * m;
        if (l.end - l.start > l.startLength) l.start = l.end - l.startLength;
        if (l.start < 0.0f) l.start = 0.0f;
        float len = l.end - l.start;
        if (l.start > 0.0f) len *= 0.7f;  // the hit box shortens once the laser left its origin [QUIRK]
        const Vec3f centre{(l.end - l.start) / 2.0f + l.start + l.origin.x, l.origin.y, 0.0f};
        Vec3f size{len, l.width / 2.0f, 0.0f};
        auto test = [&](bool graze) {
            if (player) player->laserTest(w, l.origin, l.angle, centre, size, graze);
        };
        const std::int32_t t = l.timer.current;
        bool tail = true;
        if (l.state == 0) {
            if (l.flags & 0x1) {
                l.beam.color = static_cast<std::uint32_t>(std::min(255, l.startTime > 0 ? t * 255 / l.startTime : 255)) << 24;
            } else {
                const std::int32_t ramp = std::min(l.startTime, 30);
                const float wdt = (l.startTime - ramp) < t && l.startTime > 0 ? static_cast<float>(t) * l.width / static_cast<float>(l.startTime) : 1.2f;
                l.targetWidth = wdt;
                l.beam.scale.x = wdt / 16.0f;
                size.y = wdt / 2.0f;
            }
            if (t >= l.hitboxStart) test(false);
            if (t >= l.startTime) {
                l.timer.assign(0);
                l.state = 1;
                l.targetWidth = l.width;
            }
        }
        if (l.state == 1 && tail) {
            test(l.timer.current % 20 == 0);
            if (l.timer.current >= l.duration) {
                l.timer.assign(0);
                l.state = 2;
                if (l.endTime == 0) {
                    l.inUse = false;
                    continue;
                }
            }
        }
        if (l.state == 2) {
            if (l.endTime > 0) {
                const float wdt = l.width - static_cast<float>(l.timer.current) * l.width / static_cast<float>(l.endTime);
                l.beam.scale.x = wdt / 16.0f;
                size.y = wdt / 2.0f;
            }
            if (l.timer.current < l.hitboxEnd) test(false);
            if (l.timer.current >= l.endTime) {
                l.inUse = false;
                continue;
            }
        }
        if (l.start >= 640.0f) l.inUse = false;
        l.timer.tick();
        l.beam.scale.y = (l.end - l.start) / 16.0f;
        l.beam.set(AnmVm::ScaleDirty, true);
        l.beam.rotation.z = l.angle - kZunPi / 2.0f;
        if (w.rng) l.beam.tick(*w.rng);
    }
}

// ---- items ---------------------------------------------------------------------------------

int BulletManager::liveItems() const {
    int n = 0;
    for (int i = itemHead_; i >= 0; i = items_[static_cast<std::size_t>(i)].next) ++n;
    return n;
}

void BulletManager::freeItem(int i) {
    Item& it = items_[static_cast<std::size_t>(i)];
    if (it.prev >= 0) items_[static_cast<std::size_t>(it.prev)].next = it.next;
    else itemHead_ = it.next;
    if (it.next >= 0) items_[static_cast<std::size_t>(it.next)].prev = it.prev;
    else itemTail_ = it.prev;
    it.inUse = false;
    it.next = it.prev = -1;
}

BulletManager::Item& BulletManager::spawnItem(const World& w, const Vec3f& pos, int type, int state) {
    Item& sink = items_[kItems];
    if (pos.x < -64.0f || pos.x > 448.0f) return sink;
    if (w.state && w.state->power >= 128.0f && (type == SmallPower || type == BigPower)) type = SmallFullPower;
    if (type == TimeOrb) state = OrbRising;
    if (type == 10) state = GrazeOrb, type = TimeOrb;
    int slot = -1;
    for (int probe = 0; probe < kItems; ++probe) {
        itemCursor_ = (itemCursor_ + 1) % kItems;
        if (items_[static_cast<std::size_t>(itemCursor_)].inUse) {
            if (type == TimeOrb) return sink;  // a time orb needs the very first probe free [QUIRK]
            continue;
        }
        slot = itemCursor_;
        break;
    }
    if (slot < 0) return sink;
    Item& it = items_[static_cast<std::size_t>(slot)];
    it = Item{};
    it.inUse = true;
    it.pos = pos;
    it.velocity = {0.0f, -2.2f, 0.0f};
    it.type = static_cast<std::int8_t>(type);
    it.state = static_cast<std::uint8_t>(state);
    it.timer.assign(0);
    if (state == Scatter && w.rng) {
        it.target.x = w.rng->f32InRange(288.0f) + 48.0f;
        it.target.y = w.rng->f32InRange(192.0f) - 64.0f;
        it.target.z = 0.0f;
        it.velocity = pos;  // the start position during the scatter
    } else if ((state == OrbRising || state == GrazeOrb) && w.rng) {
        it.velocity.y = -2.0f - w.rng->f32InRange(0.2f);
        it.velocity.x = randSigned(*w.rng) * 0.6f;
        if (w.player && w.player->state() == Player::State::Dead) {
            it.state = Falling;
            it.velocity = {0.0f, -0.9f, 0.0f};
        }
    }
    if (w.anm && w.rng) {
        it.vm.start(*w.anm, *w.rng, kSlotEtama, type + 61);
        it.vm.color = 0xFFFFFFFFu;
        it.vm.set(AnmVm::ZWriteDisable, true);
    }
    it.onScreen = true;
    it.prev = itemTail_;
    if (itemTail_ >= 0) items_[static_cast<std::size_t>(itemTail_)].next = slot;
    else itemHead_ = slot;
    itemTail_ = slot;
    return it;
}

void BulletManager::collectAll() {
    for (int i = itemHead_; i >= 0; i = items_[static_cast<std::size_t>(i)].next) {
        Item& it = items_[static_cast<std::size_t>(i)];
        it.state = Homing;
        it.velocity = {0.0f, -0.5f, 0.0f};
    }
}

void BulletManager::stopHoming() {
    for (int i = itemHead_; i >= 0; i = items_[static_cast<std::size_t>(i)].next) {
        Item& it = items_[static_cast<std::size_t>(i)];
        if (it.state != Homing) continue;
        it.state = Falling;
        it.velocity = {0.0f, -0.9f, 0.0f};
    }
}

void BulletManager::convertPowerItems(const World& w, const Item* except) {
    for (int i = itemHead_; i >= 0; i = items_[static_cast<std::size_t>(i)].next) {
        Item& it = items_[static_cast<std::size_t>(i)];
        if (&it == except || (it.type != SmallPower && it.type != BigPower)) continue;
        if (it.velocity.y > -0.5f) it.velocity = {0.0f, -0.5f, 0.0f};
        if (w.effects) w.effects->spawn(w, 0, it.pos, 1, 0xFFFFFFFFu);
        it.type = SmallFullPower;
        if (w.anm && w.rng) it.vm.start(*w.anm, *w.rng, kSlotEtama, 0x45);
    }
}

void BulletManager::updateItems(const World& w) {
    Player* player = w.player;
    GameState* g = w.state;
    if (!player || !g) return;
    const ShotType& st = player->shots();
    const float k = (player->focused() ? st.focused.itemFactor : st.unfocused.itemFactor) * g_timing.speed;
    const float m = g_timing.speed;
    itemSound_ = -1;
    bool fullValueSound = false;
    for (int i = itemHead_; i >= 0;) {
        Item& it = items_[static_cast<std::size_t>(i)];
        const int next = it.next;
        bool pickup = true, common = true;
        switch (it.state) {
            case Scatter:
                if (it.timer.current < 60) {
                    const float s = it.timer.asFloat() / 60.0f;
                    it.pos = it.target * s + it.velocity * (1.0f - s);
                    common = false;
                } else {
                    it.velocity = {};
                    it.state = Falling;
                }
                break;
            case OrbRising:
                it.velocity.y += 0.05f * m;
                if (it.velocity.y > 0.0f || !player->firing()) it.state = Homing;
                if (player->state() == Player::State::Dead) {
                    it.state = Falling;
                    it.velocity = {0.0f, -0.7f, 0.0f};
                }
                pickup = false;  // (no pickup while rising)
                break;
            case GrazeOrb:
                it.velocity.y += 0.05f * m;
                it.pos += it.velocity * k;
                if (it.velocity.y <= 0.0f) {
                    common = pickup = false;
                } else {
                    it.state = Homing;
                    if (player->state() == Player::State::Dead) {
                        it.state = Falling;
                        it.velocity = {0.0f, -0.7f, 0.0f};
                    }
                }
                break;
            default: {
                bool home = it.state == Homing;
                if (it.state == Falling) {
                    const bool autoCollect = player->position().y < st.unfocused.pocLine &&
                                             (g->power >= 128.0f || player->focused() || player->character() == 1 || player->character() == 6);
                    if (autoCollect) home = true;
                    else {
                        it.velocity.x = 0.0f, it.velocity.z = 0.0f;
                        it.velocity.y = std::max(it.velocity.y, -2.2f);
                    }
                }
                if (home) {
                    if (player->dead()) {
                        it.velocity.y = -0.7f;
                        it.state = Falling;
                    } else {
                        const Vec3f pp = player->position();
                        const float a = std::atan2(pp.y - it.pos.y, pp.x - it.pos.x);
                        it.velocity = {static_cast<float>(std::cos(static_cast<double>(a)) * st.unfocused.itemHomingSpeed),
                                       static_cast<float>(std::sin(static_cast<double>(a)) * st.unfocused.itemHomingSpeed), 0.0f};
                        it.state = Homing;
                        it.pos += it.velocity * m;
                        common = false;
                    }
                }
                break;
            }
        }
        if (common) {
            it.pos += it.velocity * k;
            if (it.state == Falling && it.pos.y >= 448.0f + 16.0f) {
                freeItem(i);
                i = next;
                continue;
            }
            if (it.velocity.y < 3.0f) it.velocity.y += 0.03f * k;
            else it.velocity.y = 3.0f;
        }
        if (pickup && it.state != OrbRising && player->canPickUp() && player->pickupBoxTouches(it.pos)) {
            fullValueSound = fullValueSound || it.fullValue;
            collect(w, it);
            itemSound_ = fullValueSound ? 44 : 21;
            freeItem(i);
            i = next;
            continue;
        }
        it.timer.tick();
        if (it.vm.current && w.rng) it.vm.tick(*w.rng);
        i = next;
    }
    if (itemSound_ >= 0) w.playSound(itemSound_);
}

void BulletManager::collect(const World& w, Item& it) {
    GameState& g = *w.state;
    Player& player = *w.player;
    switch (it.type) {
        case SmallPower:
        case BigPower: {
            if (g.power >= 128.0f) break;
            const int before = g.powerI();
            g.power = std::min(128.0f, g.power + (it.type == SmallPower ? 1.0f : 8.0f));
            g.score += 1;  // 10 points
            if (g.power >= 128.0f) {
                if (!w.spellCardActive) clearAll(w, 1);
                convertPowerItems(w, &it);
            }
            (void)before;
            break;
        }
        case Point: {
            const std::int32_t maxV = g.pointValue;
            const float poc = player.header().pocLine;
            std::int32_t v = it.pos.y < poc ? maxV : maxV / 2 - static_cast<std::int32_t>(it.pos.y - poc) * (maxV / 1000);
            if (it.fullValue) v = maxV;
            v -= v % 10;
            if (v >= maxV) it.fullValue = true;
            g.score += static_cast<std::uint32_t>(std::max(0, v)) / 10;
            ++g.stagePointItems;
            ++g.pointItems;
            break;
        }
        case SmallFullPower: {
            const std::int32_t maxV = g.pointValue;
            const float poc = player.header().pocLine;
            std::int32_t v = it.pos.y < poc ? maxV : maxV / 2 - static_cast<std::int32_t>(it.pos.y - poc) * (maxV / 1000);
            v /= 10;
            v -= v % 10;
            g.score += static_cast<std::uint32_t>(std::max(0, v)) / 10;
            break;
        }
        case Bomb:
            if (g.bombs < 8.0f) g.bombs += 1.0f;
            break;
        case FullPower:
            if (g.power < 128.0f) {
                clearAll(w, 1);
                convertPowerItems(w, &it);
            }
            g.power = 128.0f;
            g.score += 100;
            break;
        case Extend:
            if (g.lives < 8.0f) g.lives += 1.0f;
            else if (g.bombs < 8.0f) g.bombs += 1.0f;
            w.playSound(28);
            break;
        case PointBullet: {
            const std::int32_t v = std::max(10, g.graze / 40 * 10 + 300);
            g.score += static_cast<std::uint32_t>(v) / 10;
            break;
        }
        case TimeOrb: {
            const std::int32_t v = g.stagePointItems >= 2000 ? 10000 : std::max(100, (g.pointItems / 2) * 10);
            g.score += static_cast<std::uint32_t>(v) / 10;
            ++g.timePoints;
            break;
        }
        default: break;
    }
    g.guiScore = g.score * 10 > g.guiScore ? g.score * 10 : g.guiScore;
}

// ---- drawing -------------------------------------------------------------------------------

void BulletManager::draw(SpriteSink& sink, const AnmDrawState& state) {
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        const float mid = (l.end + l.start) / 2.0f;
        l.beam.position = {l.origin.x + static_cast<float>(std::cos(l.angle)) * mid + kPlayfieldX,
                           l.origin.y + static_cast<float>(std::sin(l.angle)) * mid + kPlayfieldY, 0.0f};
        drawVm(sink, l.beam, state);
    }
    for (int i = itemHead_; i >= 0; i = items_[static_cast<std::size_t>(i)].next) {
        Item& it = items_[static_cast<std::size_t>(i)];
        it.vm.position = {it.pos.x + kPlayfieldX, it.pos.y + kPlayfieldY, 0.0f};
        drawVm(sink, it.vm, state);
    }
    for (int bk = 0; bk < 6; ++bk) {
        for (std::int16_t idx : buckets_[static_cast<std::size_t>(bk)]) {
            Bullet& b = bullets_[static_cast<std::size_t>(idx)];
            AnmVm& vm = b.state >= 2 && b.state <= 4 ? b.spawnVm : b.state == 5 ? b.cancelVm : b.vm;
            if (vm.autoRotate != 0) vm.rotation.z = b.angle + kZunPi / 2.0f;
            vm.position = {b.pos.x + kPlayfieldX, b.pos.y + kPlayfieldY, b.pos.z};
            drawVm(sink, vm, state);
        }
    }
}

}  // namespace touhou::th08
