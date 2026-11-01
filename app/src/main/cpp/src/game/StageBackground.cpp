#include "game/StageBackground.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "game/EffectManager.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

constexpr float kTargetW = 640.0f, kTargetH = 480.0f;  // the game's virtual screen
constexpr float kNear = 100.0f;
constexpr float kDepthScale = 10000.0f / 9900.0f;
constexpr int kScriptSpellBackground = kAnmOffsetEffects + 0;

template <class T>
T rd(std::span<const std::uint8_t> b, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

Vec3f rdVec(std::span<const std::uint8_t> b, std::size_t at) {
    return {rd<float>(b, at), rd<float>(b, at + 4), rd<float>(b, at + 8)};
}

float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3f cross(const Vec3f& a, const Vec3f& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(const Vec3f& v) { return std::sqrt(dot(v, v)); }

// Rotation about X, then Y, then Z (world frame, +Y up).
Vec3f rotate(Vec3f p, const Vec3f& r) {
    if (r.x != 0.0f) {
        const float c = std::cos(r.x), s = std::sin(r.x);
        p = {p.x, p.y * c - p.z * s, p.y * s + p.z * c};
    }
    if (r.y != 0.0f) {
        const float c = std::cos(r.y), s = std::sin(r.y);
        p = {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
    }
    if (r.z != 0.0f) {
        const float c = std::cos(r.z), s = std::sin(r.z);
        p = {p.x * c - p.y * s, p.x * s + p.y * c, p.z};
    }
    return p;
}

std::uint32_t lerpColor(std::uint32_t from, std::uint32_t to, float t) {
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float a = static_cast<float>((from >> shift) & 0xff), b = static_cast<float>((to >> shift) & 0xff);
        const auto v = static_cast<std::uint32_t>(std::clamp(static_cast<int>(a + (b - a) * t), 0, 255));
        out |= v << shift;
    }
    return out;
}

}  // namespace

void StageBackground::clear() {
    loaded_ = false;
    objects_.clear();
    instances_.clear();
    script_.clear();
    vms_.clear();
}

bool StageBackground::parse(std::span<const std::uint8_t> b, std::string* error) {
    clear();
    auto fail = [&](const char* why) {
        if (error) *error = why;
        clear();
        return false;
    };
    if (b.size() < 0x490) return fail("STD header cut off");
    const auto nObj = rd<std::int16_t>(b, 0);
    const auto nQuad = rd<std::int16_t>(b, 2);
    const auto instOff = rd<std::int32_t>(b, 4);
    const auto scriptOff = rd<std::int32_t>(b, 8);
    if (nObj < 0 || nQuad < 0 || 0x490 + 4 * static_cast<std::size_t>(nObj) > b.size()) return fail("STD object table out of range");
    int vmCount = 0;
    for (int o = 0; o < nObj; ++o) {
        const std::size_t off = rd<std::uint32_t>(b, 0x490 + 4 * static_cast<std::size_t>(o));
        if (off + 0x1c > b.size()) return fail("STD object out of range");
        Object ob;
        ob.layer = rd<std::int8_t>(b, off + 2);
        ob.boxOrigin = rdVec(b, off + 4);
        ob.boxSize = rdVec(b, off + 0x10);
        std::size_t q = off + 0x1c;
        while (true) {
            if (q + 4 > b.size()) return fail("STD quad list runs off the file");
            const auto type = rd<std::int16_t>(b, q);
            const auto size = rd<std::int16_t>(b, q + 2);
            if (type < 0) break;
            if (size <= 0 || q + 0x1c > b.size()) return fail("STD quad record malformed");
            Quad quad;
            quad.type = type;
            quad.script = rd<std::int16_t>(b, q + 4);
            quad.vm = vmCount++;
            quad.pos = rdVec(b, q + 8);
            quad.width = rd<float>(b, q + 0x14);
            quad.height = rd<float>(b, q + 0x18);
            ob.quads.push_back(quad);
            q += static_cast<std::size_t>(size);
        }
        objects_.push_back(std::move(ob));
    }
    if (vmCount > nQuad) return fail("STD has more quads than its header says");
    if (instOff < 0 || scriptOff < 0) return fail("STD offsets negative");
    for (std::size_t p = static_cast<std::size_t>(instOff);; p += 0x10) {
        if (p + 2 > b.size()) return fail("STD instance list runs off the file");
        const auto obj = rd<std::int16_t>(b, p);
        if (obj < 0) break;
        if (obj >= nObj || p + 0x10 > b.size()) return fail("STD instance refers to a missing object");
        instances_.push_back({obj, rdVec(b, p + 4)});
    }
    for (std::size_t p = static_cast<std::size_t>(scriptOff); p + 0x14 <= b.size(); p += 0x14) {
        Record r;
        r.time = rd<std::int32_t>(b, p);
        r.op = rd<std::int16_t>(b, p + 4);
        for (int i = 0; i < 3; ++i) r.a[i] = rd<std::uint32_t>(b, p + 8 + 4 * static_cast<std::size_t>(i));
        script_.push_back(r);
        if (r.op < 0 || r.op > 5) break;  // the end record (or anything else) stops the script
    }
    vms_.resize(static_cast<std::size_t>(vmCount));
    loaded_ = true;
    return true;
}

bool StageBackground::load(World& world, const Vfs& vfs, int stage) {
    clear();
    std::string err;
    AnmManager& anm = *world.anm;
    anm.releaseAnm(kAnmFileStageBg);
    const std::string art = std::format("data/stg{}bg.anm", stage), data = std::format("data/stage{}.std", stage);
    if (!anm.loadAnm(vfs, kAnmFileStageBg, art, kAnmOffsetStageBg, &err)) {
        log::warn("No stage background: {}", err);
        return false;
    }
    auto bytes = vfs.read(data);
    if (!bytes) {
        log::warn("No stage background: {} not found", data);
        return false;
    }
    if (!parse(*bytes, &err)) {
        log::warn("No stage background: {}: {}", data, err);
        return false;
    }
    start(world);
    log::info("Stage background: {} objects, {} instances, {} script records", objects_.size(), instances_.size(),
              script_.size());
    return true;
}

void StageBackground::start(World& world) {
    for (Object& ob : objects_) {
        ob.animating = true;
        for (const Quad& q : ob.quads) {
            AnmVm& vm = vms_[static_cast<std::size_t>(q.vm)];
            vm = AnmVm{};
            world.anm->setAndExecuteScript(vm, kAnmOffsetStageBg + q.script);
        }
    }
    scriptTime_ = 0;
    instr_ = 0;
    camera_ = prevCamera_ = p0_ = p1_ = {};
    t0_ = t1_ = 0;
    spell_ = Spell::Off;
    spellCounter_ = 0;
    spellVm_ = AnmVm{};
    fogDuration_ = fogTimer_ = 0;
    fogColor_ = 0xff000000u;
    fogStartZ_ = 200.0f;
    fogEndZ_ = 500.0f;
    aimFrom_ = aimTo_ = {0.0f, 0.0f, 1.0f};
    aimDuration_ = 1;
    aimTimer_ = 0;
    unpause_ = false;
    cameraAim_ = aim_;
}

void StageBackground::buildCamera(Camera& cam, const Vec3f& aim) const {
    const float d = (cam.h / 2.0f) / std::tan(15.0f * kZunPi / 180.0f);
    cam.cot = 1.0f / std::tan(15.0f * kZunPi / 180.0f);
    cam.eye = {cam.w / 2.0f, -cam.h / 2.0f, -d * aim.z};
    const Vec3f target{cam.w / 2.0f + aim.x, -cam.h / 2.0f + aim.y, 0.0f};
    Vec3f f = target - cam.eye;
    Vec3f r{f.z, 0.0f, -f.x};
    if (length(f) <= 0.0f || length(r) <= 0.0f) {  // degenerate aim: look straight ahead
        cam.eye = {cam.w / 2.0f, -cam.h / 2.0f, -d};
        f = {0.0f, 0.0f, d};
        r = {d, 0.0f, 0.0f};
    }
    cam.forward = f / length(f);
    cam.right = r / length(r);
    cam.up = cross(cam.forward, cam.right);
}

void StageBackground::project(const Vec3f& q, float& sx, float& sy, float& eyeZ) const {
    const Camera& c = camera3D_;
    const Vec3f rel = Vec3f{q.x, -q.y, q.z} - c.eye;
    const float xe = dot(c.right, rel), ye = dot(c.up, rel), ze = dot(c.forward, rel);
    eyeZ = ze;
    sx = c.x + c.w / 2.0f * (1.0f + c.cot * (c.h / c.w) * xe / ze);
    sy = c.y + c.h / 2.0f * (1.0f - c.cot * ye / ze);
}

bool StageBackground::instanceVisible(const Instance& inst, const Vec3f& camera) const {
    const Object& ob = objects_[static_cast<std::size_t>(inst.object)];
    const Vec3f base = ob.boxOrigin + inst.pos - camera;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                const Vec3f corner{base.x + static_cast<float>(i) * ob.boxSize.x, base.y + static_cast<float>(j) * ob.boxSize.y,
                                   base.z + static_cast<float>(k) * ob.boxSize.z};
                float sx, sy, ze;
                project(corner, sx, sy, ze);
                if (ze != 0.0f && sy >= camera3D_.y && sy <= camera3D_.y + camera3D_.h) return true;
            }
        }
    }
    return false;
}

// ---- update ---------------------------------------------------------------------------

void StageBackground::update(World& world) {
    if (!loaded_) return;
    const GameState& gs = *world.state;
    // The camera of this frame: the playfield rectangle (whole pixels) and last frame's aim.
    camera3D_.x = std::trunc(gs.playfieldX);
    camera3D_.y = std::trunc(gs.playfieldY);
    camera3D_.w = std::trunc(gs.playfieldW);
    camera3D_.h = std::trunc(gs.playfieldH);
    cameraAim_ = aim_;
    buildCamera(camera3D_, cameraAim_);
    prevCamera_ = camera_;
    // Time stop: everything holds, and the spell background is tinted dark red (the engine
    // never undoes the tint; the next spell card's script start does).
    if (gs.timeStop != 0) {
        spellVm_.color = 0x60803030u;
        return;
    }

    // Script: at most one camera key per frame.
    auto vec = [](const Record& r) { return Vec3f{std::bit_cast<float>(r.a[0]), std::bit_cast<float>(r.a[1]), std::bit_cast<float>(r.a[2])}; };
    std::int32_t stopTime = -1;
    bool stopPause = false;
    while (instr_ < script_.size()) {
        const Record& r = script_[instr_];
        const bool due = scriptTime_ >= r.time;
        bool stop = false;
        switch (r.op) {
            case 0:
                if (r.time == -1) {  // static key: pins the camera, nothing after it runs
                    camera_ = p0_ = vec(r);
                    stopTime = -1;
                    stop = true;
                    break;
                }
                if (!due) {
                    stopTime = r.time;
                    stop = true;
                    break;
                }
                camera_ = p0_ = vec(r);
                t0_ = r.time;
                ++instr_;
                {
                    std::size_t next = instr_;
                    while (next < script_.size() && script_[next].op != 0) ++next;
                    if (next < script_.size()) {
                        t1_ = script_[next].time;
                        p1_ = vec(script_[next]);
                        stopTime = t1_;
                    } else {  // no later key: hold (the original reads past the data)
                        t1_ = t0_;
                        p1_ = p0_;
                        stopTime = -1;
                    }
                }
                stop = true;
                break;
            case 1:
                if (!due) {
                    stopTime = r.time;
                    stop = true;
                    break;
                }
                fogColor_ = r.a[0];
                fogStartZ_ = std::bit_cast<float>(r.a[1]);
                fogEndZ_ = std::bit_cast<float>(r.a[2]);
                fogToColor_ = fogColor_;
                fogToStart_ = fogStartZ_;
                fogToEnd_ = fogEndZ_;
                ++instr_;
                break;
            case 2:
                if (!due) {
                    stopTime = r.time;
                    stop = true;
                    break;
                }
                aimFrom_ = aimTo_;
                aimTo_ = vec(r);
                ++instr_;
                break;
            case 3:
                if (!due) {
                    stopTime = r.time;
                    stop = true;
                    break;
                }
                aimDuration_ = static_cast<std::int32_t>(r.a[0]);
                aimTimer_ = 0;
                ++instr_;
                break;
            case 4:
                if (!due) {
                    stopTime = r.time;
                    stop = true;
                    break;
                }
                fogFromColor_ = fogColor_;
                fogFromStart_ = fogStartZ_;
                fogFromEnd_ = fogEndZ_;
                fogDuration_ = std::max<std::int32_t>(static_cast<std::int32_t>(r.a[0]), 0);
                fogTimer_ = 0;
                ++instr_;
                break;
            case 5:  // pause until ECL unpauses (its time is ignored)
                if (unpause_) {
                    unpause_ = false;
                    ++instr_;
                    break;
                }
                stopTime = r.time;
                stopPause = true;
                stop = true;
                break;
            default:  // the end
                stopTime = r.time;
                stop = true;
                break;
        }
        if (stop) break;
    }

    // Camera position along the current segment (unclamped).
    if (stopTime != -1 && t1_ != t0_) {
        const float ratio = (static_cast<float>(scriptTime_) - static_cast<float>(t0_)) / static_cast<float>(t1_ - t0_);
        camera_ = p0_ + (p1_ - p0_) * ratio;
    }
    // Aim.
    if (aimDuration_ != 0) {
        if (aimTimer_ < aimDuration_) ++aimTimer_;
        else aimTimer_ = aimDuration_;
        const float ratio = static_cast<float>(aimTimer_) / static_cast<float>(aimDuration_);
        aim_ = aimFrom_ + (aimTo_ - aimFrom_) * ratio;
    }
    // Fog.
    if (fogDuration_ != 0) {
        ++fogTimer_;
        const float ratio = std::min(static_cast<float>(fogTimer_) / static_cast<float>(fogDuration_), 1.0f);
        fogColor_ = lerpColor(fogFromColor_, fogToColor_, ratio);
        fogStartZ_ = fogFromStart_ + (fogToStart_ - fogFromStart_) * ratio;
        fogEndZ_ = fogFromEnd_ + (fogToEnd_ - fogFromEnd_) * ratio;
        if (fogTimer_ >= fogDuration_) fogDuration_ = 0;
    }
    if (!stopPause) ++scriptTime_;

    // Quad animation: an object stops animating once all its quads' scripts have ended.
    for (Object& ob : objects_) {
        if (!ob.animating) continue;
        int running = 0;
        for (const Quad& q : ob.quads) {
            AnmVm& vm = vms_[static_cast<std::size_t>(q.vm)];
            if (q.type == 0 || q.type == 1) world.anm->executeScript(vm);
            running += vm.currentInstruction != nullptr;
        }
        if (running == 0) ob.animating = false;
    }
    // Spell card: 60 frames of fade, then only the spell background.
    if (spell_ != Spell::Off) {
        if (spellCounter_ == 60 && spell_ == Spell::Running) spell_ = Spell::FullyOn;
        ++spellCounter_;
        world.anm->executeScript(spellVm_);
    }
}

// ---- drawing --------------------------------------------------------------------------

void StageBackground::drawQuad(World& world, SpriteSink& sink, AnmVm& vm, const Vec3f& pos, const Quad& q) {
    const AnmLoadedSprite* s = world.anm->sprite(vm);
    if (!s) return;
    vm.pos = pos;
    if (q.width != 0.0f) vm.scaleX = q.width / s->widthPx;
    if (q.height != 0.0f) vm.scaleY = q.height / s->heightPx;
    const Camera& c = camera3D_;
    Draw3DState state;
    state.depthWrite = !vm.zWriteDisable;
    state.fogStart = fogStartZ_;
    state.fogEnd = fogEndZ_;
    state.fogColor = argbToRgbaBytes(fogColor_ | 0xff000000u);
    state.clipX = c.x;
    state.clipY = c.y;
    state.clipW = c.w;
    state.clipH = c.h;
    const std::uint32_t color = argbToRgbaBytes(vm.color);
    const Blend blend = makeBlend(vm.additive, vm.colorAdd);
    const float u0 = s->u0 + vm.uvScrollX, v0 = s->v0 + vm.uvScrollY, u1 = s->u1 + vm.uvScrollX, v1 = s->v1 + vm.uvScrollY;
    Vertex3D v[4];
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};

    if (vm.autoRotate == 2) {  // camera-facing: a screen-aligned sprite scaled by the local perspective
        float sx0, sy0, ze0, sx1, sy1, ze1;
        project(pos, sx0, sy0, ze0);
        const float w0 = q.width != 0.0f ? q.width : s->widthPx;
        project({pos.x + w0 * vm.scaleX, pos.y, pos.z}, sx1, sy1, ze1);
        const float k = (sx1 - sx0) / w0;
        const float zn = kDepthScale * (1.0f - kNear / ze0);
        vm.scaleX = vm.scaleY = k;
        vm.pos = {sx0, sy0, 0.5f + 0.5f * zn};
        if (!vm.visible || !vm.visibleOverride || vm.color == 0 || ze0 <= 0.0f) return;
        const float w = s->widthPx * k, h = s->heightPx * k;
        const float x0 = (static_cast<std::uint8_t>(vm.anchor) & 1) ? sx0 : sx0 - w / 2.0f;
        const float y0 = (static_cast<std::uint8_t>(vm.anchor) & 2) ? sy0 : sy0 - h / 2.0f;
        const float xs[4] = {x0, x0 + w, x0, x0 + w}, ys[4] = {y0, y0, y0 + h, y0 + h};
        for (int i = 0; i < 4; ++i) {
            v[i].x = xs[i] / kTargetW * 2.0f - 1.0f;
            v[i].y = 1.0f - ys[i] / kTargetH * 2.0f;
            v[i].z = 2.0f * zn - 1.0f;
            v[i].w = 1.0f;
            v[i].u = uv[i][0];
            v[i].v = uv[i][1];
            v[i].color = color;
            v[i].eyeZ = 0.0f;  // not fogged
        }
        sink.drawQuad3D(s->texture, blend, v, state);
        return;
    }

    if (!vm.visible || !vm.visibleOverride || vm.color == 0) return;
    const float w3 = 256.0f * s->widthPx / s->textureWidth * vm.scaleX;
    const float h3 = 256.0f * s->heightPx / s->textureHeight * vm.scaleY;
    const Vec3f local[4] = {{-w3 / 2, h3 / 2, 0}, {w3 / 2, h3 / 2, 0}, {-w3 / 2, -h3 / 2, 0}, {w3 / 2, -h3 / 2, 0}};
    Vec3f origin{pos.x, -pos.y, pos.z};
    if (static_cast<std::uint8_t>(vm.anchor) & 1) origin.x += std::fabs(s->widthPx * vm.scaleX / 2.0f);
    if (static_cast<std::uint8_t>(vm.anchor) & 2) origin.y -= std::fabs(s->heightPx * vm.scaleY / 2.0f);
    // Clip space for the whole 640x480 target, so the GPU interpolates perspective-correctly.
    const float ax = 2.0f * c.x / kTargetW + c.w / kTargetW - 1.0f, bx = c.h / kTargetW * c.cot;
    const float ay = 1.0f - 2.0f * c.y / kTargetH - c.h / kTargetH, by = c.h / kTargetH * c.cot;
    for (int i = 0; i < 4; ++i) {
        const Vec3f p = origin + rotate(local[i], vm.rotation);
        const Vec3f rel = p - c.eye;
        const float xe = dot(c.right, rel), ye = dot(c.up, rel), ze = dot(c.forward, rel);
        v[i].x = ax * ze + bx * xe;
        v[i].y = ay * ze + by * ye;
        v[i].z = 2.0f * kDepthScale * (ze - kNear) - ze;
        v[i].w = ze;
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
        v[i].color = color;
        v[i].eyeZ = std::max(ze, 0.001f);
    }
    sink.drawQuad3D(s->texture, blend, v, state);
}

void StageBackground::drawLayer(World& world, SpriteSink& sink, int layer, const Vec3f& camera) {
    for (const Instance& inst : instances_) {
        const Object& ob = objects_[static_cast<std::size_t>(inst.object)];
        if (ob.layer != layer || !instanceVisible(inst, camera)) continue;
        for (const Quad& q : ob.quads) {
            if (q.type != 0) continue;
            drawQuad(world, sink, vms_[static_cast<std::size_t>(q.vm)], q.pos + inst.pos - camera, q);
        }
    }
}

void StageBackground::draw(World& world, SpriteSink& sink, const SpriteRegion& white, float alpha, bool loadingHalted) {
    if (!loaded_) return;
    const bool layers = spell_ != Spell::FullyOn && !loadingHalted;
    const Vec3f camera = lerp(prevCamera_, camera_, alpha);
    if (layers) {
        for (int layer = 0; layer < 4; ++layer) drawLayer(world, sink, layer, camera);
        if (spell_ == Spell::Running) {  // the fade to black over 61 frames
            const auto a = static_cast<std::uint8_t>(std::min(spellCounter_, 60) * 255 / 60);
            sink.fillRect(white, kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight, rgba(0, 0, 0, a));
        }
    }
    if (spell_ != Spell::Off) {
        if (spellCounter_ == 0 && !spellRestarted_) {  // started while drawing, as in the original
            world.anm->setAndExecuteScript(spellVm_, kScriptSpellBackground);
            spellRestarted_ = true;
        }
        world.anm->drawScreen(sink, spellVm_);
    }
}

}  // namespace th06
