#include "th08/Effects.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/SpriteSink.hpp"
#include "th08/Background.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Player.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {
enum List { ListA, ListB, ListC, ListD, ListE };
}  // namespace

void Effects::reset() {
    for (Effect& e : slots_) e = Effect{};
    sink_ = Effect{};
    cursor_ = 0;
    active_ = 0;
    frames_ = 0;
    listCount_.fill(0);
}

void Effects::start(const World& w, Effect& e, int type, const Vec3f& pos, std::uint32_t color, const Vec3f* custom, bool zwriteOff) {
    e = Effect{};
    e.timer = Timer{0, 0.0f, 0};
    e.inUse = true;
    e.type = static_cast<std::uint8_t>(type);
    e.pos = pos;
    if (custom) e.custom = *custom;
    if (w.tables && w.anm && w.rng && type >= 0 && type < ExeTables::kEffectTypes) {
        const ExeTables::Effect& info = w.tables->effects[static_cast<std::size_t>(type)];
        e.vm.start(*w.anm, *w.rng, kSlotEtama, info.script);  // its time-0 draws happen now
        e.update = info.update;
        e.init = info.init;
    }
    if (zwriteOff) e.vm.set(AnmVm::ZWriteDisable, true);
    e.vm.color = color;
    e.vm.offset = {};
}

Effect& Effects::fill(const World& w, int type, const Vec3f& pos, const Vec3f* custom, int count, std::uint32_t color, bool zwriteOff) {
    Effect* p = &slots_[static_cast<std::size_t>(cursor_)];
    int steps = 0;
    while (steps < kRing) {
        cursor_ = (cursor_ + 1) % kRing;
        if (p->inUse) {
            p = &slots_[static_cast<std::size_t>(cursor_)];
            ++steps;
            continue;
        }
        start(w, *p, type, pos, color, custom, zwriteOff);
        if (!runInit(w, *p)) p->inUse = false;
        if (--count <= 0) return *p;
        p = &slots_[static_cast<std::size_t>(cursor_)];
        ++steps;
    }
    return sink_;  // the ring ran out (some may have spawned)
}

Effect& Effects::spawn(const World& w, int type, const Vec3f& pos, int count, std::uint32_t color) {
    return fill(w, type, pos, nullptr, count, color, true);
}

Effect& Effects::spawnWithVector(const World& w, int type, const Vec3f& pos, const Vec3f& custom, int count, std::uint32_t color) {
    return fill(w, type, pos, &custom, count, color, false);
}

void Effects::shiftWeather(const Vec3f& delta) {
    for (int i = 0; i < kRing; ++i) {
        Effect& e = slots_[static_cast<std::size_t>(i)];
        if (e.type == 51) e.world = e.world + delta;
    }
}

Effect& Effects::spawnSpecial(const World& w, int type, const Vec3f& pos, int k, int count, std::uint32_t color) {
    if (k < 0 || k >= kSpecial) return sink_;
    (void)count;  // ignored: one effect
    Effect& e = special(k);
    start(w, e, type, pos, color);
    if (!runInit(w, e)) e.inUse = false;
    return e;
}

namespace {

float fR(ZunRng& r, float range) { return r.f32InRange(range); }
float fsR(ZunRng& r, float range) { return (static_cast<float>(r.u32()) / 2147483648.0f - 1.0f) * range; }

}  // namespace

// The per-type init routines (spec_effects 5), by the table's routine address.
bool Effects::runInit(const World& w, Effect& e) {
    if (!w.rng) return true;
    ZunRng& rng = *w.rng;
    const float m = g_timing.speed;
    switch (e.init) {
        case 0x425D70:  // sparks 4-11
        case 0x425EA0: {  // spark 3
            const bool three = e.init == 0x425EA0;
            if (three) {
                e.velocity.x = ((fR(rng, 256.0f) - 128.0f) * 4.0f) / 33.0f;
                e.velocity.y = ((fR(rng, 256.0f) - 128.0f) * 4.0f) / 33.0f;
            } else {
                e.velocity.x = (fR(rng, 256.0f) - 128.0f) / 12.0f;
                e.velocity.y = (fR(rng, 256.0f) - 128.0f) / 12.0f;
            }
            e.velocity.z = 0.0f;
            const float k = three ? 1.0f / 20.0f : 1.0f / 19.0f;
            e.accel = Vec3f{-e.velocity.x, -e.velocity.y, -e.velocity.z} * k;
            e.velocity = e.velocity * m;
            e.accel = e.accel * m;
            return true;
        }
        case 0x425FE0:  // spell circle
            e.layer = 2;
            e.direction = {};
            e.radius = 0.0f;
            return true;
        case 0x426B20: {  // charge particles 17, 18, 27
            e.centre = {e.pos.x, e.pos.y, 0.0f};
            const float a = fR(rng, kZunTwoPi) - kZunPi;
            e.direction = {std::cos(a), std::sin(a), 0.0f};
            return true;
        }
        case 0x4270C0: {  // bursts 21, 26
            float a;
            if (e.custom.x > -990.0f) a = addNormalizeAngle(e.custom.x, 0.0f);
            else a = fR(rng, kZunTwoPi) - kZunPi;
            e.centre = {e.pos.x, e.pos.y, 0.0f};
            e.direction = Vec3f{std::cos(a), std::sin(a), 0.0f} * (fR(rng, 1.5f) + 1.0f);
            return true;
        }
        case 0x427260:  // 60, 61
            e.pos.x += static_cast<float>(std::cos(static_cast<double>(e.custom.x)) * 256.0);
            e.pos.y += static_cast<float>(std::sin(static_cast<double>(e.custom.x)) * 256.0);
            e.vm.rotation.z = addNormalizeAngle(e.custom.x, kZunPi / 2.0f);
            return true;
        case 0x4272E0:  // rings
        case 0x427970:
            e.ring = true;
            e.segments = 24;
            e.centre = e.pos;
            e.angle = e.custom.x;
            e.radius = e.custom.y;
            e.width = e.custom.z;
            if (e.init == 0x427970) e.listD = true;
            return true;
        case 0x426280:  // weather 51
        case 0x426720: {  // weather 63
            const Background* bg = w.background;
            const Vec3f facing = bg ? bg->cameraFacing() : Vec3f{};
            const Vec3f cam = bg ? bg->cameraPosition() : Vec3f{};
            const Vec3f n{-facing.x, -facing.y, -facing.z};
            e.world = facing + cam;
            e.world.x = (fsR(rng, 60.0f) + n.x / 2.0f) + e.world.x;
            if (e.init == 0x426280) e.world.y = ((fsR(rng, 100.0f) - 50.0f) + n.y / 2.0f) + e.world.y;
            else e.world.y = ((fsR(rng, 200.0f) - 200.0f) + n.y / 2.0f) + e.world.y;
            e.world.z = ((fR(rng, 100.0f) - 100.0f) + n.z / 2.0f) + e.world.z;
            e.velocity.x = fsR(rng, 0.001f) + e.custom.x;
            e.velocity.y = e.init == 0x426280 ? fsR(rng, 0.03f) + e.custom.y : fsR(rng, 0.03f) + 0.4f;
            e.velocity.z = (-fR(rng, 0.1f)) - 0.3f + e.custom.z;
            e.accel.x = fsR(rng, 0.0001f);
            e.accel.y = fsR(rng, 0.0001f);
            e.accel.z = -0.0003f;
            e.velocity = e.velocity * m;
            e.accel = e.accel * m;
            e.layer = 1;
            e.vm.offset.x = -9999.0f;
            return true;
        }
        default: return true;
    }
}

// The per-type update routines (spec_effects 5): false frees the effect.
bool Effects::runUpdate(const World& w, Effect& e) {
    switch (e.update) {
        case 0x425E60:  // sparks
            e.pos += e.velocity;
            e.velocity += e.accel;
            return true;
        case 0x426030: {  // spell circle: the enemy keeps its centre, axis, radius and angle
            const Vec3f n = normalize(e.direction);
            const float sn = std::sin(e.angle), cs = std::cos(e.angle);
            const float qx = n.x * sn, qy = n.y * sn, qz = n.z * sn, qw = cs;
            Vec3f v{n.y, -n.x, 0.0f};
            if (v.x * v.x + v.y * v.y + v.z * v.z >= 1e-5f) v = normalize(v);
            v = v * e.radius;
            // v x R(q), D3DX row-vector convention.
            const float m00 = 1 - 2 * (qy * qy + qz * qz), m01 = 2 * (qx * qy + qz * qw), m02 = 2 * (qx * qz - qy * qw);
            const float m10 = 2 * (qx * qy - qz * qw), m11 = 1 - 2 * (qx * qx + qz * qz), m12 = 2 * (qy * qz + qx * qw);
            const float m20 = 2 * (qx * qz + qy * qw), m21 = 2 * (qy * qz - qx * qw);
            (void)m02, (void)m12;
            const Vec3f r{v.x * m00 + v.y * m10 + v.z * m20, v.x * m01 + v.y * m11 + v.z * m21, 0.0f};
            e.pos = {e.centre.x + r.x, e.centre.y + r.y, 0.0f};
            if (e.fade != 0) {
                if (++e.fadeCounter >= 16) return false;
                const float f = 1.0f - static_cast<float>(e.fadeCounter) * (1.0f / 16.0f);
                e.vm.color = (e.vm.color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(f * 255.0f) << 24);
                e.vm.scale = {2.0f - f, 2.0f - f};
            }
            return true;
        }
        case 0x426BB0:
        case 0x426C90: {
            const float span = e.update == 0x426BB0 ? 60.0f : 240.0f;
            const float r = 256.0f - (e.timer.asFloat() * 256.0f / span);
            e.pos = e.centre + e.direction * r;
            e.pos.z = 0.0f;
            return true;
        }
        case 0x4271A0: {
            const float u = e.timer.asFloat() / 90.0f;
            const float k = 1.0f - (1.0f - u) * (1.0f - u);
            e.pos = (e.direction * k) * 128.0f + e.centre;
            e.pos.z = 0.0f;
            return true;
        }
        case 0x426C40:  // the focus effect follows the player
            if (!e.vm.current) return false;
            if (w.player) e.pos = w.player->position();
            return true;
        case 0x427990:  // rings 35, 39, 49, 52, 59
            e.width = e.vm.scale.x;
            e.radius = e.vm.position.x;
            return true;
        case 0x4279D0:  // rings 40, 55, 56, 58, 65
            e.segments = e.vm.intRegs[0];
            e.waves = static_cast<float>(e.vm.intRegs[1]);
            e.width = e.vm.scale.x;
            e.radius = e.vm.position.x;
            e.radius2 = e.vm.position.y;
            e.angle = e.vm.rotation.z;
            e.phase = e.vm.rotation.y;
            return true;
        case 0x427A60:  // the Dissolve ring
            e.segments = 32;
            e.width = e.vm.scale.x;
            e.radius = e.vm.position.x;
            e.radius2 = e.vm.position.y;
            return e.timer.current < 120;
        case 0x427AE0:  // rings 53, 54
            e.width = e.vm.scale.x;
            e.radius = e.vm.position.x;
            e.radius2 = e.vm.position.y;
            e.angle = e.vm.rotation.z;
            return (e.vm.color >> 24) != 0;
        case 0x427B50:  // the 6B moon
            e.segments = e.vm.intRegs[0];
            e.waves = static_cast<float>(e.vm.intRegs[1]);
            e.width = e.vm.scale.x;
            e.radius = e.vm.floatRegs[1];
            e.angle = e.vm.rotation.z;
            e.phase = e.vm.rotation.y;
            e.centre = e.vm.position;
            return true;
        case 0x4264F0:
        case 0x426990: {
            e.velocity += e.accel;
            e.world += e.velocity;
            e.pos = e.world;
            if (w.background) {
                const Vec3f d = normalize(e.pos - w.background->cameraPosition());
                const Vec3f f = w.background->normalizedFacing();
                const float c = f.x * d.x + f.y * d.y + f.z * d.z;
                if (c < 0.94f) return false;
            }
            return true;
        }
        default: return true;
    }
}

void Effects::update(const World& w) {
    listCount_.fill(0);
    active_ = 0;
    auto list = [&](int l, int slot) { lists_[static_cast<std::size_t>(l)][static_cast<std::size_t>(listCount_[static_cast<std::size_t>(l)]++)] = static_cast<std::int16_t>(slot); };
    for (int i = 0; i < kSlots; ++i) {
        Effect& e = slots_[static_cast<std::size_t>(i)];
        if (!e.inUse) continue;
        ++active_;
        if (!(w.mode & ModeSpellFreeze) || e.ignoreFreeze) {
            if (!runUpdate(w, e)) {
                e.inUse = false;
                continue;
            }
            if (w.rng && e.vm.tick(*w.rng)) {
                e.inUse = false;
                continue;
            }
            e.timer.tick();
        }
        if (e.type == 64) continue;  // the 6B moon: drawn by the stage itself
        if (e.layer == 1 || e.layer >= 3) list(ListB, i);
        else if (e.layer == 2) list(ListC, i);
        else if (e.listD) list(ListD, i);
        else if (e.vm.blend() == 1) list(ListE, i);
        else list(ListA, i);
    }
    ++frames_;
}

// The ring/strip drawer (spec_effects 4.5): N + 1 vertex pairs around the centre, outer first,
// drawn as N quads. Visual only.
void Effects::drawRing(SpriteSink& sink, const Effect& e, const AnmDrawState& state) {
    const AnmVm& vm = e.vm;
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    const int n = std::clamp(e.segments, 1, 128);
    const float step = kZunTwoPi / static_cast<float>(n);
    const float h = e.width / std::sin((kZunPi - step) / 2.0f);
    const float psiStep = kZunTwoPi * e.waves / static_cast<float>(n);
    std::array<float, 129 * 4> pts{};  // outer x, y, inner x, y
    float theta = e.angle, phi = 0.0f, psi = e.phase;
    const float ct = std::cos(e.phase), st = std::sin(e.phase);
    for (int i = 0; i <= n; ++i) {
        if (theta >= kZunPi) theta -= kZunTwoPi;
        float ox, oy, ix, iy;
        if (e.radius2 == 0.0f) {
            ox = std::cos(theta) * (e.radius + h), oy = std::sin(theta) * (e.radius + h);
            ix = std::cos(theta) * (e.radius - h), iy = std::sin(theta) * (e.radius - h);
        } else if (e.waves == 0.0f) {
            const float ex0 = std::cos(phi) * (e.radius + h), ey0 = std::sin(phi) * (e.radius2 + h);
            const float ex1 = std::cos(phi) * (e.radius - h), ey1 = std::sin(phi) * (e.radius2 - h);
            ox = ex0 * ct - ey0 * st, oy = ex0 * st + ey0 * ct;
            ix = ex1 * ct - ey1 * st, iy = ex1 * st + ey1 * ct;
        } else {
            const float wave = std::cos(psi) * e.radius2;
            ox = std::cos(theta) * (e.radius + h + wave), oy = std::sin(theta) * (e.radius + h + wave);
            ix = std::cos(theta) * (e.radius - h + wave), iy = std::sin(theta) * (e.radius - h + wave);
            psi += psiStep;
        }
        const float cx = e.centre.x + kPlayfieldX + state.offsetX + 0.5f, cy = e.centre.y + kPlayfieldY + state.offsetY + 0.5f;
        float* p = &pts[static_cast<std::size_t>(i) * 4];
        p[0] = cx + ox, p[1] = cy + oy, p[2] = cx + ix, p[3] = cy + iy;
        theta += step;
        phi += step;
    }
    const std::uint32_t color = argbToRgbaBytes(effectiveColor(vm, state));
    const float u0 = vm.sprite->uvStartX + vm.uvScroll.x, u1 = vm.sprite->uvEndX + vm.uvScroll.x;
    const float v0 = vm.sprite->uvStartY + vm.uvScroll.y, v1 = vm.sprite->uvEndY + vm.uvScroll.y;
    Draw3DState ds;
    ds.depthWrite = false;
    ds.clipX = kPlayfieldX, ds.clipY = kPlayfieldY, ds.clipW = kPlayfieldW, ds.clipH = kPlayfieldH;
    constexpr float kW = 640.0f, kH = 480.0f;
    for (int i = 0; i < n; ++i) {
        const float* a = &pts[static_cast<std::size_t>(i) * 4];
        const float* b = &pts[static_cast<std::size_t>(i + 1) * 4];
        const float ua = u0 + (u1 - u0) * static_cast<float>(i) / static_cast<float>(n);
        const float ub = u0 + (u1 - u0) * static_cast<float>(i + 1) / static_cast<float>(n);
        const float xs[4] = {a[0], b[0], a[2], b[2]}, ys[4] = {a[1], b[1], a[3], b[3]};
        const float us[4] = {ua, ub, ua, ub}, vs[4] = {v0, v0, v1, v1};
        Vertex3D v[4];
        for (int k = 0; k < 4; ++k) {
            v[k].x = xs[k] / kW * 2.0f - 1.0f;
            v[k].y = 1.0f - ys[k] / kH * 2.0f;
            v[k].z = -1.0f;
            v[k].w = 1.0f;
            v[k].u = us[k];
            v[k].v = vs[k];
            v[k].color = color;
        }
        sink.drawQuad3D(vm.sprite->texture, drawBlend(vm), v, ds);
    }
}

void Effects::draw(SpriteSink& sink, const AnmDrawState& state, Background* bg) {
    auto draw2D = [&](int l, float z) {
        for (int k = 0; k < listCount_[static_cast<std::size_t>(l)]; ++k) {
            Effect& e = slots_[static_cast<std::size_t>(lists_[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)])];
            if (e.ring) {
                drawRing(sink, e, state);
                continue;
            }
            e.vm.position = Vec3f{e.pos.x + kPlayfieldX, e.pos.y + kPlayfieldY, z} + e.vm.offset;
            drawVm(sink, e.vm, state);
        }
    };
    draw2D(ListA, 0.07f);
    for (int k = 0; k < listCount_[ListC]; ++k) {
        Effect& e = slots_[static_cast<std::size_t>(lists_[ListC][static_cast<std::size_t>(k)])];
        e.vm.position = e.pos;
        if (bg) bg->drawPlayfieldBillboard(sink, e.vm, state);
    }
    draw2D(ListE, 0.07f);
}

void Effects::drawListD(SpriteSink& sink, const AnmDrawState& state) {
    for (int k = 0; k < listCount_[ListD]; ++k) {
        Effect& e = slots_[static_cast<std::size_t>(lists_[ListD][static_cast<std::size_t>(k)])];
        if (e.ring) {
            drawRing(sink, e, state);
            continue;
        }
        e.vm.position = {e.pos.x + kPlayfieldX + e.vm.offset.x, e.pos.y + kPlayfieldY + e.vm.offset.y, 0.04f};
        drawVm(sink, e.vm, state);
    }
}

void Effects::drawListB(SpriteSink& sink, const AnmDrawState& state, Background& bg) {
    if (quality_ <= 1) return;  // quality 1 stops at the first (odd-counted) effect: nothing [QUIRK]
    for (int k = 0; k < listCount_[ListB]; ++k) {
        Effect& e = slots_[static_cast<std::size_t>(lists_[ListB][static_cast<std::size_t>(k)])];
        if (e.layer == 4) {  // the lantern glows: screen positions
            e.vm.position = e.pos;
            drawVm(sink, e.vm, state);
        } else if (e.layer == 1) {
            e.vm.position = e.pos;
            bg.drawBillboard(sink, e.vm, state);
        } else {
            e.vm.position = e.pos;
            bg.drawProjected(sink, e.vm, state);
        }
    }
}

}  // namespace touhou::th08
