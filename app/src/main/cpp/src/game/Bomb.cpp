#include "game/Bomb.hpp"

#include <algorithm>
#include <cmath>

#include "game/EffectManager.hpp"
#include "game/EnemyManager.hpp"
#include "game/Gui.hpp"
#include "game/ItemManager.hpp"
#include "game/Player.hpp"
#include "game/ScreenShake.hpp"
#include "game/World.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {
namespace {

enum : int { kReimuA = 0, kReimuB = 1, kMarisaA = 2, kMarisaB = 3 };

// Sound table indices.
constexpr int kSoundPower = 6, kSoundOrbLaunch = 13, kSoundBombStart = 14, kSoundOrbExplode = 15, kSoundSpark = 19;

// Scripts in the player's ANM file (global index = 0x400 + n).
constexpr int kScriptReimuAOrb = 133;   // 133..136, four layers per orb
constexpr int kScriptReimuBBeam = 137;  // 137..140, one per beam
constexpr int kScriptMarisaAStar = 5;   // 5..7 by star index mod 3
constexpr int kScriptMarisaBBeam = 8;   // 8..11

constexpr int kOverlayMax = 176;

constexpr const char* kNames[4] = {
    "SPIRIT SIGN \"DREAM SEAL\"",
    "DREAM SIGN \"EVIL-SEALING CIRCLE\"",
    "MAGIC SIGN \"STARDUST REVERIE\"",
    "LOVE SIGN \"MASTER SPARK\"",
};

bool overlap(const Vec3f& aTL, const Vec3f& aBR, const Vec3f& bTL, const Vec3f& bBR) {
    return !(aTL.x > bBR.x || aBR.x < bTL.x || aTL.y > bBR.y || aBR.y < bTL.y);
}

float length2d(const Vec3f& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

void shake(World& world, int length, float from, float to) {
    if (world.shake) world.shake->add(length, from, to);
}

}  // namespace

void Bomb::reset() {
    regions_ = {};
    cancel_ = {};
    partState_ = {};
    partPos_ = partPrevPos_ = partVel_ = {};
    partSpeed_ = {};
    timer_ = 0;
    duration_ = 999;
    moveMultiplier_ = 1.0f;
    nameTimer_ = nameLeaving_ = -1;
}

void Bomb::clearBoxes() {
    for (Region& r : regions_) r.size.x = 0.0f;
    for (Rect& r : cancel_) r.size.x = 0.0f;
}

int Bomb::liveRegions() const {
    int n = 0;
    for (const Region& r : regions_) n += r.size.x > 0.0f;
    return n;
}

int Bomb::liveCancelRects() const {
    int n = 0;
    for (const Rect& r : cancel_) n += r.size.x != 0.0f;
    return n;
}

void Bomb::setRegion(int i, const Vec3f& center, const Vec3f& size, int damage) {
    Region& r = regions_[static_cast<std::size_t>(i)];
    r.center = center;
    r.size = size;
    r.damage = damage;
}

void Bomb::setRect(int i, const Vec3f& center, const Vec3f& size) {
    cancel_[static_cast<std::size_t>(i)] = {center, size};
}

void Bomb::startVm(World& world, AnmVm& vm, int script) {
    vm = AnmVm{};
    world.anm->setAndExecuteScript(vm, kAnmOffsetPlayer + script);
}

void Bomb::showName(World& world) {
    // With the game's GUI files the portrait and name come from face/text ANM scripts;
    // otherwise the stage draws its own banner from nameVisibility().
    if (world.gui && world.gui->ready()) world.gui->showBomb(world, type_);
    nameTimer_ = 0;
    nameLeaving_ = -1;
    world.playSound(kSoundBombStart);
}

void Bomb::start(World& world, Player& player) {
    GameState& gs = *world.state;
    ++gs.bombsUsed;
    --gs.bombs;
    gs.bombActive = true;
    timer_ = 0;
    duration_ = 999;
    type_ = gs.shotType & 3;
    step(world, player);
    if (world.enemies) world.enemies->onPlayerBomb();
    gs.decreaseSubrank(200);
}

void Bomb::step(World& world, Player& player) {
    GameState& gs = *world.state;
    if (timer_ >= duration_) {
        nameLeaving_ = 0;  // interrupt 1: the name banner leaves
        if (world.gui) world.gui->endBombName();
        gs.bombActive = false;
        if (type_ == kMarisaB) moveMultiplier_ = 1.0f;
        return;
    }
    const int t = timer_;
    switch (type_) {
        case kReimuA: stepReimuA(world, player, t); break;
        case kReimuB: stepReimuB(world, player, t); break;
        case kMarisaA: stepMarisaA(world, player, t); break;
        default: stepMarisaB(world, player, t); break;
    }
    player.state = Player::Invulnerable;
    ++timer_;
}

void Bomb::stepReimuA(World& world, Player& player, int t) {
    if (t == 0) {
        showName(world);
        duration_ = 300;
        player.invulnerabilityTimer_ = 360;
        partState_ = {};
        if (world.items) world.items->magnetAll();
        spawnEffect(world, EffectFlash, player.positionCenter, 1, 0xff4040ffu);
        setRect(8, player.positionCenter, {256.0f, 256.0f, 0.0f});  // live on this step only
    }
    if (t >= 60 && t < 180 && t % 16 == 0) {
        const int k = (t - 60) / 16;  // orb k launches at t = 64 + 16k; k = 0 launches nothing
        if (k != 0) {
            const auto ki = static_cast<std::size_t>(k);
            partState_[ki] = 1;
            partSpeed_[ki] = 4.0f;
            partPos_[ki] = partPrevPos_[ki] = player.positionCenter;
            const float theta = world.rng->f32InRange(kZunTwoPi) - kZunPi;
            partVel_[ki] = {std::cos(theta) * 4.0f, std::sin(theta) * 4.0f, 0.0f};
            regions_[ki].accumulated = 0;
            for (int j = 0; j < 4; ++j) startVm(world, vms_[ki][static_cast<std::size_t>(j)], kScriptReimuAOrb + j);
            world.playSound(kSoundOrbLaunch);
        }
    }
    for (int i = 0; i < kParts; ++i) {
        const auto ii = static_cast<std::size_t>(i);
        partPrevPos_[ii] = partPos_[ii];
        if (partState_[ii] == 0) continue;
        Vec3f& p = partPos_[ii];
        Vec3f& v = partVel_[ii];
        if (partState_[ii] == 1) {
            const Vec3f target = player.homingTarget_.x > -100.0f ? player.homingTarget_ : player.positionCenter;
            Vec3f d = target - p;
            d.z = 0.0f;
            float k = length2d(d) / (partSpeed_[ii] / 8.0f);
            if (k < 1.0f) k = 1.0f;
            Vec3f w = d / k + v;
            w.z = 0.0f;
            const float n = length2d(w);
            float s = std::min(n, 10.0f);
            if (s < 1.0f) s = 1.0f;
            if (n > 0.0f) v = w * (s / n);
            partSpeed_[ii] = s;
            setRegion(i, p, {48.0f, 48.0f, 0.0f}, 8);
            setRect(i, p, {48.0f, 48.0f, 0.0f});
            if (regions_[ii].accumulated >= 100 || t >= duration_ - 30) {
                spawnEffect(world, EffectParticleGreen, p, 8);
                spawnEffect(world, EffectFlash, p, 1, 0xff4040ffu);
                partState_[ii] = 2;
                for (AnmVm& vm : vms_[ii]) vm.pendingInterrupt = 1;
                setRegion(i, p, {256.0f, 256.0f, 0.0f}, 200);
                setRect(i, p, {256.0f, 256.0f, 0.0f});
                world.playSound(kSoundOrbExplode);
                shake(world, 16, 8.0f, 0.0f);
            }
        } else if (++partState_[ii] >= 30) {
            partState_[ii] = 0;
        }
        p += v;
        for (AnmVm& vm : vms_[ii]) world.anm->executeScript(vm);
    }
}

void Bomb::stepReimuB(World& world, Player& player, int t) {
    static constexpr Vec3f kVertical{62.0f, 448.0f, 0.0f}, kHorizontal{384.0f, 62.0f, 0.0f};
    if (t == 0) {
        if (world.items) world.items->magnetAll();
        showName(world);
        duration_ = 140;
        player.invulnerabilityTimer_ = 200;
        for (int i = 0; i < 4; ++i) startVm(world, vms_[static_cast<std::size_t>(i)][0], kScriptReimuBBeam + i);
        world.playSound(kSoundPower);
        // Fixed where the bomb started: the beams do not follow the player.
        const Vec3f& pc = player.positionCenter;
        partPos_[0] = partPos_[2] = {pc.x, kGameRegionHeight / 2.0f, 0.0f};
        partPos_[1] = partPos_[3] = {kGameRegionWidth / 2.0f, pc.y, 0.0f};
        shake(world, 60, 2.0f, 6.0f);
        return;
    }
    if (t == 60) shake(world, 80, 20.0f, 0.0f);
    for (int i = 0; i < 4; ++i) cancel_[static_cast<std::size_t>(i)].size = i % 2 ? kHorizontal : kVertical;
    for (int i = 0; i < 4; ++i) {
        AnmVm& vm = vms_[static_cast<std::size_t>(i)][0];
        world.anm->executeScript(vm);
        if (t & 1) {
            // The beams move only through their scripts' position offsets.
            const Vec3f c = partPos_[static_cast<std::size_t>(i)] + vm.posOffset;
            cancel_[static_cast<std::size_t>(i)].center = c;
            setRegion(i, c, cancel_[static_cast<std::size_t>(i)].size, 8);
        }
    }
}

void Bomb::stepMarisaA(World& world, Player& player, int t) {
    if (t == 0) {
        if (world.items) world.items->magnetAll();
        showName(world);
        duration_ = 250;
        player.invulnerabilityTimer_ = 300;
        for (int i = 0; i < kParts; ++i) {
            const auto ii = static_cast<std::size_t>(i);
            startVm(world, vms_[ii][0], kScriptMarisaAStar + i % 3);
            partPos_[ii] = partPrevPos_[ii] = player.positionCenter;
            const float a = static_cast<float>(i) * kZunTwoPi / 8.0f;
            partVel_[ii] = {std::cos(a) * 2.0f, std::sin(a) * 2.0f, 0.0f};
        }
        world.playSound(kSoundPower);
        shake(world, 120, 4.0f, 1.0f);
        return;
    }
    for (int i = 0; i < kParts; ++i) {
        const auto ii = static_cast<std::size_t>(i);
        partPrevPos_[ii] = partPos_[ii];
        partPos_[ii] += partVel_[ii];
        if (t % 3 != 0) {
            setRect(i, partPos_[ii], {128.0f, 128.0f, 0.0f});
            setRegion(i, partPos_[ii], {128.0f, 128.0f, 0.0f}, 8);
        }
        world.anm->executeScript(vms_[ii][0]);
    }
}

void Bomb::stepMarisaB(World& world, Player& player, int t) {
    if (t == 0) {
        if (world.items) world.items->magnetAll();
        showName(world);
        duration_ = 300;
        player.invulnerabilityTimer_ = 360;
        for (int j = 0; j < 4; ++j) startVm(world, vms_[0][static_cast<std::size_t>(j)], kScriptMarisaBBeam + j);
        world.playSound(kSoundSpark);
        moveMultiplier_ = 0.3f;
        return;
    }
    if (t == 60) shake(world, 60, 1.0f, 7.0f);
    else if (t == 120) shake(world, 200, 24.0f, 0.0f);
    if (t % 4 != 0) {
        const float py = player.positionCenter.y;  // where the player was at the end of last frame
        const Vec3f c{kGameRegionWidth / 2.0f, py / 2.0f, 0.0f};
        const Vec3f size{kGameRegionWidth, py, 0.0f};
        setRect(0, c, size);
        setRegion(0, c, size, 12);
    }
    for (AnmVm& vm : vms_[0]) world.anm->executeScript(vm);
}

int Bomb::overlayAlpha() const {
    const int t = timer_;
    if (t < 60) return std::min(t * kOverlayMax / 60, kOverlayMax);
    if (t >= duration_ - 60) return std::max((duration_ - t) * kOverlayMax / 60, 0);
    return kOverlayMax;
}

void Bomb::draw(World& world, SpriteSink& sink, const SpriteRegion& white, float alpha, const Vec3f& playerPos) {
    const GameState& gs = *world.state;
    if (!gs.bombActive) return;
    // Darkening over the fixed playfield rectangle (shake does not move it).
    if (const int a = overlayAlpha(); a > 0) {
        sink.fillRect(white, kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight,
                      rgba(0, 0, 0, static_cast<std::uint8_t>(a)));
    }
    const float ox = gs.playfieldX, oy = gs.playfieldY;
    switch (type_) {
        case kReimuA:
            for (int i = 0; i < kParts; ++i) {
                const auto ii = static_cast<std::size_t>(i);
                if (partState_[ii] == 0) continue;
                const Vec3f p = lerp(partPrevPos_[ii], partPos_[ii], alpha);
                for (AnmVm& vm : vms_[ii]) {
                    AnmVm flat = vm;  // drawn without rotation
                    flat.rotation = {};
                    flat.pos = {p.x + vm.posOffset.x, p.y + vm.posOffset.y, 0.0f};
                    world.anm->drawWorld(sink, flat, ox, oy);
                }
            }
            break;
        case kReimuB:
            for (int i = 0; i < 4; ++i) {
                AnmVm& vm = vms_[static_cast<std::size_t>(i)][0];
                const Vec3f& base = partPos_[static_cast<std::size_t>(i)];
                vm.pos = {base.x + vm.posOffset.x, base.y + vm.posOffset.y, 0.0f};
                world.anm->drawWorld(sink, vm, ox, oy);
            }
            break;
        case kMarisaA:
            for (int i = 0; i < kParts; ++i) {
                const auto ii = static_cast<std::size_t>(i);
                AnmVm& vm = vms_[ii][0];
                const Vec3f p = lerp(partPrevPos_[ii], partPos_[ii], alpha);
                const Vec3f& v = partVel_[ii];
                const struct {
                    Vec3f pos;
                    float scale;
                } copies[3] = {{p, 3.2f},
                               {{p.x - 6.0f * v.x - 32.0f, p.y - 6.0f * v.y - 32.0f, 0.0f}, 2.2f},
                               {{p.x - 10.0f * v.x, p.y - 10.0f * v.y, 0.0f}, 1.0f}};
                for (const auto& c : copies) {
                    vm.pos = c.pos;
                    vm.scaleX = vm.scaleY = c.scale;
                    world.anm->drawWorld(sink, vm, ox, oy);
                }
                vm.scaleX = vm.scaleY = 1.0f;
            }
            break;
        default:
            for (int i = 0; i < 4; ++i) {
                AnmVm& vm = vms_[0][static_cast<std::size_t>(i)];
                const float a = -3.0f * kZunPi / 5.0f + static_cast<float>(i) * kZunPi / 15.0f;
                const AnmLoadedSprite* s = world.anm->sprite(vm);
                const float reach = s ? s->heightPx * vm.scaleY / 2.0f : 0.0f;
                vm.pos = {playerPos.x + std::cos(a) * reach, playerPos.y + std::sin(a) * reach, 0.0f};
                float r = kZunHalfPi - a + kZunPi;
                while (r > kZunPi) r -= kZunTwoPi;
                while (r < -kZunPi) r += kZunTwoPi;
                vm.rotation.z = r;
                world.anm->drawWorld(sink, vm, ox, oy);
            }
            break;
    }
}

void Bomb::updateGui() {
    if (nameTimer_ < 0) return;
    ++nameTimer_;
    if (nameLeaving_ >= 0 && ++nameLeaving_ >= 32) nameTimer_ = nameLeaving_ = -1;
}

const char* Bomb::name() const { return kNames[type_ & 3]; }

float Bomb::nameVisibility() const {
    if (nameTimer_ < 0) return 0.0f;
    float v = std::min(static_cast<float>(nameTimer_) / 16.0f, 1.0f);
    if (nameLeaving_ >= 0) v *= 1.0f - static_cast<float>(nameLeaving_) / 32.0f;
    return v;
}

int Bomb::damageEnemy(World& world, const Vec3f& enemyPos, const Vec3f& tl, const Vec3f& br, bool* hit,
                      std::uint8_t& particleCounter) {
    int damage = 0;
    for (Region& r : regions_) {
        if (r.size.x <= 0.0f) continue;
        const Vec3f rtl{r.center.x - r.size.x / 2.0f, r.center.y - r.size.y / 2.0f, 0.0f};
        const Vec3f rbr{r.center.x + r.size.x / 2.0f, r.center.y + r.size.y / 2.0f, 0.0f};
        if (!overlap(rtl, rbr, tl, br)) continue;
        damage += r.damage;
        r.accumulated += r.damage;
        if (++particleCounter % 4 == 0) spawnEffect(world, EffectGlowBig, enemyPos, 1);
        if (world.state->bombActive && hit) *hit = true;
    }
    return damage;
}

bool Bomb::cancels(const Vec3f& tl, const Vec3f& br) const {
    for (const Rect& r : cancel_) {
        if (r.size.x == 0.0f) continue;
        const Vec3f rtl{r.center.x - r.size.x / 2.0f, r.center.y - r.size.y / 2.0f, 0.0f};
        const Vec3f rbr{r.center.x + r.size.x / 2.0f, r.center.y + r.size.y / 2.0f, 0.0f};
        if (overlap(rtl, rbr, tl, br)) return true;
    }
    return false;
}

}  // namespace th06
