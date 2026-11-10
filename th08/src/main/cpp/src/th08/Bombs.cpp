// Imperishable Night's bombs, Last Spells and the Dissolve spell
// (claude/th08-specs/spec_player_bullets_items.md §9 and spec_bombs_familiars.md §2-10): the
// bomb step of the player pass, the common start-up, and the 17 spells' per-frame routines and
// draws. The routines spawn the damage and cancel regions that do the bomb's work, effects
// (some of which draw RNG at spawn) and screen shakes (which draw RNG every frame they run).

#include <algorithm>
#include <cmath>

#include "gfx/SpriteSink.hpp"
#include "th08/Bullets.hpp"
#include "th08/Ecl.hpp"
#include "th08/Effects.hpp"
#include "th08/Enemies.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Gui.hpp"
#include "th08/Player.hpp"
#include "th08/ScreenEffects.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {

constexpr std::uint32_t kWhite = 0xFFFFFFFFu;

float len(const Vec3f& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// randSigned(r): u32 / 2^31 - 1, times r (one u32 draw).
float signedRand(ZunRng& rng, float range) { return (static_cast<float>(rng.u32()) / 2147483648.0f - 1.0f) * range; }

// The strip effect the Last Spells of Marisa and Remilia fire (special effect 53): the VM's
// position, scale, alpha and colour interpolated over 30 frames; the ring update reads them
// as radius, width, second radius and angle.
void stripInterpolations(AnmVm& vm, const Vec3f& posEnd, const Vec2f& scaleStart, const Vec2f& scaleEnd) {
    vm.set(AnmVm::UseOffset, false);
    vm.position = {};
    vm.posStart = {};
    vm.posEnd = posEnd;
    vm.progress[AnmVm::Position].reset();
    vm.duration[AnmVm::Position].assign(30);
    vm.ease[AnmVm::Position] = 0;
    vm.scale = scaleStart;
    vm.scaleStart = scaleStart;
    vm.scaleEnd = scaleEnd;
    vm.progress[AnmVm::Scale].reset();
    vm.duration[AnmVm::Scale].assign(30);
    vm.ease[AnmVm::Scale] = 0;
    vm.color = kWhite;
    vm.colorStart = kWhite;
    vm.colorEnd = 0x00FF0000u;
    vm.progress[AnmVm::Alpha].reset();
    vm.duration[AnmVm::Alpha].assign(30);
    vm.ease[AnmVm::Alpha] = 0;
    vm.progress[AnmVm::Color].reset();
    vm.duration[AnmVm::Color].assign(30);
    vm.ease[AnmVm::Color] = 0;
}

}  // namespace

// The spell table (spec 9.2): teams have the human's and the youkai's bomb and Last Spell,
// solos their own for both sides; index 4 is the Dissolve spell for everyone.
Player::Spell Player::spellFor(int character, int index) {
    if (index >= 4) return Spell::Dissolve;
    const int person = character < 4 ? character * 2 + (index & 1) : character - 4;
    return static_cast<Spell>(person * 2 + (index >= 2 ? 1 : 0));
}

// ---- the bomb step (spec 9.1) ------------------------------------------------------------

void Player::bombStep(const World& w, std::uint16_t input) {
    GameState& g = *w.state;
    // 1. The Dissolve auto-trigger (a hit during a boss's Last Spell).
    if (dissolveTrigger_ && countdown_ == 1) {
        startBomb(w, true);
        return;
    }
    // 2.
    if (lockout_ != 0) --lockout_;
    // 3. A bomb is running: its end, or its routine for this frame; the gauge push every frame.
    if (bomb_.active) {
        if (bomb_.timer.current >= bomb_.duration) {
            endBomb(w);
        } else {
            runBomb(w, bomb_.timer.current);
            bomb_.timer.tick();
            bombTimer_ = bomb_.timer;
        }
        if (bomb_.index < 4 && bomb_.duration > 0) {
            const int push = 26000 / bomb_.duration;
            g.addGauge((bomb_.index & 1) ? push : -push, true, true);
        }
        return;
    }
    // 4. A new bomb while Bomb is held.
    if ((input & BtnBomb) && !w.dialogue && countdown_ != 0 && g.bombsI() >= 1 && lockout_ == 0) {
        if ((w.mode & ModeDissolve) || (w.mode & ModeSpellPractice)) {
            if (!(prevInput_ & BtnBomb)) w.playSound(41);  // refused: a buzz on a fresh press
            bombingFlag_ = false;
            return;
        }
        startBomb(w, false);
        return;
    }
    bombingFlag_ = false;
}

void Player::startBomb(const World& w, bool forced) {
    GameState& g = *w.state;
    ZunRng& rng = *w.rng;
    dissolveTrigger_ = false;
    int index;
    if (w.mode & ModeDissolve) {
        index = 4;  // no cost
    } else {
        vm_.set(AnmVm::UseAltColor, false);
        if (deathEffect_) deathEffect_->inUse = false;
        deathEffect_ = nullptr;
        w.mode &= ~static_cast<std::uint32_t>(ModeSpellFreeze);
        // The side in front bombs; a Last Spell is the other character's (focus byte 2 before
        // the first input step gives the human Last Spell [QUIRK]).
        index = focus_;
        const bool last = lastSpellArmed_;
        if (last) index = 1 - index + 2;
        if (last) {
            if (forced || g.bombsI() < 2) {
                bomb_.cost = g.bombsI();
                g.bombs = 0.0f;
            } else {
                bomb_.cost = 2;
                g.bombs -= 2.0f;
                antiTamper(rng);
            }
            ++g.lastSpellsUsed;
        } else {
            ++g.bombsUsedGame;
            g.bombs -= 1.0f;
            antiTamper(rng);
        }
        g.bombsUsed += 1.0f;
        antiTamper(rng);
    }
    lastSpellArmed_ = false;
    bomb_.active = bombActive_ = true;
    bomb_.index = bombIndex_ = index;
    bomb_.spell = spellFor(character_, index);
    bombingFlag_ = true;
    bomb_.timer.assign(0);
    bomb_.duration = 999;
    runBomb(w, 0);
    bomb_.timer.tick();
    bombTimer_ = bomb_.timer;
    g.decreaseSubrank(200);
    if (w.enemies) w.enemies->bombUsed();
    countdown_ = std::min(countdown_ + 6, static_cast<std::int32_t>(header().deathbombCap));
}

void Player::endBomb(const World& w) {
    bomb_.active = bombActive_ = false;
    moveMulX_ = moveMulY_ = 1.0f;
    if (bomb_.spell == Spell::Dissolve) {
        w.mode &= ~static_cast<std::uint32_t>(ModeDissolve);
        if (w.enemies) w.enemies->dissolveEnd();
        if (w.screen) w.screen->flash(30, 1, kWhite);
    }
}

// The common start-up (0x40BE30, spec 9.3): the duration, the invulnerability, every
// projectile off, every item collected.
void Player::bombStartUp(const World& w, std::int32_t duration, std::int32_t invulnerable) {
    bomb_.duration = duration;
    state_ = State::Invulnerable;
    stateTimer_.assign(invulnerable);
    if (w.effects) bombEffect_ = &w.effects->spawnSpecial(w, 23, pos_, 0, 1, 0xFFFF4040u);
    for (Projectile& p : *projectiles_) p.state = 0;
    if (w.bullets) w.bullets->collectAll();
    bomb_.start = pos_;
}

void Player::runBomb(const World& w, std::int32_t t) {
    switch (bomb_.spell) {
        case Spell::ReimuBomb: reimu(w, t, false); break;
        case Spell::ReimuLast: reimu(w, t, true); break;
        case Spell::YukariBomb: yukari(w, t, false); break;
        case Spell::YukariLast: yukari(w, t, true); break;
        case Spell::MarisaBomb: marisa(w, t, false); break;
        case Spell::MarisaLast: marisa(w, t, true); break;
        case Spell::AliceBomb: alice(w, t, false); break;
        case Spell::AliceLast: alice(w, t, true); break;
        case Spell::SakuyaBomb: sakuya(w, t, false); break;
        case Spell::SakuyaLast: sakuya(w, t, true); break;
        case Spell::RemiliaBomb: remilia(w, t, false); break;
        case Spell::RemiliaLast: remilia(w, t, true); break;
        case Spell::YoumuBomb: youmu(w, t, false); break;
        case Spell::YoumuLast: youmu(w, t, true); break;
        case Spell::YuyukoBomb: yuyuko(w, t, false); break;
        case Spell::YuyukoLast: yuyuko(w, t, true); break;
        case Spell::Dissolve:
            if (t == 0) dissolve(w);
            break;
    }
}

// ---- Reimu (spec_bombs 2) ----------------------------------------------------------------

void Player::reimu(const World& w, std::int32_t t, bool last) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    auto moveRegions = [&](const Projectile& p) {
        if (p.cancelRegion >= 0) region(p.cancelRegion).centre = p.pos;
        if (p.damageRegion >= 0) region(p.damageRegion).centre = p.pos;
    };
    auto deactivate = [&](const Projectile& p) {
        if (p.cancelRegion >= 0) region(p.cancelRegion).active = false;
        if (p.damageRegion >= 0) region(p.damageRegion).active = false;
    };
    auto aftermath = [&](Projectile& p) {
        if (w.effects) w.effects->spawn(w, 6, p.pos, 8, kWhite);
        p.state = 2;
        p.vms[0].interrupt(1);
        w.playSound(15);
        if (w.screen) w.screen->shake(16, 8.0f, 0.0f);
    };
    if (t == 0) {
        bombStartUp(w, 200, 260);
        if (w.effects) w.effects->spawn(w, 12, pos_, 1, 0xFF4040FFu);
        float a = -kZunPi;
        for (int i = 0; i < 16; ++i) {
            Projectile& p = proj(i);
            p.vms[0].start(lib, rng, kSlotPlayer, 19);
            p.f10 = a;
            a += 0.39269909f;
            p.pos = p.centre = pos_;
            p.f08 = 0.0f;
            p.state = 1;
            p.cancelRegion = cancelCircle(pos_, 96.0f, 0.0f, 200, 6);
            p.damageRegion = damageCircle(p.pos, 64.0f, 0.0f, 5, 200);
            Region& d = region(p.damageRegion);
            d.interval = 2, d.cap = 200, d.noSpark = true;
        }
        if (last) bomb_.scratch = 0;
        w.playSound(13);
    }
    if (!last) {
        // The spiral: 40 frames out from the start point (x from the sine [QUIRK]).
        if (t < 40) {
            for (int i = 0; i < 16; ++i) {
                Projectile& p = proj(i);
                const Vec3f old = p.pos;
                p.f10 = addNormalizeAngle(p.f10, (i & 1) ? 0.05235988f : -0.05235988f);
                p.pos.x = std::sin(p.f10) * p.f08 + p.centre.x;
                p.pos.y = std::cos(p.f10) * p.f08 + p.centre.y;
                p.f08 += 3.2f;
                p.vec = p.pos - old;
            }
        }
        // Homing from t = 40, until the cap is dealt or t = 170.
        if (t == 40) {
            for (int i = 0; i < 16; ++i) {
                Projectile& p = proj(i);
                p.f0C = len(p.vec);
                p.f10 = std::atan2(p.vec.y, p.vec.x);
                p.counter = 0;
                p.f08 = 8.0f;
            }
        }
        if (t >= 40) {
            for (int i = 0; i < 16; ++i) {
                Projectile& p = proj(i);
                if (p.state == 1) {
                    const Vec3f target = target_.x > -100.0f ? target_ : pos_;
                    const Vec3f d = target - p.pos;
                    float l = len(d) / (p.f08 / 8.0f);
                    if (l < 1.0f) l = 1.0f;
                    const Vec3f v{d.x / l + p.vec.x, d.y / l + p.vec.y, d.z / l + p.vec.z};
                    const float m = len(v);
                    float s = m > 10.0f ? 10.0f : m;
                    if (s < 1.0f) s = 1.0f;
                    p.vec = {v.x * s / m, v.y * s / m, v.z * s / m};
                    p.f08 = s;
                    cancelCircle(p.pos, 128.0f, 0.0f, 0, 6);
                    const bool capped = p.damageRegion >= 0 && region(p.damageRegion).dealt >= 200;
                    if (capped || t >= 170) {
                        deactivate(p);
                        cancelCircle(pos_, 64.0f, 4.2666669f, 30, 6);  // at the player [QUIRK]
                        const int d2 = damageCircle(p.pos, 64.0f, 12.8f, 500, 12);
                        region(d2).interval = 4;
                        aftermath(p);  // (the orb keeps its full speed [QUIRK])
                    }
                }
                if (p.state == 1 || p.state == 2) p.pos = p.pos + p.vec * g_timing.speed;
            }
        }
        // The final pass: regions follow the orbs; exploded orbs finish after 30 frames.
        for (int i = 0; i < 16; ++i) {
            Projectile& p = proj(i);
            if (p.state == 1) {
                moveRegions(p);
            } else if (p.state == 2) {
                if (++p.counter >= 30) p.state = 0;
            }
            if (p.state != 0) p.vms[0].tick(rng);
        }
        return;
    }
    // Last Spell: two rings of orbs exploding one after another from t = 145.
    for (int i = 0; i < 16; ++i) {
        Projectile& p = proj(i);
        if (p.state == 1) {
            const Vec3f old = p.pos;
            p.f10 = addNormalizeAngle(p.f10, (i & 1) ? 0.05235988f : -0.05235988f);
            p.pos.x = std::sin(p.f10) * p.f08 + p.centre.x;
            p.pos.y = std::cos(p.f10) * p.f08 + p.centre.y;
            if (t < 40) p.f08 += (i & 1) ? 1.2f : 2.4f;
            p.vec = p.pos - old;
            if (t >= 200 - 40 - i) {
                deactivate(p);
                cancelCircle(pos_, 64.0f, 4.2666669f, 30, 6);
                const int d = damageCircle(p.pos, 64.0f, 8.5333338f, 25, 15);
                region(d).interval = 5, region(d).cap = 50;
                aftermath(p);
            }
            moveRegions(p);  // (into the deactivated records after an explosion [QUIRK])
        }
        p.vms[0].tick(rng);
    }
    if (t >= 40 && t % 20 == 0) {
        const int j = 16 + bomb_.scratch;
        bomb_.scratch = 1;  // set, not incremented: every later burst reuses 17 [QUIRK]
        Projectile& p = proj(j);
        p.vms[0].start(lib, rng, kSlotPlayer, 20);
        p.state = 1;
        const std::uint32_t c = w.tables ? w.tables->burstColors[static_cast<std::size_t>((t / 20) % 7)] : kWhite;
        if (target_.x > -100.0f) {
            p.pos = {target_.x, target_.y, 0.0f};
        } else {
            const float x = rng.f32InRange(320.0f) + 32.0f;
            const float y = rng.f32InRange(384.0f) + 32.0f;
            p.pos = {x, y, 0.0f};
        }
        if (w.effects) {
            w.effects->spawn(w, 49, p.pos, 1, c);
            w.effects->spawn(w, 55, p.pos, 1, c);
        }
        p.cancelRegion = cancelCircle(p.pos, 64.0f, 4.2666669f, 30, 6);
        p.damageRegion = damageCircle(p.pos, 64.0f, 8.5333338f, 400, 15);
        region(p.damageRegion).interval = 2;
        w.playSound(15);
        if (w.screen) {
            w.screen->shake(16, 8.0f, 0.0f);
            w.screen->flash(8, 1, c);
        }
    }
    for (int i = 16; i < kProjectiles; ++i) {
        Projectile& p = proj(i);
        if (p.state == 0) continue;
        moveRegions(p);
        if (p.vms[0].tick(rng)) p.state = 0;
    }
}

// ---- Yukari (spec_bombs 3) ----------------------------------------------------------------

void Player::yukari(const World& w, std::int32_t t, bool last) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    static constexpr float kAngles[4] = {kZunPi / 4.0f, 3.0f * kZunPi / 8.0f, kZunPi / 2.0f, 5.0f * kZunPi / 8.0f};
    const int type = last ? 37 : 36;
    if (t == 0) {
        bombStartUp(w, last ? 250 : 150, last ? 300 : 200);
        w.playSound(13);
        proj(0).pos = pos_;
        if (last) {
            proj(0).vms[0].start(lib, rng, kSlotPlayer, 21);
            proj(0).vms[1].start(lib, rng, kSlotPlayer, 22);
        }
        cancelCircle(pos_, 100.0f, 1.0f, last ? 100 : 40, 6);
        region(damageCircle(pos_, 100.0f, 1.0f, 70, 40)).interval = 5;
        if (w.effects) w.effects->spawnSpecialWithVector(w, type, proj(0).pos, {kAngles[0], 1.0f, 4.0f}, 4, 1, kWhite);
    } else if (t == 10 || t == 20 || t == 30) {
        const int n = t / 10;
        cancelCircle(pos_, 100.0f, 1.0f, last && t != 10 ? 100 : 40, 6);
        region(damageCircle(pos_, 100.0f, 1.0f, 70, 40)).interval = 5;
        const Vec3f at = last ? proj(0).pos : pos_;
        if (w.effects) {
            Effect& e = w.effects->spawnSpecialWithVector(w, type, at, {kAngles[n], 1.0f, 4.0f}, 4 + n, 1, kWhite);
            e.vm.start(lib, rng, Effects::kSlotEtama, (last ? 93 : 89) + n - 1);
        }
        if (!last) proj(n).pos = pos_;
    }
    proj(0).vms[0].tick(rng);
    proj(0).vms[1].tick(rng);
}

// A ring's four barriers (effect updates 0x4114E0 / 0x4117B0 at e = 40 / 50): bars tangent
// to the circle of radius R, each a long rotated damage rectangle and a shorter cancel one.
void Player::ringBarrier(const World& w, const Vec3f& centre, float angle, float radius, float thickness, bool lastSpell) {
    if (w.screen) {
        if (lastSpell) w.screen->shake(16, 8.0f, 0.0f);
        w.screen->flash(8, 1, lastSpell ? 0x8F6060F0u : 0x8FF08080u);
    }
    float a = angle + kZunPi / 4.0f;
    const float r = radius * 0.70710671f;
    for (int j = 0; j < 4; ++j) {
        if (a >= kZunPi) a -= kZunTwoPi;
        const Vec3f c{centre.x + static_cast<float>(std::cos(static_cast<double>(a)) * r),
                      centre.y + static_cast<float>(std::sin(static_cast<double>(a)) * r), 0.0f};
        Region& d = region(damageRect(c, 8.0f * r, 4.0f * thickness, 60, lastSpell ? 100 : 70));
        d.interval = lastSpell ? 2 : 4;
        d.angle = addNormalizeAngle(a, kZunPi / 2.0f);
        a = d.angle;
        region(cancelRect(c, 4.0f * r, 4.0f * thickness, 6, lastSpell ? 150 : 100)).angle = a;
    }
}

// ---- Marisa (spec_bombs 4) ----------------------------------------------------------------

void Player::marisa(const World& w, std::int32_t t, bool last) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    Projectile& p0 = proj(0);
    if (t == 0) {
        bombStartUp(w, last ? 350 : 300, last ? 380 : 350);
        if (last) w.playSound(13);
        w.playSound(19);
        p0.pos = pos_;
        for (int k = 0; k < 5; ++k) p0.vms[static_cast<std::size_t>(k)].start(lib, rng, kSlotPlayer, (last ? 35 : 30) + k);
        moveMulX_ = moveMulY_ = 0.2f;
        if (w.screen) w.screen->rampedShake(16.0f, 120, 60, 120);
        if (last) bomb_.scratch = 0;
    }
    if (last && t % 10 == 0 && w.effects) {
        Effect& e = w.effects->spawnSpecial(w, 53, pos_, (bomb_.scratch % 4) + 4, 1, kWhite);
        if (bomb_.scratch & 1) e.vm.start(lib, rng, Effects::kSlotEtama, 92);
        stripInterpolations(e.vm, {128.0f, 0.0f, 0.0f}, {32.0f, 32.0f}, {64.0f, 64.0f});
        e.vm.tick(rng);
        ++bomb_.scratch;
        w.playSound(17);
    }
    if (t % 4 != 0) {
        const float px = pos_.x, py = pos_.y;
        const float h = py / 2.0f + py / 2.0f;
        cancelRect({192.0f, py / 2.0f, 0.0f}, 384.0f, h, 6, 0);
        region(damageRect({px, py / 2.0f, 0.0f}, 128.0f, h, 12, 0)).noSpark = true;
        region(damageRect({192.0f, py / 2.0f, 0.0f}, 384.0f, h, last ? 7 : 6, 0)).noSpark = true;
    }
    for (int k = 0; k < 5; ++k) p0.vms[static_cast<std::size_t>(k)].tick(rng);
}

// ---- Alice (spec_bombs 5) -----------------------------------------------------------------

void Player::alice(const World& w, std::int32_t t, bool last) {
    Familiar& doll = familiars_[0];
    if (t == 0) {
        bombStartUp(w, last ? 230 : 210, last ? 280 : 250);
        w.playSound(13);
        proj(0).centre = last ? pos_ : doll.pos;
        if (!last) w.playSound(6);
    }
    const Vec3f s0 = proj(0).centre;
    if (t < 60) {
        const float u = static_cast<float>(t) / 60.0f;
        const float s = u * u;
        doll.pos = s0 + (Vec3f{192.0f, 224.0f, 0.0f} - s0) * s;
        doll.vm.rotation.z += -kZunPi / 10.0f;
        cancelCircle(doll.pos, 32.0f, 0.0f, 0, 6);
        damageCircle(doll.pos, 32.0f, 0.0f, 40, 0);
        return;
    }
    doll.vm.rotation.z = 0.0f;
    doll.pos.x = 192.0f;
    doll.pos.y = 224.0f;
    if (t >= (last ? 128 : 150)) doll.vm.color &= 0x00FFFFFFu;
    static constexpr std::uint32_t kSparks[5] = {0xFFFFFFFFu, 0xFFFFD0D0u, 0xFFFFB0B0u, 0xFFFF8080u, 0xFFFF4040u};
    static constexpr std::uint32_t kRings[4] = {0xFFFFFFFFu, 0xFFFFD0D0u, 0xFFFF8080u, 0xFFFF0000u};
    auto effect = [&](int type, const Vec3f& at, std::uint32_t c) {
        if (w.effects) w.effects->spawn(w, type, at, 1, c);
    };
    const int blast = last ? 120 : 90;
    if (t >= 60 && t <= 76 && t % 4 == 0) {
        effect(40, doll.pos, kSparks[(t - 60) / 4]);  // 2 floats each
    } else if (t == blast) {
        w.playSound(15);
        effect(42, doll.pos, kWhite);
        effect(43, doll.pos, kWhite);
        effect(44, doll.pos, kWhite);
        if (last) {
            effect(45, {64.0f, 96.0f, 0.0f}, 0xFF0000F0u);
            effect(45, {64.0f, 352.0f, 0.0f}, 0xFFF00000u);
            effect(45, {320.0f, 352.0f, 0.0f}, 0xFF00F000u);
            effect(45, {320.0f, 96.0f, 0.0f}, 0xFF00F0F0u);
        }
        cancelCircle(doll.pos, 1.0f, 5.0f, 110, 6);
        region(damageCircle(doll.pos, 1.0f, 5.0f, 70, 110)).interval = 5;
        if (!last && w.screen) {
            w.screen->shake(24, 8.0f, 0.0f);
            w.screen->flash(8, 1, 0x8FFFFFFFu);
        }
    } else if (t > blast && t <= blast + 40 && (t - blast) % 10 == 0) {
        effect(45, doll.pos, kRings[(t - blast) / 10 - 1]);
    } else if (t == (last ? 180 : 150)) {
        if (w.screen) {
            w.screen->flash(8, 1, last ? kWhite : 0x8FFFFFFFu);
            w.screen->shake(24, 8.0f, 0.0f);
        }
        w.playSound(25);
    } else if (t == (last ? 229 : 209)) {
        doll.state = 1;
        doll.timer.assign(0);
    }
}

// ---- Sakuya (spec_bombs 6) ----------------------------------------------------------------

void Player::sakuya(const World& w, std::int32_t t, bool last) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    const int knives = last ? 128 : 96, pair = last ? 64 : 48;
    Projectile& p0 = proj(0);
    if (t == 0) {
        bombStartUp(w, last ? 320 : 250, last ? 350 : 290);
        moveMulX_ = moveMulY_ = 0.5f;
        p0.effect = w.effects ? &w.effects->spawn(w, 20, pos_, 1, kWhite) : nullptr;
        w.playSound(5);
        if (last && w.screen) w.screen->shake(50, 4.0f, 1.0f);
    }
    if (t <= 60 && p0.effect) p0.effect->pos = pos_;  // (even into a reused slot [QUIRK, visual])
    if (t >= 20 && t < (last ? 148 : 116)) {
        for (int i = 0; i < knives; ++i) {
            if (t != 20 + 2 * (i % pair)) continue;
            Projectile& k = proj(i);
            if (k.state != 0) return;  // the whole routine stops (never within one bomb)
            k.state = 1;
            k.vms[0].startFresh(lib, rng, kSlotPlayer, last ? 20 : 22);
            k.f10 = last ? addNormalizeAngle(static_cast<float>(i) * kZunTwoPi / 64.0f - kZunPi, 0.0f)
                         : static_cast<float>(i) * kZunTwoPi / 96.0f - kZunPi;
            k.f0C = rng.f32InRange(1.0f) + 0.5f;
            k.f08 = rng.f32InRange(0.1f) + 0.03f;
            k.f1AC = rng.u16InRange(1) != 0 ? kZunPi / 20.0f : -kZunPi / 20.0f;
            k.pos = pos_ + Vec3f{std::cos(k.f10) * 24.0f, std::sin(k.f10) * 24.0f, 0.0f};
            k.ownTimer.assign(0);
            k.cancelRegion = cancelCircle(k.pos, 32.0f, 0.0f, 500, 6);
            k.damageRegion = damageCircle(k.pos, 32.0f, 0.0f, last ? 30 : 20, 500);
        }
        w.playSound(6);
        if (!last && w.screen) w.screen->shake(120, 4.0f, 1.0f);
    }
    for (int i = 0; i < knives; ++i) {
        Projectile& k = proj(i);
        if (k.state == 0) continue;
        const std::int32_t a = k.ownTimer.current;
        if (a < 30 || a >= 70) {
            if (a == 70) {
                if (last && w.effects) w.effects->spawn(w, 46, k.pos, 1, kWhite);
                if (target_.x > -100.0f) k.f10 = addNormalizeAngle(std::atan2(target_.y - k.pos.y, target_.x - k.pos.x), 0.0f);
                k.f0C = 14.0f;
            }
            k.f0C += k.f08;
            k.vec = {std::cos(k.f10) * k.f0C, std::sin(k.f10) * k.f0C, 0.0f};
        } else {  // it hangs and spins
            k.f10 = addNormalizeAngle(k.f10, k.f1AC);
            k.vec = {};
        }
        if (k.damageRegion >= 0) {
            region(k.cancelRegion).centre = k.pos;
            region(k.damageRegion).centre = k.pos;
            if (a >= 120) {
                region(k.cancelRegion).active = region(k.damageRegion).active = false;
                k.cancelRegion = k.damageRegion = -1;
            } else if (region(k.damageRegion).dealt > 0) {
                if (last && i % 3 == 0 && w.screen) w.screen->flash(2, 1, 0x208080FFu);
                k.vms[0].startFresh(lib, rng, kSlotPlayer, last ? 21 : 23);
                if (w.effects) w.effects->spawn(w, 0, k.pos, 1, 0xFFFF80FFu);
                region(k.cancelRegion).active = region(k.damageRegion).active = false;
                k.cancelRegion = k.damageRegion = -1;
                w.playSound(43);
            }
        }
        k.pos = k.pos + k.vec;  // not x M [QUIRK]
        k.vms[0].tick(rng);
        k.ownTimer.tick();
    }
}

// ---- Remilia (spec_bombs 7) ---------------------------------------------------------------

void Player::remilia(const World& w, std::int32_t t, bool last) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    static constexpr Vec3f kCross[4] = {{-32, 0, 0}, {0, -32, 0}, {0, 32, 0}, {32, 0, 0}};
    if (t == 0) {
        bombStartUp(w, last ? 280 : 240, last ? 320 : 290);
        w.playSound(13);
        for (int k = 0; k < 4; ++k) proj(k).centre = familiars_[static_cast<std::size_t>(k)].anchor;
        w.playSound(6);
        bomb_.scratch = 0;
        moveMulX_ = moveMulY_ = 0.0f;
    }
    if (t < 60) {
        const float u = static_cast<float>(t) / 60.0f;
        const float s = u * u;
        for (int k = 0; k < 4; ++k) {
            const Vec3f a = proj(k).centre;
            familiars_[static_cast<std::size_t>(k)].anchor = (pos_ + kCross[k] - a) * s + a;
        }
        return;
    }
    if (t == 60) {
        moveMulX_ = moveMulY_ = last ? 3.0f : 2.0f;
        for (Familiar& f : familiars_) f.vm.interrupt(2);
    }
    if (last) cancelCircle(pos_, 96.0f, 0.0f, 0, 6);
    for (int k = 0; k < 4; ++k) familiars_[static_cast<std::size_t>(k)].anchor = pos_ + kCross[k];
    if (!last) cancelCircle(pos_, 96.0f, 0.0f, 0, 6);
    if (t % 10 == 0 && w.effects) {
        Effect& e = w.effects->spawnSpecial(w, 53, pos_, (bomb_.scratch % 4) + 4, 1, kWhite);
        const float y = rng.f32InRange(128.0f);
        stripInterpolations(e.vm, {192.0f, y, 0.0f}, {64.0f, 0.0f}, {last ? 128.0f : 64.0f, 0.0f});
        ++bomb_.scratch;
        w.playSound(17);
        e.vm.tick(rng);
    }
    (void)lib;
    if (fireCycle_.current >= 5) {
        cancelRect(pos_, 96.0f, 800.0f, 6, 0);
        cancelRect(pos_, 800.0f, 96.0f, 6, 0);
    }
    if (t == (last ? 279 : 239)) {
        for (Familiar& f : familiars_) {
            f.state = 1;
            f.timer.assign(0);
        }
    }
}

// ---- Youmu (spec_bombs 8) -----------------------------------------------------------------

void Player::youmu(const World& w, std::int32_t t, bool last) {
    Projectile& p0 = proj(0);
    if (t == 0) {
        bombStartUp(w, last ? 250 : 220, last ? 300 : 270);
        w.playSound(13);
        moveMulX_ = moveMulY_ = 0.0f;
        setSpriteScript(w, 0);
        w.playSound(6);
        p0.centre = pos_;  // S; the dash target T is (S.x, 416)
    }
    const Vec3f s0 = p0.centre, dash{s0.x, 416.0f, s0.z};
    if (t < 40) {
        const float u = 1.0f - static_cast<float>(t) / 40.0f;
        const float s = 1.0f - u * u;
        pos_ = s0 + (dash - s0) * s;
    }
    auto column = [&](float x, std::int32_t damage, std::int32_t life, std::int32_t interval) {
        const Vec3f c{x, 224.0f, 0.0f};
        cancelRect(c, 96.0f, 448.0f, 6, 60);
        region(damageRect(c, 96.0f, 448.0f, damage, life)).interval = interval;
        if (w.effects) w.effects->spawn(w, 48, c, 1, kWhite);
    };
    static constexpr std::int32_t kBombDamage[3] = {100, 100, 80};
    static constexpr std::uint32_t kBombFlash[3] = {0xCFFFFFFFu, 0xBFFFFFFFu, 0x8FFFFFFFu};
    static constexpr std::int32_t kLastDamage[6] = {100, 100, 80, 60, 50, 40};
    static constexpr std::uint32_t kLastFlash[6] = {0xCFFFFFFFu, 0xAFFFFFFFu, 0x8FFFFFFFu, 0x6FFFFFFFu, 0x5FFFFFFFu, 0x5FFFFFFFu};
    const int pairs = last ? 6 : 3;
    const int back = last ? 150 : 120;
    if (t == 40) {
        if (w.effects) w.effects->spawn(w, 40, pos_, 1, last ? 0xFFFF8080u : 0xFF8080FFu);
    } else if (t == 70) {
        pos_.y = 32.0f;
        const Vec3f c{pos_.x, 224.0f, 0.0f};
        if (w.screen) w.screen->flash(8, 1, 0xEFFFFFFFu);
        w.playSound(42);
        cancelRect(c, 96.0f, 448.0f, 6, 60);
        damageRect(c, 96.0f, 448.0f, last ? 500 : 300, last ? 0 : 10);
        region(damageRect(c, 96.0f, 448.0f, last ? 100 : 80, 60)).interval = 5;
        if (w.effects) w.effects->spawn(w, 48, c, 1, kWhite);
    } else if (t >= 80 && t <= 70 + 10 * pairs && t % 10 == 0) {
        const int n = (t - 70) / 10;  // 1..
        const float off = 32.0f * static_cast<float>(n);
        const std::int32_t damage = last ? kLastDamage[n - 1] : kBombDamage[n - 1];
        column(pos_.x - off, damage, 40, 2);
        column(pos_.x + off, damage, 40, 3);
        if (w.screen) w.screen->flash(8, 1, last ? kLastFlash[n - 1] : kBombFlash[n - 1]);
        if (n == pairs) p0.vec = pos_;  // U: the top
    } else if (t >= back && t < back + 30) {
        const float u = 1.0f - static_cast<float>(t - back) / 30.0f;
        const float s = 1.0f - u * u;
        pos_ = p0.vec + (s0 - p0.vec) * s;  // stops (S - U) / 900 short [QUIRK]
    } else if (t == back + 30) {
        moveMulX_ = moveMulY_ = 1.0f;
    }
}

// ---- Yuyuko (spec_bombs 9) ----------------------------------------------------------------

void Player::yuyuko(const World& w, std::int32_t t, bool last) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    if (t == 0) {
        bombStartUp(w, 300, 350);
        static constexpr float kSpin[4] = {kZunPi / 240.0f, -kZunPi / 240.0f, kZunPi / 200.0f, -kZunPi / 200.0f};
        for (int ring = 0; ring < 4; ++ring) {
            for (int j = 0; j < 16; ++j) {
                Projectile& p = proj(ring * 16 + j);
                p.state = 1;
                p.vms[0].start(lib, rng, kSlotPlayer, (ring == 0 || ring == 2) ? 18 : 19);
                p.f10 = static_cast<float>(j) * kZunPi / 8.0f - kZunPi;
                p.f08 = kSpin[ring];
                p.centre = p.pos = pos_;
                p.damageRegion = damageCircle(p.pos, 24.0f, 0.0f, 50, 500);
                region(p.damageRegion).cap = 800;
                p.cancelRegion = cancelCircle(p.pos, 24.0f, 0.0f, 500, 6);
                p.vec.x = 0.0f;
                p.vec.y = ring < 2 ? 2.0f : 1.5f;
            }
        }
        moveMulX_ = moveMulY_ = 0.8f;
        if (w.effects) proj(0).effect = &w.effects->spawn(w, 20, pos_, 1, kWhite);
        w.playSound(5);
        if (w.screen) w.screen->shake(last ? 60 : 120, last ? 16.0f : 12.0f, 0.0f);
    }
    if (last && t >= 60 && t < 200 && t % 20 == 0) {
        const float base = signedRand(rng, kZunPi);
        int n = 0;
        for (int l = 0; l < kProjectiles && n < 16; ++l) {
            Projectile& p = proj(l);
            if (p.state != 0) continue;
            p.state = 1;
            p.vms[0].start(lib, rng, kSlotPlayer, 20);
            p.f10 = addNormalizeAngle(static_cast<float>(l) * kZunPi / 8.0f + base, 0.0f);  // by slot index [QUIRK]
            p.centre = p.pos = pos_;
            p.damageRegion = damageCircle(p.pos, 64.0f, 0.0f, 100, 500);
            region(p.damageRegion).cap = 1200;
            p.cancelRegion = cancelCircle(p.pos, 64.0f, 0.0f, 500, 6);
            p.vec.x = 0.0f;
            p.vec.y = 8.0f;  // (the angular speed is left as it was [QUIRK])
            ++n;
        }
        w.playSound(15);
        if (w.screen) {
            w.screen->shake(30, 8.0f, 0.0f);
            w.screen->flash(8, 1, 0xE0F0F0F0u);
        }
    }
    butterflies(w);
}

void Player::butterflies(const World& w) {
    ZunRng& rng = *w.rng;
    for (int i = 0; i < kProjectiles; ++i) {
        Projectile& p = proj(i);
        if (p.state == 0) continue;
        const Vec3f before = p.pos;
        p.vec.x += p.vec.y;
        p.f10 = addNormalizeAngle(p.f10, p.f08);
        p.pos = p.centre + Vec3f{std::cos(p.f10) * p.vec.x, std::sin(p.f10) * p.vec.x, 0.0f};
        p.prev = p.pos - before;
        auto off = [&]() {
            if (p.damageRegion >= 0) region(p.damageRegion).active = false;
            if (p.cancelRegion >= 0) region(p.cancelRegion).active = false;
        };
        if (p.vec.x >= 500.0f) {
            p.state = 0;
            off();
            continue;
        }
        if (p.damageRegion >= 0) region(p.damageRegion).centre = p.pos;
        if (p.cancelRegion >= 0) region(p.cancelRegion).centre = p.pos;
        if (p.vms[0].tick(rng)) {
            p.state = 0;
            off();
        }
    }
}

// ---- the Dissolve spell (spec_bombs 10) ----------------------------------------------------

void Player::dissolve(const World& w) {
    const bool practice = (w.mode & ModeSpellPractice) != 0;
    bombStartUp(w, practice ? 40 : 120, 200);
    if (w.effects) {
        w.effects->spawn(w, 12, pos_, 1, 0xFF4040FFu);
        w.effects->spawnSpecial(w, 50, pos_, 4, 1, kWhite);
    }
    w.playSound(13);
    moveMulX_ = moveMulY_ = 0.0f;
    if (w.enemies) w.enemies->dissolveStart();
}

// ---- drawing (visual) ---------------------------------------------------------------------

void Player::drawBomb(SpriteSink& sink, const AnmDrawState& state) {
    auto draw = [&](AnmVm& vm, const Vec3f& at, float z) {
        vm.position = Vec3f{at.x + vm.offset.x + kPlayfieldX, at.y + vm.offset.y + kPlayfieldY, z};
        drawVm(sink, vm, state);
    };
    switch (bomb_.spell) {
        case Spell::ReimuBomb:
        case Spell::ReimuLast: {
            const int n = bomb_.spell == Spell::ReimuBomb ? 16 : kProjectiles;
            for (int i = 0; i < n; ++i) {
                Projectile& p = proj(i);
                if (p.state != 0) draw(p.vms[0], p.pos, 0.0f);
            }
            break;
        }
        case Spell::YukariLast: {
            Projectile& p = proj(0);
            draw(p.vms[0], p.pos, 0.01f);
            draw(p.vms[1], p.pos, 0.0f);
            break;
        }
        case Spell::MarisaBomb:
        case Spell::MarisaLast: {
            Projectile& p = proj(0);
            for (int k = 0; k < 5; ++k) {
                AnmVm& vm = p.vms[static_cast<std::size_t>(k)];
                const float a = addNormalizeAngle(static_cast<float>(k) * kZunPi / 15.0f - kZunPi / 2.0f - 2.0f * kZunPi / 15.0f, 0.0f);
                const float reach = vm.spriteHeight * vm.scale.x / 2.0f;
                vm.rotation.z = a;
                draw(vm, pos_ + Vec3f{std::cos(a) * reach, std::sin(a) * reach, 0.0f}, 0.0f);
            }
            break;
        }
        case Spell::SakuyaBomb:
        case Spell::SakuyaLast:
            for (int i = 0; i < 96; ++i) {  // (the Last Spell's knives 96-127 are invisible [QUIRK])
                Projectile& p = proj(i);
                if (p.state == 0) continue;
                p.vms[0].rotation.z = p.f10;
                draw(p.vms[0], p.pos, 0.0f);
            }
            break;
        case Spell::YuyukoBomb:
        case Spell::YuyukoLast: {
            const int n = bomb_.spell == Spell::YuyukoBomb ? 96 : kProjectiles;
            for (int i = 0; i < n; ++i) {
                Projectile& p = proj(i);
                if (p.state == 0) continue;
                p.vms[0].rotation.z = std::atan2(p.prev.y, p.prev.x);
                draw(p.vms[0], p.pos, 0.0f);
            }
            break;
        }
        default: break;  // the background tint only
    }
}

}  // namespace touhou::th08
