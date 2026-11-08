// The 24 boss-specific ECL "ex" routines (opcodes 121 and 122, spec section 10). Most reach
// into the bullet pool: they test bullets by state, colour, sprite and the per-bullet tag
// ("state2"), fire temporary patterns at them, give them commands, or free them. Bullet
// scans visit the 1024 slots in order; "live" is neither free nor cancelling, "non-free"
// includes cancelling bullets.

#include <format>

#include "th07/Background.hpp"
#include "th07/Bgm.hpp"
#include "th07/Bullets.hpp"
#include "th07/Effects.hpp"
#include "th07/Enemies.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {

using Bullet = BulletManager::Bullet;

constexpr int kS0 = 0x200;  // etama.anm section 0 (bullet sprites)
constexpr float kPi = kZunPi;
constexpr float kHalfPi = kZunPi / 2.0f;
constexpr float kThreeHalfPi = (kZunPi * 3.0f) / 2.0f;

bool live(const Bullet& b) { return b.state != 0 && b.state != 5; }
bool nonFree(const Bullet& b) { return b.state != 0; }
bool arrowhead(const Bullet& b) { return b.vm.activeSprite >= kS0 + 96 && b.vm.activeSprite <= kS0 + 111; }
bool butterfly(const Bullet& b) { return b.vm.activeSprite >= kS0 + 120 && b.vm.activeSprite <= kS0 + 127; }
bool lightBlueButterfly(const Bullet& b) { return b.vm.activeSprite == kS0 + 124; }

// The bullet's current sprite height (the original reads it without checking the sprite).
float spriteHeight(const Bullet& b, int routine) {
    if (b.vm.sprite) return b.vm.sprite->pixelHeight;
    eclWarnOnce(std::format("ex routine {}: a bullet without a sprite (the original would crash); height 0", routine));
    return 0.0f;
}

// The temporary records the routines build: zero-filled, per-bullet sound -1.
Shooter blankShooter() {
    Shooter s;
    s.count1 = s.count2 = 0;
    s.sound = 0;
    s.bulletSound = -1;
    return s;
}

// The command helpers OR their type bit into the record's flags (spec 9.1). Field a is the
// command's "speed" value, b its "angle".
void addCommand(Shooter& s, int slot, std::uint32_t type, std::int32_t duration, std::int32_t loops, float a, float b) {
    BulletCommand& c = s.commands[static_cast<std::size_t>(slot)];
    c = BulletCommand{};
    c.type = type;
    c.duration = duration;
    c.loops = loops;
    c.a = a;
    c.b = b;
    s.flags |= type;
}

// The same on a live bullet: the command goes into the bullet's own list, its type into the
// bullet's allowed set, and the bullet's command cursor moves to that slot. [AMBIGUOUS]
// Neither spec says what happens to the cursor. Left alone, the command would never start
// on bullets whose earlier commands already ran, and the shipped data does call routine 1
// on such bullets (stage 2's Lunatic Chen); routine 14's "clear command 1" also only makes
// sense if the chain restarts at slot 0. So the helper restarts the chain at the slot.
void addBulletCommand(Bullet& b, int slot, std::uint32_t type, std::int32_t duration, float a, float bb) {
    BulletCommand& c = b.commands[static_cast<std::size_t>(slot)];
    c = BulletCommand{};
    c.type = type;
    c.duration = duration;
    c.a = a;
    c.b = bb;
    b.allowed |= type;
    b.cursor = slot;
}

// The rotated rectangle test of routines 7 and 8 (a point in the laser's own frame).
bool insideLaser(const Laser& l, float c, float s, float width, const Vec3f& p) {
    const float length = l.end - l.start;
    const float cx = (l.start + length / 2.0f) + l.origin.x, cy = l.origin.y;
    const float dx = p.x - l.origin.x, dy = p.y - l.origin.y;
    const float lx = dx * c + dy * s + l.origin.x, ly = dy * c - dx * s + l.origin.y;
    return !(lx < cx - length / 2.0f || lx > cx + length / 2.0f || ly < cy - width / 2.0f || ly > cy + width / 2.0f);
}

}  // namespace

void EnemyManager::exRoutine(Enemy& e, int routine, const std::uint8_t* ins) {
    const World& wd = w();
    GameState& g = *wd.state;
    ZunRng& rng = *wd.rng;
    BulletManager* bm = wd.bulletManager;
    const std::int32_t a = eclI32(ins + 16);  // word 1, always literal
    const int difficulty = g.difficulty;
    auto shake = [&](int duration, float px) {
        if (wd.shakes) wd.shakes->shake(duration, px, 0.0f);
    };
    auto pulse = [&]() { eclWarnOnce("screen pulses (ex routines) are not drawn yet"); };
    auto fire = [&](const Shooter& s) {
        if (bm) bm->fire(wd, s);
    };
    if (!bm && routine != 0 && routine != 3 && routine != 5 && routine != 15 && routine != 19 && routine != 20) return;

    switch (routine) {
        case 0: {
            Enemy* b = bossAt(a);
            if (!b) {
                eclWarnOnce("ex routine 0 follows a missing boss (the original crashes)");
                break;
            }
            e.pos = b->pos;
            e.velocity = b->velocity;
            e.angle = b->angle;
            e.moveDisabled = true;  // never cleared again [QUIRK]
            break;
        }
        case 1: {
            shake(30, 12.0f);
            pulse();
            float angle = 0.0f;  // uninitialised in the original until a matching colour [QUIRK]
            bool angleSet = false;
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!live(b) || !b.vm.sprite || b.tag != 0) continue;
                if ((a == 1 && b.color != 8) || (a == 2 && b.color != 4)) continue;
                if (b.color == 2 || b.color == 4) {
                    angle = -kPi / (rng.f32InRange(60.0f) + 180.0f);
                    angleSet = true;
                } else if (b.color == 6 || b.color == 8) {
                    angle = kPi / (rng.f32InRange(60.0f) + 180.0f);
                    angleSet = true;
                } else if (!angleSet) {
                    eclWarnOnce("ex routine 1: a bullet colour without a turn angle first (uninitialised in the original); 0 used");
                }
                b.speed = 0.3f;
                for (int c = 0; c < 5; ++c) b.commands[static_cast<std::size_t>(c)] = BulletCommand{};
                if (difficulty < Lunatic) addBulletCommand(b, 0, 0x20, 60, 1.0f / 60.0f, angle);
                else addBulletCommand(b, 0, 0x20, 240, 1.0f / 190.0f, angle);
                b.tag = 1;
            }
            break;
        }
        case 2: {
            float radius;
            switch (a) {
                case 0:
                    radius = 128.0f;
                    shake(32, 12.0f);
                    pulse();
                    break;
                case 1: radius = 192.0f; break;
                case 2: radius = 256.0f; break;
                case 3: radius = 999.0f; break;
                default:
                    eclWarnOnce(std::format("ex routine 2 with argument {} (radius uninitialised in the original); nothing done", a));
                    return;
            }
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!live(b) || !b.vm.sprite || b.color != 2) continue;
                const float dx = b.pos.x - e.pos.x, dy = b.pos.y - e.pos.y;
                if (!(std::sqrt(dx * dx + dy * dy) < radius)) continue;
                Shooter s = blankShooter();
                s.pos = b.pos;
                s.type = 0;
                s.color = 6;
                s.angle1 = 0.0f;
                s.angle2 = -kPi;
                s.speed1 = 0.7f;
                s.count1 = 2;
                s.count2 = 1;
                s.aimMode = 6;
                s.flags = 2;
                addCommand(s, 0, 0x10, 180, 0, rng.f32InRange(0.005f) + 0.013f, kHalfPi);  // flags 0x12
                fire(s);
                bm->freeBullet(i);
            }
            break;
        }
        case 3: break;
        case 4: {
            e.ctx.args.floatA[0] = -999.0f;
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!live(b) || spriteHeight(b, 4) < 60.0f) continue;
                e.ctx.args.floatA[0] = b.pos.x;
                e.ctx.args.floatA[1] = b.pos.y;
                if (wd.effects) wd.effects->spawn(wd, 2, b.pos, 1, 0xFFFFFFFFu);
                bm->freeBullet(i);
                break;
            }
            break;
        }
        case 5: {
            Enemy* b = bossAt(0);
            if (!b) {
                eclWarnOnce("ex routine 5 orbits a missing boss (the original crashes)");
                break;
            }
            e.moveOrigin = b->pos;
            e.orbitRadius = b->orbitRadius;
            e.orbitAngularVelocity = b->orbitAngularVelocity;
            break;
        }
        case 6: {
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!live(b) || !b.vm.sprite) continue;
                if (!((a == 0 && b.color == 6) || (a == 1 && b.color == 15) || (a == 2 && b.color == 2))) continue;
                Shooter s = blankShooter();
                s.pos = b.pos;
                s.type = 6;
                s.color = 15;
                s.angle1 = addNormalizeAngle(b.angle, kPi);
                s.angle2 = kPi / 6.0f;
                s.speed1 = b.speed * 1.1f;
                // Extra counts as "Lunatic and above" for the counts, "not Lunatic" for the flags [QUIRK].
                if (difficulty < Lunatic) {
                    s.count1 = 4;
                } else {
                    s.count1 = 2;
                    s.angle2 = kHalfPi;
                }
                s.count2 = 1;
                s.flags = 2;
                addCommand(s, 0, 0x2000, 130, 0, 0.0f, 0.0f);
                if (a == 0) {
                    s.flags = 0x2002;
                } else if (a == 1) {
                    s.flags = difficulty == Lunatic ? 0x2002 : 2;
                    s.color = 2;
                } else {
                    s.flags = 2;
                    s.color = 10;
                }
                s.aimMode = 1;
                fire(s);
                s.angle2 = kPi / 3.0f;
                s.flags = a == 0 ? 0x2000 : (a == 1 && difficulty == Lunatic) ? 0x2000 : 0;
                s.speed1 = b.speed * 0.7f;
                s.count1 = 2;
                fire(s);
                s.speed1 = b.speed * 0.85f;
                s.count1 = 1;
                fire(s);
                bm->freeBullet(i);
            }
            break;
        }
        case 7:
        case 8: {
            for (int li = 0; li < BulletManager::kLasers; ++li) {
                Laser& l = bm->laser(li);
                if (!l.inUse || l.state >= 2) continue;
                // Routine 7: only laser 0 on even frames and laser 1 on odd ones [QUIRK].
                if (routine == 7 ? e.timer.current % 2 != li : e.timer.current % 3 != li % 3) continue;
                const float c = zcos(l.angle), s = zsin(l.angle);
                const float width = routine == 7 ? l.width : l.width * 1.5f;
                for (int i = 0; i < BulletManager::kBullets; ++i) {
                    Bullet& b = bm->bullet(i);
                    if (!live(b) || !b.vm.sprite) continue;
                    if (routine == 8 && (b.tag == li + 1 || b.tag < 0)) continue;
                    if (!insideLaser(l, c, s, width, b.pos)) continue;
                    if (routine == 7) {
                        if (b.tag > 0) --b.tag;
                        if (b.tag != 0) continue;
                        if (b.speed > 0.5f) b.speed -= 0.1f;
                        const float dot = c * b.velocity.y + s * b.velocity.x;
                        b.angle = addNormalizeAngle(l.angle, dot >= 0.0f ? kHalfPi : -kHalfPi);
                        fromAngle(b.velocity, b.angle, g_timing.speed * b.speed);
                        b.tag = 10;
                        bm->retypeBullet(wd, i, 5);
                    } else {
                        const float f = difficulty < Hard ? rng.f32InRange(0.3f) + 0.7f : rng.f32InRange(0.4f) + 0.8f;
                        b.speed *= f;
                        const Vec3f dir{-s, c, 0.0f};
                        const float dot = dir.x * b.velocity.x + dir.y * b.velocity.y;
                        b.velocity.x = dot >= 0.0f ? dir.x : -dir.x;
                        b.velocity.y = dot >= 0.0f ? dir.y : -dir.y;
                        bm->retypeBullet(wd, i, 5);
                        b.angle = zatan2(b.velocity.y, b.velocity.x);
                        fromAngle(b.velocity, b.angle, b.speed);  // not scaled by the game speed
                        b.tag = static_cast<std::int16_t>(difficulty < Hard ? -1 : li + 1);
                    }
                }
            }
            break;
        }
        case 9: {
            shake(80, 8.0f);
            if (wd.effects) {
                for (int i = 0; i < Effects::kNormal; ++i) {
                    Effect& fx = wd.effects->normal(i);
                    if (fx.id == 30) fx.accel.z = -0.01f;
                }
            }
            break;
        }
        case 10: {
            g_timing.speed = 1.0f / static_cast<float>(a);
            if (wd.background) {
                wd.background->spellInterrupt(0, 2);
                wd.background->spellInterrupt(1, 2);
            }
            const float m = g_timing.speed;
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b)) continue;
                b.velocity = b.velocity * m;
                b.vm.baseSprite = b.vm.activeSprite;  // every bullet [QUIRK]
                if (arrowhead(b)) b.vm.selectSprite(*wd.anm, kS0 + 111);
            }
            break;
        }
        case 11: {
            const float f = 1.0f / g_timing.speed;
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b)) continue;
                b.velocity = b.velocity * f;
                if (arrowhead(b)) b.vm.selectSprite(*wd.anm, b.vm.baseSprite);
            }
            // The restore's own argument decides the sticky force-integer flag (verified on the
            // binary; the shipped scripts pass 0 or 1, which never set it).
            if (1.0f / static_cast<float>(a) < 1.0f) g_timing.forceInteger = true;
            g_timing.speed = 1.0f;
            if (wd.background) {
                wd.background->spellInterrupt(0, 1);
                wd.background->spellInterrupt(1, 1);
            }
            break;
        }
        case 12:
        case 21: {
            pulse();
            const int n = routine == 21 ? 15 : difficulty == Easy ? 10 : difficulty == Normal ? 18 : difficulty == Hard ? 22 : 25;
            const float band = routine == 21 ? (difficulty == Hard ? 128.0f : 180.0f) : (difficulty < Hard ? 64.0f : 48.0f);
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b) || !(spriteHeight(b, routine) > 48.0f)) continue;
                if (routine == 12 ? !(std::fabs(b.pos.y - e.pos.y) < band) : !(e.pos.y - band < b.pos.y && b.pos.y < band + e.pos.y)) {
                    continue;
                }
                for (int j = 0; j < n; ++j) {
                    Shooter s = blankShooter();
                    s.pos = b.pos;
                    s.pos.x += rng.f32InRange(32.0f) - 16.0f;
                    s.pos.y += rng.f32InRange(32.0f) - 16.0f;
                    const std::uint16_t pick = rng.u16InRange(3);
                    s.type = static_cast<std::int16_t>(pick == 0 ? 0 : pick == 1 ? 3 : 7);
                    if (routine == 12) s.color = static_cast<std::int16_t>(pick == 2 ? 1 : 2);
                    else s.color = static_cast<std::int16_t>(pick == 2 ? 2 : 4);
                    if (a == 0) s.angle1 = rng.f32InRange(kThreeHalfPi) - kHalfPi;
                    else s.angle1 = addNormalizeAngle(rng.f32InRange(kThreeHalfPi), kPi / 4.0f);
                    s.speed1 = 0.1f;
                    s.count1 = s.count2 = 1;
                    s.flags = routine == 12 && (j & 1) ? 2 : 0;
                    s.aimMode = 1;
                    addCommand(s, 0, 0x20, 100, 0, rng.f32InRange(0.008f) + 0.01f, 0.0f);
                    fire(s);
                }
                bm->freeBullet(i);
            }
            break;
        }
        case 13: {
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b) || b.tag != 0) continue;
                if (!(e.pos.y < b.pos.y && b.pos.y < 352.0f && std::fabs(b.pos.x - e.pos.x) < 16.0f)) continue;
                addBulletCommand(b, 0, 0x20, 160, -b.speed / 180.0f, (i & 1) ? kPi / 60.0f : -kPi / 60.0f);
                b.tag = 1;
            }
            break;
        }
        case 14: {
            pulse();
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b) || b.tag != 1) continue;
                addBulletCommand(b, 0, 0x10, 90, 2.0f / 75.0f, angleToPlayer(b.pos));
                b.commands[1] = BulletCommand{};
                b.tag = 2;
            }
            break;
        }
        case 15: pulse(); break;
        case 16: {
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b) || b.tag != 0 || !butterfly(b)) continue;
                Shooter s = blankShooter();
                s.pos = b.pos;
                s.type = 0;
                s.color = 6;
                s.angle1 = addNormalizeAngle(b.angle, kPi);
                s.angle2 = kPi / 8.0f;
                s.speed1 = e.ctx.args.floatA[1];
                s.count1 = 5;
                s.count2 = 1;
                s.flags = 2;
                s.aimMode = 1;
                fire(s);  // the butterfly stays
            }
            break;
        }
        case 17: {
            EclArgs copy = e.ctx.args;
            float offset = -kPi;
            pulse();
            const int sub = e.ctx.sub + 1;
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b)) continue;
                if (b.tag == 0 && lightBlueButterfly(b)) {
                    copy.floatA[0] = b.angle;
                    copy.floatA[7] = offset;
                    offset += kPi / 4.0f;
                    spawnWithContext(sub, b.pos, 1, -2, 10, copy);
                    bm->freeBullet(i);
                } else if (butterfly(b)) {
                    bm->freeBullet(i);
                }
            }
            break;
        }
        case 18: {
            std::int32_t n = 0;
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                const Bullet& b = bm->bullet(i);
                if (nonFree(b) && b.tag == 0 && lightBlueButterfly(b)) ++n;
            }
            e.ctx.args.intA[0] = n;
            break;
        }
        case 19:
            if (music_) music_->fadeOut(3.0f);
            break;
        case 20:
            if (music_) {
                if (stage_ == 6) music_->playLoaded(2);
                else music_->play("bgm/th07_13b.mid");
            }
            break;
        case 22:
        case 23: {
            const std::int32_t count = e.timer.current;
            if (routine == 22 ? count % 3 == 0 : count % 3 == 2) return;
            const bool m = routine == 22 ? static_cast<std::uint32_t>(count) % 2 != 0 : count % 3 != 0;
            Shooter s = blankShooter();  // reused across bullets within one call
            for (int i = 0; i < BulletManager::kBullets; ++i) {
                Bullet& b = bm->bullet(i);
                if (!nonFree(b) || (b.active & 0x40) || !(b.pos.y < 320.0f) || !(spriteHeight(b, routine) > 60.0f)) continue;
                s.pos = b.pos;
                s.type = static_cast<std::int16_t>(m ? 1 : 3);
                if (routine == 22) s.color = static_cast<std::int16_t>(b.color == 1 ? 6 : 2);
                else s.color = static_cast<std::int16_t>(b.color == 2 ? 10 : 13);
                s.angle1 = rng.f32InRange(kZunTwoPi) - kPi;
                s.angle2 = -kPi;
                s.speed1 = m ? 1.2f : 0.8f;
                s.count1 = static_cast<std::int16_t>(routine == 23 || m ? 1 : 2);
                s.count2 = 1;
                s.flags = 0x208;
                s.aimMode = 3;
                s.bulletSound = 25;
                if (m) {
                    if (routine == 22) addCommand(s, 0, 0x80, 60, 1, 0.0f, 3.1f);  // flags 0x288
                    else addCommand(s, 0, 0x80, 40, 1, 0.0f, 2.9f);
                }
                fire(s);
            }
            break;
        }
        default:
            eclWarnOnce(std::format("ex routine {} (only 0-23 exist)", routine));
            break;
    }
}

}  // namespace touhou::th07
