#include "game/EffectManager.hpp"

#include <cmath>

namespace th06 {
namespace {

using Motion = EffectManager::Motion;

struct TypeInfo {
    int script;  // global ANM script index
    Motion motion;
};

// etama4.anm is loaded at 0x29a: scripts 3..19 are the effects (0..2 are bubble bullets).
constexpr TypeInfo kTypeTable[EffectManager::kTypes] = {
    {0x29a + 3, Motion::None},       {0x29a + 4, Motion::None},       {0x29a + 5, Motion::None},
    {0x29a + 6, Motion::BigSplash},  {0x29a + 9, Motion::Splash},     {0x29a + 10, Motion::Splash},
    {0x29a + 11, Motion::Splash},    {0x29a + 12, Motion::Splash},    {0x29a + 13, Motion::Splash},
    {0x29a + 14, Motion::Splash},    {0x29a + 15, Motion::Splash},    {0x29a + 16, Motion::Splash},
    {0x29a + 17, Motion::None},      {0x29a + 18, Motion::Orbit},     {0x29a + 18, Motion::Orbit},
    {0x29a + 18, Motion::Orbit},     {kAnmOffsetEffects + 0, Motion::None},
    {0x29a + 7, Motion::Converge60}, {0x29a + 8, Motion::Converge240}, {0x29a + 19, Motion::Drift},
};

constexpr std::uint32_t kColorTable[28] = {
    0xff000000, 0xff303030, 0xff606060, 0xff500000, 0xff900000, 0xffff2020, 0xff400040,
    0xff800080, 0xffff30ff, 0xff000050, 0xff000090, 0xff2020ff, 0xff203060, 0xff304090,
    0xff3080ff, 0xff005000, 0xff009000, 0xff20ff20, 0xff206000, 0xff409010, 0xff80ff20,
    0xff505000, 0xff909000, 0xffffff20, 0xff603000, 0xff904010, 0xfff08020, 0xffffffff,
};

float length3(const Vec3f& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Burst velocity: two draws (x, then y) in [0, 256], centred and scaled.
void startSplash(World& world, EffectManager::Effect& e, float scale, float stopFrames) {
    e.velocity.x = (world.rng->f32InRange(256.0f) - 128.0f) * scale;
    e.velocity.y = (world.rng->f32InRange(256.0f) - 128.0f) * scale;
    e.velocity.z = 0.0f;
    e.acceleration = e.velocity / -stopFrames;
}

}  // namespace

std::uint32_t EffectManager::colorTable(int index) {
    return index >= 0 && index < 28 ? kColorTable[index] : 0xffffffffu;
}

void EffectManager::reset() {
    effects_ = {};
    cursor_ = 0;
}

int EffectManager::activeCount() const {
    int n = 0;
    for (int i = 0; i < kMaxEffects; ++i) n += effects_[static_cast<std::size_t>(i)].inUse;
    return n;
}

int EffectManager::spawn(World& world, int type, const Vec3f& pos, int count, std::uint32_t color) {
    if (type < 0 || type >= kTypes) {  // reads past the type table in the original
        world.unported->hit("effect type out of range");
        return kSink;
    }
    const TypeInfo& info = kTypeTable[type];
    for (int examined = 0; examined < kMaxEffects; ++examined) {
        const int index = cursor_;
        cursor_ = (cursor_ + 1) % kMaxEffects;
        Effect& e = effects_[static_cast<std::size_t>(index)];
        if (e.inUse) continue;
        e.inUse = true;
        e.type = static_cast<std::uint8_t>(type);
        e.pos = e.prevPos = pos;
        world.anm->setAndExecuteScript(e.vm, info.script);
        e.vm.color = color;  // replaces whatever frame 0 of the script set
        e.motion = info.motion;
        e.timer = 0;
        e.fading = false;
        e.fadeCounter = 0;
        if (--count == 0) return index;  // a count <= 0 never gets here: every free slot is filled
    }
    return kSink;
}

void EffectManager::update(World& world) {
    const GameState& gs = *world.state;
    viewX_ = gs.playfieldX;
    viewY_ = gs.playfieldY;
    viewW_ = gs.playfieldW;
    viewH_ = gs.playfieldH;
    for (int i = 0; i < kMaxEffects; ++i) {
        Effect& e = effects_[static_cast<std::size_t>(i)];
        if (!e.inUse) continue;
        e.prevPos = e.pos;
        const bool first = e.timer == 0;
        bool finished = false;
        switch (e.motion) {
            case Motion::None:
                break;
            case Motion::Splash:
                if (first) startSplash(world, e, 1.0f / 12.0f, 19.0f);
                e.pos += e.velocity;
                e.velocity += e.acceleration;  // never clamped: the particle drifts back if it lives on
                break;
            case Motion::BigSplash:
                if (first) startSplash(world, e, 4.0f / 33.0f, 20.0f);
                e.pos += e.velocity;
                e.velocity += e.acceleration;
                break;
            case Motion::Drift:
                e.pos += e.velocity;
                e.velocity += e.acceleration;
                break;
            case Motion::Converge60:
            case Motion::Converge240: {
                const float period = e.motion == Motion::Converge60 ? 60.0f : 240.0f;
                if (first) {
                    e.anchor = e.pos;
                    const float theta = world.rng->f32InRange(kZunTwoPi) - kZunPi;
                    e.axis = {std::cos(theta), std::sin(theta), 0.0f};
                }
                const float rho = 256.0f - static_cast<float>(e.timer) * 256.0f / period;
                e.pos = e.anchor + e.axis * rho;
                if (first) e.prevPos = e.pos;  // it appears out there, it does not streak there
                break;
            }
            case Motion::Orbit: {
                const float len = length3(e.axis);
                const Vec3f n = len > 0.0f ? e.axis / len : Vec3f{};
                const float theta = e.halfAngle * 2.0f;
                Vec3f u{n.y, -n.x, 0.0f};
                const float u2 = u.x * u.x + u.y * u.y;
                if (u2 >= 0.00001f) u = u / std::sqrt(u2);  // an axis along z stays unnormalised (original)
                Vec3f w = (u * std::cos(theta) + cross(n, u) * std::sin(theta)) * e.radius;
                w.z *= 6.0f;
                e.pos = e.anchor + w;
                if (e.fading) {
                    if (++e.fadeCounter >= 16) {
                        finished = true;
                    } else {
                        const float a = 1.0f - static_cast<float>(e.fadeCounter) / 16.0f;
                        e.vm.color = (e.vm.color & 0x00ffffffu) | (static_cast<std::uint32_t>(a * 255.0f) << 24);
                        e.vm.scaleX = e.vm.scaleY = 2.0f - a;
                    }
                }
                break;
            }
        }
        if (finished) e.inUse = false;
        if (world.anm->executeScript(e.vm) != 0) e.inUse = false;
        ++e.timer;
    }
}

void EffectManager::draw(World& world, SpriteSink& sink, float alpha) {
    // Perspective camera: 30 degree vertical field of view looking at the playfield, so a
    // point at depth z is scaled by d / (d + z) about the playfield centre.
    const float d = (viewH_ / 2.0f) / std::tan(15.0f * kZunPi / 180.0f);
    for (int i = 0; i < kMaxEffects; ++i) {
        const Effect& e = effects_[static_cast<std::size_t>(i)];
        if (!e.inUse) continue;
        const Vec3f p = lerp(e.prevPos, e.pos, alpha);
        if (d + p.z < 100.0f) continue;  // in front of the near plane
        const float s = d / (d + p.z);
        AnmVm vm = e.vm;
        vm.pos = {viewW_ / 2.0f + (p.x - viewW_ / 2.0f) * s, viewH_ / 2.0f + (p.y - viewH_ / 2.0f) * s, 0.0f};
        vm.scaleX *= s;
        vm.scaleY *= s;
        world.anm->drawWorld(sink, vm, viewX_, viewY_, false);
    }
}

int spawnEffect(World& world, int type, const Vec3f& pos, int count, std::uint32_t color) {
    return world.effects ? world.effects->spawn(world, type, pos, count, color) : EffectManager::kSink;
}

}  // namespace th06
