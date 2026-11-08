#include "th07/Bullets.hpp"

#include <algorithm>
#include <cmath>

#include "th07/Effects.hpp"
#include "th07/ExeTables.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {

constexpr int kS0 = 0x200, kS1 = 0x2A8;
// Bullet, fast spawn, normal spawn, slow spawn, cancel (global scripts).
constexpr int kTypeScripts[11][5] = {
    {kS0 + 0, kS0 + 18, kS0 + 19, kS0 + 20, kS0 + 15}, {kS0 + 1, kS0 + 21, kS0 + 22, kS0 + 23, kS0 + 16},
    {kS0 + 2, kS0 + 21, kS0 + 22, kS0 + 23, kS0 + 16}, {kS0 + 3, kS0 + 21, kS0 + 22, kS0 + 23, kS0 + 16},
    {kS0 + 4, kS0 + 21, kS0 + 22, kS0 + 23, kS0 + 16}, {kS0 + 5, kS0 + 21, kS0 + 22, kS0 + 23, kS0 + 16},
    {kS0 + 6, kS0 + 21, kS0 + 22, kS0 + 23, kS0 + 16}, {kS0 + 7, kS0 + 24, kS0 + 24, kS0 + 24, kS0 + 17},
    {kS0 + 8, kS0 + 24, kS0 + 24, kS0 + 24, kS0 + 17}, {kS0 + 9, kS0 + 24, kS0 + 24, kS0 + 24, kS0 + 17},
    {kS1 + 0, kS1 + 2, kS1 + 2, kS1 + 2, kS1 + 1},
};
constexpr int kTable16[24] = {0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 0, 0, 1, 1, 2, 2, 3, 4, 0};

// Polar vector: sine and cosine in double precision, one rounding (x87 behaviour).
Vec3f polar(float a, float m, float z = 0.0f) {
    return {static_cast<float>(std::cos(static_cast<double>(a)) * static_cast<double>(m)),
            static_cast<float>(std::sin(static_cast<double>(a)) * static_cast<double>(m)), z};
}

float aimAngle(const World& w, const Vec3f& p) {
    const Vec3f& pl = w.player->position();
    const float dx = pl.x - p.x, dy = pl.y - p.y;
    if (dx == 0.0f && dy == 0.0f) return kZunPi / 2.0f;
    return std::atan2(dy, dx);
}

int remap(const AnmVm& bulletVm, int c) {
    const float h = bulletVm.sprite ? bulletVm.sprite->pixelHeight : 0.0f;
    if (h <= 16.0f) return c >= 0 && c < 24 ? kTable16[c] : 0;
    if (h <= 32.0f) return c >= 0 && c < 8 ? kTable16[16 + c] : 0;
    return c;
}

// The conditional copy: only when the slot's VM ran a different script or the template is
// still running; otherwise the previous occupant's VM state is kept.
void copyVm(AnmVm& slot, const AnmVm& tpl, bool force) {
    if (force || slot.scriptIndex != tpl.scriptIndex || tpl.current != nullptr) slot = tpl;
}

void setSprite(AnmVm& vm, const AnmLibrary& lib, int index) {
    if (vm.activeSprite != index) vm.selectSprite(lib, index);
}

}  // namespace

// ---- stage start ---------------------------------------------------------------------------

void BulletManager::stageStart(const World& w) {
    world_ = &w;
    for (Bullet& b : bullets_) b = Bullet{};
    for (Laser& l : lasers_) l = Laser{};
    for (Item& it : items_) it = Item{};
    bulletCursor_ = itemCursor_ = 0;
    liveCount_ = 0;
    clearCountdown_ = 0;
    cancelItemType_ = PointBullet;
    for (int t = 0; t < 11; ++t) {
        Template& tp = templates_[static_cast<std::size_t>(t)];
        AnmVm* vms[5] = {&tp.vm, &tp.fast, &tp.normal, &tp.slow, &tp.cancel};
        for (int k = 0; k < 5; ++k) {
            *vms[k] = AnmVm{};
            vms[k]->start(*w.anm, *w.rng, kTypeScripts[t][k]);
            vms[k]->set(AnmVm::ZWriteDisable, true);
        }
        tp.vm.baseSprite = tp.vm.activeSprite;
        const float h = tp.vm.sprite ? tp.vm.sprite->pixelHeight : 0.0f;
        tp.height = static_cast<std::uint8_t>(static_cast<int>(h));
        const int script = kTypeScripts[t][0];
        const bool slim = script == kS0 + 2 || script == kS0 + 4 || script == kS0 + 5 || script == kS0 + 6;
        if (h <= 8.0f) tp.hitSize = 4, tp.bucket = 5;
        else if (h <= 16.0f) tp.hitSize = slim ? 4.0f : 6.0f, tp.bucket = slim ? 4 : 3;
        else if (h <= 32.0f) {
            if (script == kS0 + 8) tp.hitSize = 5, tp.bucket = 1;
            else if (script == kS0 + 9) tp.hitSize = 8, tp.bucket = 2;
            else tp.hitSize = 10, tp.bucket = 2;
        } else {
            tp.hitSize = 24, tp.bucket = 0;
        }
    }
}

// ---- patterns ------------------------------------------------------------------------------

void BulletManager::fire(const World& w, const Shooter& s) {
    if (liveCount_ >= kBullets) return;
    const Template& t = templates_[static_cast<std::size_t>(std::clamp<int>(s.type, 0, 10))];
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
    b.timerA.assign(0);
    b.timerB.assign(0);
    b.speed = v;
    b.angle = addNormalizeAngle(a, 0.0f);
    b.pos = {s.pos.x, s.pos.y, 0.1f};
    b.velocity = polar(a, v * g_timing.speed, 0.0f);
    b.color = s.color;
    b.tag = 0;
    const bool fresh = b.vm.scriptIndex == 0 && b.vm.sprite == nullptr;
    copyVm(b.vm, t.vm, fresh);
    copyVm(b.cancelVm, t.cancel, fresh);
    b.hitSize = t.hitSize;
    b.bucket = t.bucket;
    b.height = t.height;
    b.sound = s.bulletSound;
    b.spawnDelay = 0;
    setSprite(b.vm, *w.anm, t.vm.activeSprite + b.color);
    b.cancelVm.selectSprite(*w.anm, t.cancel.activeSprite + remap(b.vm, b.color));
    const AnmVm* spawn = nullptr;
    if (s.flags & 0x2) spawn = &t.fast, b.state = 2;
    else if (s.flags & 0x4) spawn = &t.normal, b.state = 3;
    else if (s.flags & 0x8) spawn = &t.slow, b.state = 4;
    if (spawn) {
        copyVm(b.spawnVm, *spawn, fresh);
        b.spawnVm.selectSprite(*w.anm, spawn->activeSprite + remap(b.vm, b.color));
        b.pos.x -= b.velocity.x * 4.0f;
        b.pos.y -= b.velocity.y * 4.0f;
    }
    for (int c = 0; c < 5; ++c) b.commands[static_cast<std::size_t>(c)] = s.commands[static_cast<std::size_t>(c)];
    b.allowed = s.flags & 0xFFFFu;
    b.active = 0;
    b.cursor = 0;
    runScheduler(w, b);
    if (clearCountdown_ != 0 && !(s.flags & 0x1000)) b.state = 5;
    bulletCursor_ = (slot + 1) % kBullets;
    return true;
}

void BulletManager::runScheduler(const World& w, Bullet& b) {
    while (b.cursor < 5) {
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
            default: break;
        }
        ++b.cursor;
        return;
    }
}

bool BulletManager::inBounds(const Bullet& b) const {
    const float w = b.vm.sprite ? b.vm.sprite->pixelWidth : 0.0f;
    const float h = b.vm.sprite ? b.vm.sprite->pixelHeight : 0.0f;
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

void BulletManager::cancelInto(const World& w, Bullet& b, int itemType) {
    (void)w;
    b.state = 5;
    spawn(b.pos, itemType, AutoCollect);
}

// ---- the frame -----------------------------------------------------------------------------

void BulletManager::update(const World& w) {
    world_ = &w;
    if (w.state->timeStop) return;
    updateItems(w);
    liveCount_ = 0;
    bucketCount_.fill(0);
    Player& player = *w.player;
    for (int n = 0; n < kBullets; ++n) {
        const int i = n == 0 ? 0 : kBullets - n;  // visit order 0, 1023, 1022, ..., 1
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
            if (b.spawnVm.tick(*w.anm, *w.rng)) {
                b.state = 1;
                b.timerA.assign(0);
                normal = true;
            }
        } else if (b.state == 5) {
            b.pos.x += b.velocity.x * 0.5f;
            b.pos.y += b.velocity.y * 0.5f;
            b.pos.z += b.velocity.z * 0.5f;
            if (b.cancelVm.tick(*w.anm, *w.rng)) {
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
            b.pos += b.velocity;
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
            const Vec3f size{b.hitSize, b.hitSize, b.hitSize};
            using R = Player::HitResult;
            auto hitTest = [&]() {
                const R r = player.bulletHit(w, b.pos, size);
                if (r == R::Hit) b.state = 5;
                else if (r == R::Cancelled && !(b.allowed & 0x1000)) cancelInto(w, b, player.cancelItemType());
            };
            if (b.grazed == 0 && b.timerB.current >= 16) {
                const R r = player.bulletGraze(w, b.pos, size);
                if (r == R::Grazed) {
                    b.grazed = 1;
                    hitTest();
                } else if (r == R::Cancelled && !(b.allowed & 0x1000)) {
                    cancelInto(w, b, player.cancelItemType());
                }
            } else {
                hitTest();
            }
            if (b.vm.current) b.vm.tick(*w.anm, *w.rng);
        }
        b.timerA.tick();
        b.timerB.tick();
        const auto bucket = static_cast<std::size_t>(std::min<int>(b.bucket, 5));
        buckets_[bucket][static_cast<std::size_t>(bucketCount_[bucket]++)] = static_cast<std::int16_t>(i);
    }
    updateLasers(w);
    if (clearCountdown_ != 0) --clearCountdown_;
}

// ---- clears ----------------------------------------------------------------------------------

void BulletManager::clearAll(const World& w, int q) {
    for (Bullet& b : bullets_) {
        if (b.state == 0 || b.state == 5) continue;
        if (q >= 1 && q <= 8) {
            if (q < 3) spawn(b.pos, cancelItemType_, q);
            else spawn(b.pos, SmallCherry, AutoCollect);
            b = Bullet{};
        } else {
            b.state = 5;
        }
    }
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        if ((l.flags & 0x4) && q != 10) continue;
        if (l.state < 2) {
            l.state = 2;
            l.timer.assign(0);
            l.width = l.targetWidth;
            if (q >= 1 && q <= 8) {
                if (q < 3) laserItems(w, l, cancelItemType_, q, false);
                else laserItems(w, l, SmallCherry, AutoCollect, false);
            }
        }
        l.hitboxEnd = 0;
    }
    clearCountdown_ = 10;
}

std::uint32_t BulletManager::bonusClear(const World& w, std::uint32_t maxBonus, bool items) {
    std::uint32_t b = 2000, total = 0;
    for (Bullet& bl : bullets_) {
        if (bl.state == 0) continue;
        spawn(bl.pos, cancelItemType_, AutoCollect);
        if (w.popups) w.popups->score(bl.pos, b, b >= maxBonus ? 0xFFFFFF00u : 0xFFFFFFFFu, false);
        total += b;
        b = std::min(b + 20, maxBonus);
        bl.state = 5;
    }
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        if (l.state < 2) {
            l.state = 2;
            l.timer.assign(0);
            l.width = l.targetWidth;
            if (items) laserItems(w, l, cancelItemType_, AutoCollect, true);
        }
        l.hitboxEnd = 0;
    }
    clearCountdown_ = 10;
    return total;
}

void BulletManager::radiusClear(const World& w, const Vec3f& c, float r) {
    (void)w;
    for (Bullet& b : bullets_) {
        if (b.state == 0 || b.state == 5) continue;
        const float dx = b.pos.x - c.x, dy = b.pos.y - c.y, dz = b.pos.z - c.z;
        if (dx * dx + dy * dy + dz * dz > r * r) continue;
        spawn(b.pos, PointBullet, AutoCollect);
        b = Bullet{};
    }
}

void BulletManager::freeze() {
    if (!world_) return;
    for (Bullet& b : bullets_) {
        if (b.state == 0) continue;
        b.velocity = {};
        b.speed = 0.0f;
        b.color = 0;
        b.vm.selectSprite(*world_->anm, b.vm.baseSprite);
    }
}

// ---- lasers ----------------------------------------------------------------------------------

Laser* BulletManager::spawnLaser(const World& w, const Shooter& s, float startOffset, float endOffset, float startLength, float width,
                                 std::int32_t startTime, std::int32_t duration, std::int32_t endTime, std::int32_t hitboxStart,
                                 std::int32_t hitboxEnd, std::uint16_t flags, bool aimed) {
    if (clearCountdown_ != 0 && !(flags & 0x4)) return &lasers_[0];
    Laser* l = nullptr;
    for (Laser& c : lasers_) {
        if (!c.inUse) {
            l = &c;
            break;
        }
    }
    if (!l) {
        dummyLaser_ = Laser{};
        return &dummyLaser_;
    }
    l->beam.start(*w.anm, *w.rng, kS0 + 10 + s.type);
    l->beam.selectSprite(*w.anm, l->beam.activeSprite + s.color);
    l->orb = AnmVm{};
    l->orb.selectSprite(*w.anm, kS0 + 146 + (s.color >= 0 && s.color < 24 ? kTable16[s.color] : 0));
    l->orb.flags |= AnmVm::Visible | AnmVm::Active;
    l->orb.set(AnmVm::Additive, true);
    l->origin = s.pos;
    l->color = s.color;
    l->inUse = true;
    l->angle = s.angle1;
    if (aimed) l->angle += aimAngle(w, s.pos);
    l->flags = flags;
    l->timer.assign(0);
    l->start = startOffset;
    l->end = endOffset;
    l->startLength = startLength;
    l->width = width;
    l->speed = s.speed1;
    l->startTime = startTime;
    l->duration = duration;
    l->endTime = endTime;
    l->hitboxStart = hitboxStart;
    l->hitboxEnd = hitboxEnd;
    l->hideWarning = 0;
    l->state = startTime == 0 ? 1 : 0;
    return l;
}

void BulletManager::stopLaser(Laser& l) {
    if (!l.inUse || l.state >= 2) return;
    l.state = 2;
    l.timer.assign(0);
    l.width = l.targetWidth;
}

void BulletManager::laserItems(const World& w, const Laser& l, int type, int state, bool atOrigin) {
    (void)w;
    if (atOrigin) spawn(l.origin, type, state);
    const float c = std::cos(l.angle), s = std::sin(l.angle);
    for (float o = l.start; l.end > o; o += 32.0f) spawn({c * o + l.origin.x, s * o + l.origin.y, 0.0f}, type, state);
}

void BulletManager::laserCollision(const World& w, Laser& l, const Vec3f& centre, const Vec3f& size, bool graze) {
    w.player->laserTest(w, l.origin, l.angle, centre, size, graze);
}

void BulletManager::updateLasers(const World& w) {
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        l.end = g_timing.speed * l.speed + l.end;
        if (l.startLength < l.end - l.start) l.start = l.end - l.startLength;
        if (l.start < 0.0f) l.start = 0.0f;
        Vec3f size{l.end - l.start, l.width / 2.0f, 0.0f};
        const Vec3f centre{((l.end - l.start) / 2.0f + l.start) + l.origin.x, l.origin.y, 0.0f};
        const float sw = l.beam.sprite ? l.beam.sprite->pixelWidth : 16.0f;
        const float sh = l.beam.sprite ? l.beam.sprite->pixelHeight : 16.0f;
        l.beam.scale = {l.width / sw, (l.end - l.start) / sh};
        l.beam.rotation.z = addNormalizeAngle(kZunPi / 2.0f, l.angle);
        l.beam.set(AnmVm::RotationDirty, true);
        l.beam.set(AnmVm::ScaleDirty, true);
        const float t = l.timer.asFloat();
        auto fadeAlpha = [&]() {
            const float v = (t * 255.0f) / static_cast<float>(l.startTime);
            std::int32_t a = std::isfinite(v) ? static_cast<std::int32_t>(v) : static_cast<std::int32_t>(0x80000000u);
            a = std::min(a, 255);
            l.beam.color = static_cast<std::uint32_t>(a & 0xFF) << 24;
        };
        if (l.state == 0) {
            if (l.flags & 0x1) {
                fadeAlpha();
            } else {
                const std::int32_t ramp = std::min(l.startTime, 30);
                const float wdt = (l.startTime - ramp) < l.timer.current ? (t * l.width) / static_cast<float>(l.startTime) : 1.2f;
                l.targetWidth = wdt;
                l.beam.scale.x = wdt / 16.0f;
                size.x = wdt / 2.0f;
            }
            if (l.timer.current >= l.hitboxStart) laserCollision(w, l, centre, size, l.timer.current % 12 == 0);
            if (l.timer.current >= l.startTime) {
                l.timer.assign(0);
                l.state = 1;
                l.targetWidth = l.width;
            }
        }
        if (l.state == 1) {
            laserCollision(w, l, centre, size, l.timer.current % 12 == 0);
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
            const float ts = l.timer.asFloat();
            if (l.flags & 0x1) {
                const float v = (ts * 255.0f) / static_cast<float>(l.startTime);
                std::int32_t a = std::isfinite(v) ? static_cast<std::int32_t>(v) : static_cast<std::int32_t>(0x80000000u);
                a = std::min(a, 255);
                l.beam.color = static_cast<std::uint32_t>(a & 0xFF) << 24;
            } else if (l.endTime > 0) {
                const float wdt = l.width - (ts * l.width) / static_cast<float>(l.endTime);
                l.beam.scale.x = wdt / 16.0f;
                size.x = wdt / 2.0f;
            }
            if (l.timer.current < l.hitboxEnd) laserCollision(w, l, centre, size, false);
            if (l.timer.current >= l.endTime) {
                l.inUse = false;
                continue;
            }
        }
        if (l.start >= 640.0f) l.inUse = false;
        l.timer.tick();
        l.beam.tick(*w.anm, *w.rng);
    }
}

// ---- items -----------------------------------------------------------------------------------

void BulletManager::spawn(const Vec3f& pos, int type, int state) {
    const World* w = world_;
    if (!w) return;
    if (w->state->powerI() >= 128 && (type == SmallPower || type == BigPower)) type = Cherry;
    Item* it = nullptr;
    for (int probe = 0; probe < kItems; ++probe) {
        const int i = (itemCursor_ + probe) % kItems;
        if (!items_[static_cast<std::size_t>(i)].inUse) {
            it = &items_[static_cast<std::size_t>(i)];
            itemCursor_ = (i + 1) % kItems;
            break;
        }
    }
    if (!it) return;
    it->inUse = true;
    it->pos = pos;
    it->velocity = {0.0f, -2.2f, 0.0f};
    it->type = static_cast<std::int8_t>(type);
    it->state = static_cast<std::uint8_t>(state);
    it->timer.assign(0);
    if (state == Scatter) {
        it->target.x = w->rng->f32InRange(288.0f) + 48.0f;
        it->target.y = w->rng->f32InRange(192.0f) - 64.0f;
        it->target.z = 0.0f;
        it->velocity = pos;
    } else if (state == 3) {
        it->state = AutoCollect;
    } else if (state == 4) {
        it->state = Normal;
    }
    it->vm = AnmVm{};
    it->vm.start(*w->anm, *w->rng, kS1 + 28 + type);
    it->vm.color = 0xFFFFFFFFu;
    it->vm.set(AnmVm::ZWriteDisable, true);
    it->bonus = false;
    it->onScreen = true;
}

void BulletManager::collectAll() {
    for (Item& it : items_) {
        if (!it.inUse) continue;
        it.state = AutoCollect;
        it.velocity = {0.0f, -0.5f, 0.0f};
    }
}

void BulletManager::removeItems() {
    for (Item& it : items_) it.inUse = false;
}

void BulletManager::stopHoming() {
    for (Item& it : items_) {
        if (!it.inUse || it.state != AutoCollect) continue;
        it.state = Normal;
        it.velocity = {0.0f, -0.9f, 0.0f};
    }
}

void BulletManager::convertPowerItems(const World& w, const Item* except) {
    for (Item& it : items_) {
        if (!it.inUse || &it == except || (it.type != SmallPower && it.type != BigPower)) continue;
        if (it.velocity.y > -0.5f) it.velocity = {0.0f, -0.5f, 0.0f};
        if (w.effects) w.effects->spawn(w, 0, it.pos, 1, 0xFFFFFFFFu);
        it.type = Cherry;
        it.vm.start(*w.anm, *w.rng, kS1 + 28 + Cherry);
    }
}

void BulletManager::updateItems(const World& w) {
    const Player& player = *w.player;
    const ShotFile& h = player.header();
    const float k = h.itemBox;
    GameState& g = *w.state;
    bool collected = false;
    itemDrawCount_ = 0;
    for (int i = 0; i < kItems; ++i) {
        Item& it = items_[static_cast<std::size_t>(i)];
        if (!it.inUse) continue;
        bool pickupOnly = false;
        if (it.state == Scatter) {
            if (it.timer.current < 60) {
                const float s = it.timer.asFloat() / 60.0f;
                it.pos = {it.target.x * s + it.velocity.x * (1.0f - s), it.target.y * s + it.velocity.y * (1.0f - s),
                          it.target.z * s + it.velocity.z * (1.0f - s)};
                pickupOnly = true;
            } else {
                it.velocity = {};
                it.state = Normal;
            }
        }
        if (!pickupOnly) {
            const bool border = player.borderFlag() == Player::BorderFlag::Active;
            if (it.state == AutoCollect || ((g.powerI() >= 128 || g.difficulty >= Extra) && player.position().y < h.pocLine) || border) {
                if (player.state() != Player::State::Spawning) {
                    it.velocity = polar(aimAngle(w, it.pos), h.autoCollectSpeed, it.velocity.z);
                    it.state = AutoCollect;
                    if (border) it.bonus = true;
                } else {
                    it.velocity.y = -0.5f;
                    it.state = Normal;
                }
            } else {
                it.velocity.x = 0.0f;
                it.velocity.z = 0.0f;
                if (it.velocity.y < -2.2f) it.velocity.y = -2.2f;
            }
            it.pos.x += it.velocity.x * g_timing.speed;
            it.pos.y += it.velocity.y * g_timing.speed;
            it.pos.z += it.velocity.z * g_timing.speed;
            if (it.pos.y >= kPlayfieldH + 16.0f) {
                it.inUse = false;
                g.decreaseSubrank(3);
                continue;
            }
            if (it.velocity.y < 3.0f) it.velocity.y += 0.03f * g_timing.speed;
            else it.velocity.y = 3.0f;
        }
        if (player.canPickUp() && player.pickupBoxTouches(it.pos, k / 2.0f)) {
            g.eventFlags |= 0x40;
            collect(w, it, i);
            it.inUse = false;
            collected = true;
            continue;
        }
        it.timer.tick();
        if (it.vm.current) it.vm.tick(*w.anm, *w.rng);
        itemDraw_[static_cast<std::size_t>(itemDrawCount_++)] = static_cast<std::int16_t>(i);
    }
    if (collected) w.playSound(21);
}

void BulletManager::collect(const World& w, Item& it, int slot) {
    GameState& g = *w.state;
    const Player& player = *w.player;
    const float poc = player.header().pocLine;
    auto popup = [&](std::uint32_t value, std::uint32_t color, bool small = false) {
        if (w.popups) w.popups->score(it.pos, value, color, small);
    };
    auto levelIndex = [&]() {
        static constexpr int kLevels[] = {8, 16, 32, 48, 64, 80, 96, 128, 999};
        int i = 0;
        while (g.powerI() >= kLevels[i]) ++i;
        return i;
    };
    auto fullPower = [&](bool evenInSpell) {
        if (evenInSpell || !g.spellCardActive) clearAll(w, 1);
        if (w.popups) w.popups->status(PopupSink::FullPowerMode, 0);
        convertPowerItems(w, &it);
    };
    switch (it.type) {
        case SmallPower:
        case BigPower: {
            if (g.powerI() >= 128) {
                if (it.type == SmallPower) {
                    g.fullPowerChain = std::min(g.fullPowerChain + 1, 30);
                    const auto v = static_cast<std::uint32_t>(w.tables ? w.tables->fullPowerChain[static_cast<std::size_t>(g.fullPowerChain)] : 0);
                    g.addScore(v);
                    popup(v, v >= 12800 ? 0xFFFFFF00u : 0xFFFFFFFFu);
                }
            } else {
                const int before = levelIndex();
                if (it.type == SmallPower) g.fullPowerChain = 0;
                w.addPower(it.type == SmallPower ? 1.0f : 8.0f);
                if (g.powerI() >= 128) {
                    w.setPower(128.0f);
                    fullPower(false);
                }
                g.addScore(10);
                if (levelIndex() != before) {
                    if (w.popups) w.popups->powerUp(it.pos);
                    w.playSound(31);
                } else {
                    popup(10, 0xFFFFFFFFu);
                }
            }
            if (it.type == SmallPower) g.increaseSubrank(1);
            break;
        }
        case Point: {
            std::int32_t base = it.pos.y < poc ? 50000 : 50000 - static_cast<std::int32_t>(it.pos.y - poc) * 100;
            if (it.bonus) base = 50000;
            std::int32_t value;
            if (base >= 50000) value = g.cherry > 50000 ? g.cherry : 50000;
            else value = g.cherry > 50000 ? base + (g.cherry - 50000) / 5 : base;
            value -= value % 10;
            popup(static_cast<std::uint32_t>(value), (it.pos.y < poc || it.bonus) ? 0xFFFFFF00u : 0xFFFFFFFFu);
            g.addScore(static_cast<std::uint32_t>(value));
            ++g.stagePointItems;
            ++g.extendPoints;
            g.increaseSubrank(it.pos.y < 128.0f ? 10 : 3);
            w.checkPointExtends();
            break;
        }
        case Bomb:
            if (g.bombsI() < GameState::kMaxBombs) w.addBombs(1.0f);
            g.increaseSubrank(5);
            break;
        case FullPower:
            if (g.powerI() < 128) {
                fullPower(true);
                w.playSound(31);
                if (w.popups) w.popups->powerUp(it.pos);
            }
            w.setPower(128.0f);
            g.addScore(1000);
            popup(1000, 0xFFFFFFFFu);
            break;
        case Extend: w.grantExtend(); break;
        case PointBullet: {
            std::int32_t v;
            if (player.bombing()) v = 100;
            else {
                v = static_cast<std::int32_t>(g.totalGraze / 40) * 10 + 300;
                if (v <= 0) v = 10;
            }
            popup(static_cast<std::uint32_t>(v), 0xFFFFFFFFu, true);
            g.addScore(static_cast<std::uint32_t>(v));
            if (!g.bombActive) w.cherryAddWithGauge(20);
            else if ((slot & 1) == 0) w.cherryAddWithGauge(10);
            else w.cherryAdd(10);
            break;
        }
        case Cherry: {
            if (g.cherry >= g.cherryMax) {
                std::int32_t v = (it.pos.y < poc || it.bonus) ? 50000 : 50000 - static_cast<std::int32_t>(it.pos.y - poc) * 100;
                v -= v % 10;
                popup(static_cast<std::uint32_t>(v), (it.pos.y < poc || it.bonus) ? 0xFFFFFF00u : 0xFFFFFFFFu);
                g.addScore(static_cast<std::uint32_t>(v));
            }
            const std::int32_t gain = 1000 + 100 * g.spellsCaptured;
            if (g.cherry < g.cherryMax) popup(static_cast<std::uint32_t>(gain), 0xFFFF4040u);
            w.cherryAddWithGauge(gain);
            break;
        }
        case SmallCherry:
            w.cherryAddWithGauge(30);
            w.cherryAdd(70);
            break;
        case Star: {
            std::int32_t v = static_cast<std::int32_t>(g.totalGraze / 40) * 10 + 300;
            if (v <= 0) v = 10;
            const bool atMax = g.cherry >= g.cherryMax;
            if (atMax) popup(static_cast<std::uint32_t>(v), 0xFFFFFFFFu);
            g.addScore(static_cast<std::uint32_t>(v));
            if (!atMax) popup(100, 0xFFFF4040u);
            w.cherryAddWithGauge(100);
            break;
        }
        default: break;
    }
}

// ---- drawing ---------------------------------------------------------------------------------

void BulletManager::draw(SpriteSink& sink, const AnmDrawState& state) {
    if (!world_) return;
    const AnmLibrary& lib = *world_->anm;
    for (Laser& l : lasers_) {
        if (!l.inUse) continue;
        const float mid = (l.end - l.start) / 2.0f + l.start;
        const float c = std::cos(l.angle), s = std::sin(l.angle);
        l.beam.position = {c * mid + l.origin.x + kPlayfieldX, s * mid + l.origin.y + kPlayfieldY, 0.05f};
        drawVm(sink, l.beam, state);
        if ((l.start < 16.0f || l.speed == 0.0f) && (l.hideWarning == 0 || l.state != 0)) {
            l.orb.position = {c * l.start + l.origin.x + kPlayfieldX, s * l.start + l.origin.y + kPlayfieldY, 0.05f};
            l.orb.color = l.beam.color | 0xFF000000u;
            float sc = (l.width / 10.0f) * ((16.0f - l.start) / 16.0f);
            if (sc <= 0.0f) sc = l.width / 10.0f;
            l.orb.scale = {sc, sc};
            drawVm(sink, l.orb, state);
        }
    }
    for (int n = 0; n < itemDrawCount_; ++n) {
        Item& it = items_[static_cast<std::size_t>(itemDraw_[static_cast<std::size_t>(n)])];
        if (!it.inUse) continue;
        it.vm.position = {kPlayfieldX + it.pos.x, kPlayfieldY + it.pos.y, 0.01f};
        if (it.pos.y < -8.0f) {
            it.vm.position.y = kPlayfieldY + 8.0f;
            if (it.onScreen) {
                it.vm.selectSprite(lib, kS1 + 14 + it.type);
                it.onScreen = false;
                it.vm.set(AnmVm::ZWriteDisable, true);
            }
            const std::int32_t a = std::max(64, 255 - static_cast<std::int32_t>((8.0f - it.pos.y) * 255.0f / 128.0f));
            it.vm.color = (it.vm.color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(a) << 24);
        } else if (!it.onScreen) {
            it.vm.selectSprite(lib, kS1 + 4 + it.type);
            it.onScreen = true;
            it.vm.color = 0xFFFFFFFFu;
        }
        drawVm(sink, it.vm, state);
    }
    for (int bucket = 0; bucket < 6; ++bucket) {
        for (int n = bucketCount_[static_cast<std::size_t>(bucket)] - 1; n >= 0; --n) {
            Bullet& b = bullets_[static_cast<std::size_t>(buckets_[static_cast<std::size_t>(bucket)][static_cast<std::size_t>(n)])];
            if (b.state == 0) continue;
            AnmVm& vm = b.state >= 2 && b.state <= 4 ? b.spawnVm : b.state == 5 ? b.cancelVm : b.vm;
            vm.position = {kPlayfieldX + b.pos.x, kPlayfieldY + b.pos.y, 0.05f};
            vm.color |= 0x00FFFFFFu;
            if (vm.autoRotate != 0) {
                vm.rotation.z = addNormalizeAngle(kZunPi / 2.0f, b.angle);
                vm.set(AnmVm::RotationDirty, true);
            }
            drawVm(sink, vm, state);
        }
    }
}

}  // namespace touhou::th07
