// The twelve bombs of Perfect Cherry Blossom (spec: player 8). Part of Player.

#include <algorithm>
#include <cmath>

#include "th07/Effects.hpp"
#include "th07/English.hpp"
#include "th07/ExeTables.hpp"
#include "th07/Player.hpp"
#include "th07/World.hpp"

namespace touhou::th07 {

namespace {

bool ticked(const Timer& t) { return g_timing.speed > 0.99f || t.current != t.previous; }

struct BombInfo {
    int duration, invulnerability, minCost;
    float scale, move;
};
// [shot index][focused]
constexpr BombInfo kBombs[6][2] = {
    {{140, 200, 4000, 0.20f, 1.0f}, {300, 360, 5000, 0.22f, 0.6f}},
    {{140, 200, 3000, 0.17f, 1.0f}, {190, 250, 3000, 0.17f, 0.4f}},
    {{200, 250, 8000, 0.30f, 1.0f}, {260, 310, 9000, 0.33f, 0.4f}},
    {{300, 300, 8000, 0.35f, 0.4f}, {340, 390, 10000, 0.41f, 0.2f}},
    {{160, 210, 6000, 0.28f, 1.0f}, {250, 290, 6500, 0.29f, 0.3f}},
    {{160, 260, 5500, 0.26f, 2.0f}, {300, 420, 6000, 0.29f, 1.5f}},
};

Vec3f polar(float angle, float length) { return {std::cos(angle) * length, std::sin(angle) * length, 0.0f}; }

void shake(const World& w, int duration, float from, float to) {
    if (w.shakes) w.shakes->shake(duration, from, to);
}

}  // namespace

void Player::startBomb(const World& w) {
    GameState& g = *w.state;
    g.eventFlags |= 0x01;
    w.addBombsUsed(1.0f);
    w.addBombs(-1.0f);
    bombFocus_ = focused_;
    bombActive_ = true;
    bombing_ = true;
    g.bombActive = true;
    bombTimer_.reset();
    bombTimer_.assign(0);
    bombDuration_ = 999;
    bombFrame(w);
    g.captureValue = 0;
    g.capturing = false;
    g.decreaseSubrank(200);
    g.bombUsedInCard = g.spellCardActive;
    respawn_ = std::min(respawn_ + 6, header().deathbombWindow);
}

void Player::bombStartUp(const World& w, int invulnerability, int minCost, float scale) {
    GameState& g = *w.state;
    if (w.items) w.items->collectAll();
    // The portrait and name banner (it plays the banner sound).
    static constexpr int kFace[12] = {1, 1, 1, 1, 3, 2, 1, 2, 1, 1, 3, 3};
    const int which = shotIndex_ * 2 + (bombFocus_ ? 1 : 0);
    std::string name = w.tables ? w.tables->bombNames[static_cast<std::size_t>(which)] : std::string();
    if (w.english) {  // the English text mode: the patch's name for the program's string
        if (const auto t = w.english->exeString(ExeTables::kBombNameAddresses[static_cast<std::size_t>(which)])) name = English::gameText(*t);
    }
    if (w.gui) w.gui->bombBanner(0x4A0 + kFace[which], name);
    else w.playSound(14);
    stateTimer_.assign(invulnerability);
    if (w.effects) bombRing_ = &w.effects->spawnSpecial(w, 25, pos_, 0, 0xFFFF8080u);
    std::int32_t d = static_cast<std::int32_t>(static_cast<float>(g.cherry) * scale);
    switch (g.difficulty) {
        case Hard: d /= 2; break;
        case Lunatic: d /= 4; break;
        case Extra:
        case Phantasm: d /= 3; break;
        default: break;
    }
    d /= bombDuration_;
    d -= d % 10;
    std::int32_t m = minCost / bombDuration_;
    m -= m % 10;
    drain_ = std::max(d, m);
}

void Player::bombFrame(const World& w) {
    const int t = bombTimer_.current;
    if (t >= bombDuration_) {
        endBomb(w);
        return;
    }
    const BombInfo& info = kBombs[shotIndex_][bombFocus_ ? 1 : 0];
    const bool startUp = t == 0 && ticked(bombTimer_);
    if (startUp) {
        bombDuration_ = info.duration;
        bombStartUp(w, info.invulnerability, info.minCost, info.scale);
        if (info.move != 1.0f) moveMulX_ = moveMulY_ = info.move;
    }
    switch (shotIndex_ * 2 + (bombFocus_ ? 1 : 0)) {
        case 0: bombReimuA(w, startUp); break;
        case 1: bombReimuAFocus(w, startUp); break;
        case 2: bombReimuB(w, startUp); break;
        case 3: bombReimuBFocus(w, startUp); break;
        case 4: bombMarisaA(w, startUp); break;
        case 5: bombMarisaAFocus(w, startUp); break;
        case 6: bombMarisaB(w, startUp); break;
        case 7: bombMarisaBFocus(w, startUp); break;
        case 8: bombSakuyaA(w, startUp); break;
        case 9: bombSakuyaAFocus(w, startUp); break;
        case 10: bombSakuyaB(w, startUp); break;
        default: bombSakuyaBFocus(w, startUp); break;
    }
    state_ = State::Invulnerable;
    bombTimer_.tick();
}

void Player::endBomb(const World& w) {
    if (w.gui) w.gui->bombBannerEnd();
    bombActive_ = false;
    w.state->bombActive = false;
    moveMulX_ = moveMulY_ = 1.0f;
    if (shotIndex_ == 5) {
        cancelCircle(pos_, 800.0f, 0.0f, 0);
        if (bombFocus_) {
            CancelRegion& c = cancels_[0];
            c.centre = {192.0f, 224.0f, 0.0f};
            c.w = 448.0f;
            c.h = 512.0f;
            c.lifetime = 0;
            c.itemType = ItemField::PointBullet;
        }
    }
    for (Projectile& p : proj_) p.active = false;
    for (AnmVm& vm : bombVms_) vm = AnmVm{};
    projDrawCount_ = 0;
}

// ---- Reimu ---------------------------------------------------------------------------------

void Player::reimuOrbExplode(const World& w, int i, float radius, float growth, std::int32_t lifetime) {
    Projectile& p = proj_[static_cast<std::size_t>(i)];
    if (w.effects) {
        w.effects->spawn(w, 6, p.pos, 8, 0xFFFFFFFFu);
        w.effects->spawn(w, 12, p.pos, 1, 0xFF4040FFu);
    }
    p.state = 2;
    for (AnmVm& vm : p.vms) vm.interrupt(1);
    setRegion(i, p.pos, 256.0f, 256.0f, 400);
    cancelCircle(p.pos, radius, growth, lifetime);
    w.playSound(15);
    shake(w, 16, 8.0f, 0.0f);
}

void Player::launchReimuOrb(const World& w, int i, float speed, float angle) {
    Projectile& p = proj_[static_cast<std::size_t>(i)];
    p.active = true;
    p.state = 1;
    p.counter = 0;
    p.speed = speed;
    p.pos = pos_;
    p.angle = angle;
    regions_[static_cast<std::size_t>(i)].dealt = 0;
    for (int k = 0; k < 4; ++k) startScript(w, p.vms[static_cast<std::size_t>(k)], 133 + k);
    w.playSound(13);
}

void Player::bombReimuA(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (Projectile& p : proj_) p.active = false;
        if (w.effects) w.effects->spawn(w, 12, pos_, 1, 0xFF4040FFu);
        cancelCircle(pos_, 32.0f, 8.0f, 16);
        bombStart_ = pos_;
        projDrawCount_ = 8;
    }
    const bool tick = ticked(bombTimer_);
    if (tick && t >= 8 && t < 80 && t % 6 == 0) {
        const int i = (t - 8) / 6;
        const float fi = static_cast<float>(i);
        const float a = bombStart_.x < 192.0f ? addNormalizeAngle(fi * kZunPi / 4.0f, -kZunPi / 2.0f)
                                               : addNormalizeAngle(-fi * kZunPi / 4.0f, -kZunPi / 2.0f);
        launchReimuOrb(w, i, 15.0f, a);
        proj_[static_cast<std::size_t>(i)].velocity = {};
    }
    for (int i = 0; i < 8; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        if (!p.active) continue;
        if (p.state == 1) {
            p.speed -= 0.4f * g_timing.speed;
            p.velocity = polar(p.angle, p.speed);
            if (p.speed < -10.0f) {
                reimuOrbExplode(w, i, 64.0f, 4.266667f, 30);
                p.velocity = {};
            }
            if (tick) {
                setRegion(i, p.pos, 48.0f, 48.0f, 8);
                cancelCircle(p.pos, 128.0f, 0.0f, 0);
            }
        } else if (p.state == 2 && tick) {
            setRegion(i, p.pos, 256.0f, 256.0f, 2);
            if (++p.counter >= 30) p.active = false;
        }
        p.pos.x += p.velocity.x * g_timing.speed;
        p.pos.y += p.velocity.y * g_timing.speed;
        for (AnmVm& vm : p.vms) vm.tick(*w.anm, *w.rng);
    }
}

void Player::bombReimuAFocus(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (int i = 0; i < 8; ++i) proj_[static_cast<std::size_t>(i)].active = false;
        if (w.effects) w.effects->spawn(w, 12, pos_, 1, 0xFF4040FFu);
        cancelCircle(pos_, 32.0f, 8.0f, 16);
        projDrawCount_ = 8;
    }
    if (t >= 60 && t < 180 && t % 16 == 0) {
        const int i = (t - 60) / 16;
        if (i != 0) {
            const float a = w.rng->f32InRange(kZunTwoPi) - kZunPi;
            launchReimuOrb(w, i, 8.0f, a);
            proj_[static_cast<std::size_t>(i)].velocity = polar(a, 8.0f);
        }
    }
    const bool tick = ticked(bombTimer_);
    for (int i = 0; i < 8; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        if (!p.active) continue;
        if (p.state == 1) {
            if (tick) {
                const Vec3f target = target_.x > -100.0f ? target_ : pos_;
                const float dx = target.x - p.pos.x, dy = target.y - p.pos.y;
                float l = std::sqrt(dx * dx + dy * dy) / (p.speed / 8.0f);
                if (l < 1.0f) l = 1.0f;
                const float vx = dx / l + p.velocity.x, vy = dy / l + p.velocity.y;
                const float m = std::sqrt(vx * vx + vy * vy);
                p.speed = std::min(m, 10.0f);
                if (p.speed < 1.0f) p.speed = 1.0f;
                p.velocity = {vx * p.speed / m, vy * p.speed / m, 0.0f};
                setRegion(i, p.pos, 48.0f, 48.0f, 8);
                cancelCircle(p.pos, 128.0f, 0.0f, 0);
                if (regions_[static_cast<std::size_t>(i)].dealt >= 100 || t >= bombDuration_ - 30) {
                    reimuOrbExplode(w, i, 32.0f, 6.6666665f, 15);
                }
            }
        } else if (p.state == 2 && tick) {
            if (++p.counter >= 30) p.active = false;
        }
        p.pos.x += p.velocity.x;
        p.pos.y += p.velocity.y;
        for (AnmVm& vm : p.vms) vm.tick(*w.anm, *w.rng);
    }
}

void Player::bombReimuB(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (int i = 0; i < 4; ++i) startScript(w, bombVms_[static_cast<std::size_t>(i)], 137 + i);
        w.playSound(6);
        bombAnchor_[0] = {pos_.x, 224.0f, 0.42f};
        bombAnchor_[1] = {192.0f, pos_.y, 0.415f};
        bombAnchor_[2] = {pos_.x, 224.0f, 0.41f};
        bombAnchor_[3] = {192.0f, pos_.y, 0.405f};
        shake(w, 60, 2.0f, 6.0f);
        return;
    }
    if (t == 60) shake(w, 80, 20.0f, 0.0f);
    static constexpr float kSize[4][2] = {{62, 448}, {384, 62}, {62, 448}, {384, 62}};
    int rect[4];
    for (int i = 0; i < 4; ++i) rect[i] = cancelRect(pos_, kSize[i][0], kSize[i][1]);
    const bool tick = ticked(bombTimer_);
    for (int i = 0; i < 4; ++i) {
        AnmVm& vm = bombVms_[static_cast<std::size_t>(i)];
        vm.tick(*w.anm, *w.rng);
        if (tick && (t & 1)) {
            const Vec3f at = bombAnchor_[static_cast<std::size_t>(i)] + vm.offset;
            cancels_[static_cast<std::size_t>(rect[i])].centre = at;
            setRegion(i, at, kSize[i][0], kSize[i][1], 16);
        }
    }
}

void Player::bombReimuBFocus(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (int i = 0; i < 3; ++i) startScript(w, bombVms_[static_cast<std::size_t>(i)], 141 + i);
        w.playSound(6);
        shake(w, 60, 2.0f, 6.0f);
        bombStart_ = pos_;
        for (int i = 0; i < 3; ++i) bombAnchor_[static_cast<std::size_t>(i)] = pos_;
        cancelCircle(pos_, 192.0f, 0.384f, 210);
        return;
    }
    if (t == 60) shake(w, 80, 20.0f, 0.0f);
    for (int i = 0; i < 3; ++i) bombVms_[static_cast<std::size_t>(i)].tick(*w.anm, *w.rng);
    setRegion(0, bombStart_ + bombVms_[0].offset, 256.0f, 256.0f, 18);
}

// ---- Marisa --------------------------------------------------------------------------------

void Player::bombMarisaA(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (int i = 0; i < 8; ++i) {
            Projectile& p = proj_[static_cast<std::size_t>(i)];
            p.active = true;
            startScript(w, p.vms[0], 5 + i % 3);
            p.pos = pos_;
            p.velocity = polar(static_cast<float>(i) * kZunPi / 4.0f, 2.0f);
        }
        projDrawCount_ = 8;
        w.playSound(6);
        shake(w, 120, 4.0f, 1.0f);
        return;
    }
    const bool tick = ticked(bombTimer_);
    for (int i = 0; i < 8; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        p.pos.x += p.velocity.x * g_timing.speed;
        p.pos.y += p.velocity.y * g_timing.speed;
        if (tick && t % 3 != 0) {
            cancelCircle(p.pos, 96.0f, 0.0f, 0);
            setRegion(i, p.pos, 128.0f, 128.0f, 8);
        }
        p.vms[0].tick(*w.anm, *w.rng);
    }
}

void Player::bombMarisaAFocus(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (int i = 0; i < 24; ++i) proj_[static_cast<std::size_t>(i)].active = false;
        projDrawCount_ = 24;
    }
    if (ticked(bombTimer_) && t % 6 == 0 && t / 6 < 24) {
        const int i = t / 6;
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        startScript(w, p.vms[0], 5 + i % 3);
        p.pos = pos_;
        for (std::size_t k = 0; k < 8; ++k) p.trail[k] = pos_;
        p.active = true;
        const float a1 = w.rng->f32InRange(kZunPi / 8.0f) - kZunPi / 16.0f - kZunPi / 2.0f;
        p.velocity = polar(a1, -5.0f);
        const float a2 = w.rng->f32InRange(kZunPi / 8.0f) - kZunPi / 16.0f - kZunPi / 2.0f;
        p.accel = polar(a2, 0.24f);
        w.playSound(7);
        shake(w, 120, 4.0f, 1.0f);
        regions_[static_cast<std::size_t>(i)].dealt = 0;
    }
    for (int i = 0; i < 24; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        if (!p.active) continue;
        for (std::size_t k = 7; k > 0; --k) p.trail[k] = p.trail[k - 1];
        p.trail[0] = p.pos;
        p.pos.x += p.velocity.x * g_timing.speed;
        p.pos.y += p.velocity.y * g_timing.speed;
        p.velocity.x += p.accel.x * g_timing.speed;
        p.velocity.y += p.accel.y * g_timing.speed;
        if (p.pos.y < -256.0f) p.active = false;
        cancelCircle(p.pos, 96.0f, 0.0f, 0);
        if (regions_[static_cast<std::size_t>(i)].dealt < 80) setRegion(i, p.pos, 128.0f, 128.0f, 12);
        p.vms[0].tick(*w.anm, *w.rng);
    }
}

void Player::bombMarisaB(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        bombStart_ = pos_;
        for (int i = 0; i < 3; ++i) {
            startScript(w, bombVms_[static_cast<std::size_t>(i)], 12 + i);
            bombAnchor_[static_cast<std::size_t>(i)] = pos_;
            beamAngle_[static_cast<std::size_t>(i)] = static_cast<float>(i) * kZunTwoPi / 3.0f - kZunPi / 2.0f;
        }
        w.playSound(19);
        return;
    }
    int region = 0;
    const float step = static_cast<float>(t) * kZunPi / 30.0f / static_cast<float>(bombDuration_);
    for (int i = 0; i < 3; ++i) {
        float& a = beamAngle_[static_cast<std::size_t>(i)];
        a = addNormalizeAngle(a, bombStart_.x < 192.0f ? step : -step);
        AnmVm& vm = bombVms_[static_cast<std::size_t>(i)];
        float dist = 32.0f;
        for (int k = 0; k < 6; ++k) {
            const Vec3f at = pos_ + polar(a, dist);
            setRegion(region++, at, 128.0f, 128.0f, 10);
            cancelCircle(at, 64.0f, 0.0f, 0);
            if (vm.sprite) dist += vm.sprite->pixelHeight * vm.scale.y / 5.0f;
        }
        vm.rotation.z = a + kZunPi / 2.0f;
        bombAnchor_[static_cast<std::size_t>(i)] = pos_;
        vm.tick(*w.anm, *w.rng);
    }
    if (t == 20) shake(w, 60, 1.0f, 7.0f);
    if (t == 80) shake(w, 100, 24.0f, 0.0f);
}

void Player::bombMarisaBFocus(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (int i = 0; i < 4; ++i) startScript(w, bombVms_[static_cast<std::size_t>(i)], 8 + i);
        w.playSound(19);
        return;
    }
    if (t == 60) shake(w, 60, 1.0f, 7.0f);
    if (t == 120) shake(w, 200, 24.0f, 0.0f);
    if (ticked(bombTimer_) && t % 4 != 0) {
        const Vec3f c{192.0f, pos_.y / 2.0f, 0.0f};
        setRegion(0, c, 384.0f, pos_.y, 23);
        cancelRect(c, 384.0f, pos_.y);
    }
    for (int i = 0; i < 4; ++i) {
        bombAnchor_[static_cast<std::size_t>(i)] = pos_;
        bombVms_[static_cast<std::size_t>(i)].tick(*w.anm, *w.rng);
    }
}

// ---- Sakuya --------------------------------------------------------------------------------

void Player::bombSakuyaA(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        bombStart_ = pos_;
        for (Projectile& p : proj_) p.active = false;
        if (w.effects) w.effects->spawn(w, 21, pos_, 1, 0xFFFFFFFFu);
        w.playSound(5);
        projDrawCount_ = kProjectiles;
    }
    if (t < 60) return;
    int budget = 5;
    for (int i = 0; i < kProjectiles; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        if (!p.active) {
            if (t <= 120 && budget > 0) {
                p.active = true;
                startScript(w, p.vms[0], 5 + i % 2);
                p.angle = w.rng->f32InRange(kZunTwoPi) - kZunPi;
                p.speed = w.rng->f32InRange(6.0f) + 5.5f;
                p.accelScalar = w.rng->f32InRange(0.1f) + 0.1f;
                p.turn = w.rng->f32InRange(kZunPi / 50.0f) - kZunPi / 100.0f;
                p.pos = bombStart_ + polar(p.angle, 24.0f);
                regions_[static_cast<std::size_t>(i)].dealt = 0;
                p.dealt = 0;
                --budget;
            }
            continue;
        }
        p.angle = addNormalizeAngle(p.angle, p.turn);
        p.speed += p.accelScalar;
        p.velocity = polar(p.angle, p.speed);
        p.vms[0].rotation.z = p.angle + kZunPi / 2.0f;
        const std::int32_t dealt = p.dealt == 999 ? 999 : regions_[static_cast<std::size_t>(i)].dealt;
        if (dealt < 30) {
            p.pos.x += p.velocity.x * g_timing.speed;
            p.pos.y += p.velocity.y * g_timing.speed;
            cancelCircle(p.pos, 32.0f, 0.0f, 0);
            setRegion(i, p.pos, 24.0f, 24.0f, 10);
        } else if (dealt < 999) {
            startScript(w, p.vms[0], 96);
            p.dealt = 999;
        }
        if (p.pos.x + 32.0f < 0.0f || p.pos.x - 32.0f > kPlayfieldW || p.pos.y + 32.0f < 0.0f || p.pos.y - 32.0f > kPlayfieldH) {
            p.active = false;
        }
        p.vms[0].tick(*w.anm, *w.rng);
    }
}

void Player::bombSakuyaAFocus(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    if (startUp) {
        for (Projectile& p : proj_) p.active = false;
        if (w.effects) knifeEffect_ = &w.effects->spawn(w, 21, pos_, 1, 0xFFFFFFFFu);
        w.playSound(5);
        projDrawCount_ = kProjectiles;
    }
    if (t <= 60 && knifeEffect_ && knifeEffect_->inUse) knifeEffect_->pos = pos_;
    if (t >= 20 && t < 116) {
        for (int i = 0; i < kProjectiles; ++i) {
            if (!(bombTimer_.current == 20 + 2 * (i % 48) && ticked(bombTimer_))) continue;
            Projectile& p = proj_[static_cast<std::size_t>(i)];
            p.active = true;
            startScript(w, p.vms[0], 7 + i % 2);
            p.angle = static_cast<float>(i) * kZunTwoPi / 96.0f - kZunPi;
            p.speed = w.rng->f32InRange(1.0f) + 0.5f;
            p.accelScalar = w.rng->f32InRange(0.1f) + 0.03f;
            static constexpr float kTurns[1] = {-kZunPi / 20.0f};
            p.turn = kTurns[w.rng->u16InRange(1)];
            p.pos = pos_ + polar(p.angle, 24.0f);
            p.timer.reset();
            p.timer.assign(0);
            p.dealt = 0;
            regions_[static_cast<std::size_t>(i)].dealt = 0;
        }
        w.playSound(6);
        shake(w, 120, 4.0f, 1.0f);
    }
    for (int i = 0; i < kProjectiles; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        if (!p.active) continue;
        const int a = p.timer.current;
        if (a < 30 || a >= 70) {
            if (a == 70 && ticked(p.timer)) {
                if (target_.x > -100.0f) p.angle = addNormalizeAngle(std::atan2(target_.y - p.pos.y, target_.x - p.pos.x), 0.0f);
                p.speed = 14.0f;
            }
            p.speed += p.accelScalar;
            p.velocity = polar(p.angle, p.speed);
        } else {
            p.angle = addNormalizeAngle(p.angle, p.turn);
            p.velocity = {};
        }
        p.vms[0].rotation.z = p.angle + kZunPi / 2.0f;
        const std::int32_t dealt = p.dealt == 999 ? 999 : regions_[static_cast<std::size_t>(i)].dealt;
        if (dealt == 0) {
            p.pos.x += p.velocity.x * g_timing.speed;
            p.pos.y += p.velocity.y * g_timing.speed;
            cancelCircle(p.pos, 32.0f, 0.0f, 0);
            setRegion(i, p.pos, 24.0f, 24.0f, 22);
        } else if (dealt < 999) {
            startScript(w, p.vms[0], 96);
            p.dealt = 999;
            if (w.effects) w.effects->spawn(w, 0, p.pos, 1, 0xFFFF80FFu);
        }
        p.vms[0].tick(*w.anm, *w.rng);
        p.timer.tick();
    }
}

void Player::bombSakuyaB(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    const bool tick = ticked(bombTimer_);
    if (startUp) {
        for (int i = 0; i < 4; ++i) proj_[static_cast<std::size_t>(i)].active = false;
        w.playSound(19);
        if (w.bullets) w.bullets->freeze();
        return;
    }
    if (tick && (t == 60 || t == 120) && w.bullets) w.bullets->freeze();
    if (t == 30) {
        static constexpr float kCorners[4][2] = {{64, 96}, {320, 96}, {64, 352}, {320, 352}};
        for (int i = 0; i < 4; ++i) {
            startScript(w, bombVms_[static_cast<std::size_t>(i)], 9 + i);
            bombAnchor_[static_cast<std::size_t>(i)] = {kCorners[i][0], kCorners[i][1], 0.49f};
        }
    }
    if (t >= 30 && tick && t % 4 == 0) setRegion(0, {192.0f, 224.0f, 0.0f}, 352.0f, 416.0f, 3);
    if (tick && t == 40) shake(w, 60, 1.0f, 7.0f);
    if (tick && t == 100) shake(w, 70, 24.0f, 0.0f);
    for (AnmVm& vm : bombVms_) {
        if (vm.sprite) vm.tick(*w.anm, *w.rng);
    }
}

void Player::bombSakuyaBFocus(const World& w, bool startUp) {
    const int t = bombTimer_.current;
    const bool tick = ticked(bombTimer_);
    if (startUp) {
        bombing_ = false;
        for (int i = 0; i < 2; ++i) {
            Projectile& p = proj_[static_cast<std::size_t>(i)];
            p.active = true;
            startScript(w, p.vms[0], 13 + i);
            p.pos = pos_;
            for (Vec3f& tr : p.trail) tr = pos_;
            p.velocity = {};
            p.accel = {0.0f, -0.008f, 0.0f};
        }
        projDrawCount_ = 2;
        w.playSound(19);
    }
    cancelCircle(proj_[0].pos, 96.0f, 0.0f, 0);
    setRegion(0, proj_[0].pos, 160.0f, 160.0f, 1);
    if (tick && (t == 40 || t == 100) && w.bullets) w.bullets->freeze();
    if (tick && t == 40) shake(w, 60, 1.0f, 7.0f);
    if (tick && t == 100) shake(w, 70, 24.0f, 0.0f);
    for (int i = 0; i < 2; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        p.accel = {(pos_.x - p.pos.x) / 1700.0f * g_timing.speed, (pos_.y - p.pos.y) / 1700.0f * g_timing.speed, 0.0f};
        p.velocity.x += p.accel.x * g_timing.speed;
        p.velocity.y += p.accel.y * g_timing.speed;
        for (std::size_t k = p.trail.size() - 1; k > 0; --k) p.trail[k] = p.trail[k - 1];
        p.trail[0] = p.pos;
        p.pos.x += p.velocity.x * g_timing.speed;
        p.pos.y += p.velocity.y * g_timing.speed;
        p.vms[0].tick(*w.anm, *w.rng);
    }
}

}  // namespace touhou::th07
