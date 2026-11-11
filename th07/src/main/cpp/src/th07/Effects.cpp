#include "th07/Effects.hpp"

#include <algorithm>
#include <cmath>

#include "th07/Background.hpp"
#include "th07/Player.hpp"
#include "th07/World.hpp"

namespace touhou::th07 {

namespace {

using Init = ExeTables::EffectInit;
using Update = ExeTables::EffectUpdate;

float rnd(const World& w, float range) { return w.rng->f32InRange(range); }

// The weather petal chance: logical cherry * 100 / the *stored* cherry maximum (it includes
// the random base, a quirk of the original); the u32 draw happens either way.
void petalChance(const World& w, Effect& e) {
    const GameState& g = *w.state;
    const std::int32_t storedMax = g.cherryMax + g.cherryBase;
    const auto chance = static_cast<std::uint32_t>(storedMax != 0 ? (g.cherry * 100) / storedMax : 0);
    if (chance >= w.rng->u32InRange(100)) {
        e.vm.selectSprite(*w.anm, 48);
        e.vm.color |= 0x00FFFFFFu;
    }
}

void weatherRotations(const World& w, Effect& e, float xRange) {
    e.vm.rotation.z = rnd(w, kZunTwoPi) - kZunPi;
    e.vm.rotation.x = rnd(w, xRange) - xRange / 2.0f;
    e.vm.set(AnmVm::RotationDirty, true);
}

}  // namespace

void Effects::reset() {
    for (Effect& e : slots_) e = Effect{};
    cursor_ = 0;
    active_ = 0;
    frames_ = 0;
    colorMultiplier_ = {1.0f, 1.0f, 1.0f, 1.0f};  // [GAP] the stages set it before it matters
}

void Effects::init(const World& w, Effect& e, int id, const Vec3f& pos, std::uint32_t color, const Vec3f& custom, bool zwriteOff) {
    const ExeTables::Effect& info = w.tables->effects[static_cast<std::size_t>(id)];
    e.drawType = 0;
    e.inUse = true;
    e.id = static_cast<std::int8_t>(id);
    e.pos = pos;
    e.vm.start(*w.anm, *w.rng, info.script);
    if (zwriteOff) e.vm.set(AnmVm::ZWriteDisable, true);
    e.vm.color = color;
    e.update = info.update;
    e.timer.reset();
    e.fading = false;
    e.fadeCounter = 0;
    e.custom = custom;
    const float m = g_timing.speed;
    const Background* bg = w.background;
    const Vec3f camPos = bg ? bg->cameraPosition() : Vec3f{};
    const Vec3f lookAt = bg ? bg->cameraFacing() : Vec3f{};
    switch (info.init) {
        case Init::None: break;
        case Init::BigBurst:
            e.velocity.x = ((rnd(w, 256.0f) - 128.0f) * 4.0f) / 33.0f;
            e.velocity.y = ((rnd(w, 256.0f) - 128.0f) * 4.0f) / 33.0f;
            e.velocity.z = 0.0f;
            e.accel = e.velocity * -(1.0f / 20.0f);
            e.velocity = e.velocity * m;
            e.accel = e.accel * m;
            break;
        case Init::SmallBurst:
            e.velocity.x = (rnd(w, 256.0f) - 128.0f) / 12.0f;
            e.velocity.y = (rnd(w, 256.0f) - 128.0f) / 12.0f;
            e.velocity.z = 0.0f;
            e.accel = e.velocity * -(1.0f / 19.0f);
            e.velocity = e.velocity * m;
            e.accel = e.accel * m;
            break;
        case Init::OrbitMarker: e.drawType = 2; break;
        case Init::RandomDirection: {
            e.emitter = {pos.x, pos.y, 0.0f};
            const float a = rnd(w, kZunTwoPi) - kZunPi;
            e.direction = {std::cos(a), std::sin(a), 0.0f};
            break;
        }
        case Init::RandomDirectionSpeed: {
            float a;
            if (e.custom.x > -990.0f) a = addNormalizeAngle(e.custom.x, 0.0f);
            else a = rnd(w, kZunTwoPi) - kZunPi;
            e.emitter = {pos.x, pos.y, 0.0f};
            const float s = rnd(w, 1.5f) + 1.0f;
            e.direction = Vec3f{std::cos(a), std::sin(a), 0.0f} * s;
            break;
        }
        case Init::WeatherForward:
            e.base = lookAt + camPos;
            e.base.x += (rnd(w, 120.0f) - 60.0f) + (-lookAt.x) / 2.0f;
            e.base.y += (rnd(w, 200.0f) - 100.0f) + (-lookAt.y) / 2.0f;
            e.base.z += (rnd(w, 100.0f) - 100.0f) + (-lookAt.z) / 2.0f;
            e.velocity.x = rnd(w, 0.06f) - 0.03f + custom.x;
            e.velocity.y = rnd(w, 0.06f) - 0.03f + custom.y;
            e.velocity.z = rnd(w, 0.1f) + 0.03f + custom.z;
            e.accel.x = rnd(w, 0.0002f) - 0.0001f;
            e.accel.y = rnd(w, 0.0002f) - 0.0001f;  // z is not set (stale)
            e.velocity = e.velocity * m;
            e.accel = e.accel * m;
            e.drawType = 1;
            weatherRotations(w, e, kZunPi / 100.0f);
            petalChance(w, e);
            break;
        case Init::WeatherVortex:
        case Init::WeatherBackward:
        case Init::WeatherSlow:
        case Init::WeatherFalling: {
            e.base.x = rnd(w, 160.0f) - 80.0f;
            e.base.y = rnd(w, 160.0f) - 80.0f;
            if (info.init == Init::WeatherSlow) e.base.z = rnd(w, 100.0f) - 100.0f;
            else if (info.init == Init::WeatherFalling) e.base.z = rnd(w, 200.0f);
            else e.base.z = rnd(w, 100.0f) - 50.0f;
            if (info.init == Init::WeatherVortex || info.init == Init::WeatherBackward) {
                e.velocity.x = -e.base.y / custom.x;
                e.velocity.y = e.base.x / custom.x;
                e.velocity.z = info.init == Init::WeatherVortex ? rnd(w, 0.1f) + 0.09f : -rnd(w, 0.2f) - 0.06f;
            } else {
                e.velocity.x = rnd(w, 0.06f) - 0.03f + custom.x;
                e.velocity.y = rnd(w, 0.06f) - 0.03f + custom.y;
                e.velocity.z = info.init == Init::WeatherSlow ? rnd(w, 0.02f) + 0.01f + custom.z : -rnd(w, 0.1f) + custom.z;
            }
            e.base = e.base + lookAt * 0.5f + camPos;
            if (info.init != Init::WeatherSlow) e.velocity = e.velocity * m;
            e.drawType = 1;
            weatherRotations(w, e, kZunPi / 50.0f);
            if (info.init == Init::WeatherVortex) petalChance(w, e);
            else e.vm.selectSprite(*w.anm, 48);
            if (info.init == Init::WeatherFalling) {
                e.vm.angularVelocity.z *= 2.0f;
                e.accel = {0.0f, 0.0f, -0.015f};
            } else {
                e.accel = {};
            }
            break;
        }
    }
}

Effect& Effects::spawn(const World& w, int id, const Vec3f& pos, int count, std::uint32_t color) {
    return fill(w, id, pos, Vec3f{}, count, color, true);
}

Effect& Effects::spawnWithVector(const World& w, int id, const Vec3f& pos, const Vec3f& custom, int count, std::uint32_t color) {
    return fill(w, id, pos, custom, count, color, false);
}

Effect& Effects::fill(const World& w, int id, const Vec3f& pos, const Vec3f& custom, int count, std::uint32_t color, bool plain) {
    Effect& sink = slots_[kSlots - 1];
    if (id < 0 || id >= ExeTables::kEffects || !w.tables) return sink;
    for (int examined = 0; examined < kNormal; ++examined) {
        Effect& e = slots_[static_cast<std::size_t>(cursor_)];
        cursor_ = (cursor_ + 1) % kNormal;
        if (e.inUse) continue;
        init(w, e, id, pos, color, custom, plain);
        if (--count == 0) return e;
    }
    return sink;
}

Effect& Effects::spawnSpecial(const World& w, int id, const Vec3f& pos, int slot, std::uint32_t color) {
    Effect& e = special(slot);
    if (id >= 0 && id < ExeTables::kEffects && w.tables) init(w, e, id, pos, color, Vec3f{}, true);
    return e;
}

bool Effects::runUpdate(const World& w, Effect& e) {
    const float t = e.timer.asFloat();
    switch (e.update) {
        case Update::None:
        case Update::Noop: return true;
        case Update::Physics:
            e.pos += e.velocity;
            e.velocity += e.accel;
            return true;
        case Update::Orbit: {
            const float len = e.direction.length();
            const Vec3f axis = len != 0.0f ? e.direction / len : e.direction;
            const float s = std::sin(e.angularVelocity), c = std::cos(e.angularVelocity);
            const float qx = axis.x * s, qy = axis.y * s, qz = axis.z * s, qw = c;
            Vec3f off{axis.y, -axis.x, 0.0f};
            const float l2 = off.x * off.x + off.y * off.y;
            if (l2 >= 0.00001f) off = off / std::sqrt(l2);
            off = off * e.radius;
            // Row vector times the rotation matrix of the quaternion.
            const float m11 = 1 - 2 * (qy * qy + qz * qz), m12 = 2 * (qx * qy + qz * qw), m13 = 2 * (qx * qz - qy * qw);
            const float m21 = 2 * (qx * qy - qz * qw), m22 = 1 - 2 * (qx * qx + qz * qz), m23 = 2 * (qy * qz + qx * qw);
            const float m31 = 2 * (qx * qz + qy * qw), m32 = 2 * (qy * qz - qx * qw), m33 = 1 - 2 * (qx * qx + qy * qy);
            Vec3f r{off.x * m11 + off.y * m21 + off.z * m31, off.x * m12 + off.y * m22 + off.z * m32, off.x * m13 + off.y * m23 + off.z * m33};
            r.z *= 6.0f;
            e.pos = r + e.emitter;
            if (e.fading) {
                if (++e.fadeCounter >= 16) return false;
                const float f = 1.0f - static_cast<float>(e.fadeCounter) / 16.0f;
                e.vm.color = (e.vm.color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(static_cast<std::int32_t>(f * 255.0f)) << 24);
                e.vm.scale = {2.0f - f, 2.0f - f};
                e.vm.set(AnmVm::ScaleDirty, true);
            }
            return true;
        }
        case Update::Gather60:
        case Update::Gather240: {
            const float n = e.update == Update::Gather60 ? 60.0f : 240.0f;
            const float d = 256.0f - (t * 256.0f) / n;
            e.pos = e.direction * d + e.emitter;
            if (e.update == Update::Gather60) e.pos.z = 0.0f;
            return true;
        }
        case Update::Burst30:
            e.pos = e.direction * ((t * 256.0f) / 30.0f) + e.emitter;
            return true;
        case Update::EaseOutBurst: {
            float s = t / 90.0f;
            s = 1.0f - (1.0f - s) * (1.0f - s);
            e.pos = (e.direction * s) * 128.0f + e.emitter;
            e.pos.z = 0.0f;
            return true;
        }
        case Update::FollowPlayer:
            if (e.vm.finished()) return false;
            if (w.player) e.pos = w.player->position();
            return true;
        case Update::FollowCamera:
            e.drawType = 3;
            if (w.background) {
                const Vec3f b = w.background->cameraFacing() + w.background->cameraPosition();
                e.pos = {b.x, b.y, 0.0f};
            }
            return true;
        case Update::Weather: {
            e.velocity += e.accel;
            e.base += e.velocity;
            e.pos = e.base;
            if (w.background) {
                const Vec3f d = normalize(e.pos - w.background->cameraPosition());
                if (dot(w.background->normalizedFacing(), d) < 0.94f) return false;
            }
            e.vm.rotation.z = addNormalizeAngle(e.vm.rotation.z, e.vm.rotation.x);
            e.vm.set(AnmVm::RotationDirty, true);
            return e.pos.z < 0.0f;
        }
    }
    return true;
}

void Effects::update(const World& w) {
    background_ = w.background;
    active_ = 0;
    layerCount_.fill(0);
    for (int i = 0; i < kSlots - 1; ++i) {
        Effect& e = slots_[static_cast<std::size_t>(i)];
        if (!e.inUse) continue;
        ++active_;
        if (e.update != Update::None && !runUpdate(w, e)) {
            e.inUse = false;
            continue;
        }
        if (e.vm.tick(*w.anm, *w.rng)) {
            e.inUse = false;
            continue;
        }
        e.timer.tick();
        int layer;
        if (e.drawType == 1 || e.drawType == 3) layer = 1;
        else if (e.drawType == 2) layer = 2;
        else layer = e.vm.has(AnmVm::Additive) ? 3 : 0;
        layers_[static_cast<std::size_t>(layer)][static_cast<std::size_t>(layerCount_[static_cast<std::size_t>(layer)]++)] =
            static_cast<std::int16_t>(i);
    }
    ++frames_;
}

void Effects::drawStageLayer(SpriteSink& sink, const AnmDrawState& state, Background& bg) {
    if (quality_ < 2) return;
    for (int k = 0; k < layerCount_[1]; ++k) {
        Effect& e = slots_[static_cast<std::size_t>(layers_[1][static_cast<std::size_t>(k)])];
        if (!e.inUse) continue;
        // The billboard routine rewrites the VM's scale and position for drawing; the
        // effect's own scale must not compound from frame to frame.
        const Vec2f scale = e.vm.scale;
        const std::uint32_t color = e.vm.color;
        if (e.id == 20) {
            const auto channel = [&](int shift, float m) {
                const float c = static_cast<float>((color >> shift) & 0xFFu) * m;
                return static_cast<std::uint32_t>(std::clamp(static_cast<int>(c), 0, 255)) << shift;
            };
            e.vm.color = channel(16, colorMultiplier_[0]) | channel(8, colorMultiplier_[1]) | channel(0, colorMultiplier_[2]) |
                         channel(24, colorMultiplier_[3]);
        }
        e.vm.position = e.pos;
        bg.drawBillboard(sink, e.vm, state);
        e.vm.scale = scale;
        e.vm.color = color;
    }
}

void Effects::draw(SpriteSink& sink, const AnmDrawState& state) {
    for (int layer : {0, 2, 3}) {
        for (int k = 0; k < layerCount_[static_cast<std::size_t>(layer)]; ++k) {
            Effect& e = slots_[static_cast<std::size_t>(layers_[static_cast<std::size_t>(layer)][static_cast<std::size_t>(k)])];
            if (!e.inUse) continue;
            if (layer == 2 && background_) {
                // The orbiting spell circles: camera-facing billboards at the raw (field)
                // position through the playfield camera, so their depth scales them.
                e.vm.position = e.pos;
                background_->drawPlayfieldBillboard(sink, e.vm, state);
                continue;
            }
            e.vm.position = {e.pos.x + kPlayfieldX, e.pos.y + kPlayfieldY, e.pos.z};
            drawVm(sink, e.vm, state);
        }
    }
}

}  // namespace touhou::th07
