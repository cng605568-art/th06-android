#include "th07/Background.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace touhou::th07 {

// ---- vector and matrix helpers (D3DX conventions: row vectors, left-handed) ------------

Vec3f normalize(const Vec3f& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len == 0.0f) return {0, 0, 0};
    const float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

Vec3f cross(const Vec3f& a, const Vec3f& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Mat4 lookAtLH(const Vec3f& eye, const Vec3f& at, const Vec3f& up) {
    const Vec3f z = normalize(at - eye);
    const Vec3f x = normalize(cross(up, z));
    const Vec3f y = cross(z, x);
    return {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1};
}

Mat4 perspectiveLH(float fovY, float aspect, float zn, float zf) {
    const float yScale = 1.0f / std::tan(fovY / 2.0f);
    const float xScale = yScale / aspect;
    return {xScale, 0, 0, 0, 0, yScale, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[static_cast<std::size_t>(i * 4 + k)] * b[static_cast<std::size_t>(k * 4 + j)];
            r[static_cast<std::size_t>(i * 4 + j)] = s;
        }
    }
    return r;
}

namespace {

struct Vec4f {
    float x, y, z, w;
};

Vec4f transform(const Vec3f& p, const Mat4& m) {
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14], p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15]};
}

Mat4 rotationX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}
Mat4 rotationY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}
Mat4 rotationZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

std::int16_t rdI16(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int16_t v;
    std::memcpy(&v, b.data() + at, 2);
    return v;
}
std::int32_t rdI32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
float rdF32(const std::vector<std::uint8_t>& b, std::size_t at) {
    float v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
Vec3f rdVec(const std::vector<std::uint8_t>& b, std::size_t at) { return {rdF32(b, at), rdF32(b, at + 4), rdF32(b, at + 8)}; }

float cameraEase(int mode, float t) {
    switch (mode) {
        case 1: {
            const float u = 1.0f - t;
            return 1.0f - u * u;
        }
        case 2: {
            const float u = 1.0f - t;
            return 1.0f - u * u * u;
        }
        case 3: {
            const float u = 1.0f - t;
            return 1.0f - u * u * u * u;
        }
        case 4: return t * t;
        case 5: return t * t * t;
        case 6: return t * t * t * t;
        default: return t;
    }
}

constexpr float kTargetW = 640.0f, kTargetH = 480.0f;

}  // namespace

float Background::Record::f(int k) const {
    float v;
    std::memcpy(&v, &a[static_cast<std::size_t>(k)], 4);
    return v;
}

// ---- loading ----------------------------------------------------------------------------

bool Background::load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, ZunRng& rng, int stage, std::string* error) {
    release(anm);
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    stage_ = stage;
    std::string err;
    if (!anm.load(vfs, textures, std::format("data/stg{}bg.anm", stage), 5, kScriptBase, &err)) return fail(err);
    if (stage == 4) {
        for (int k = 0; k < 4; ++k) {
            if (!anm.load(vfs, textures, std::format("data/stg4bg{}.anm", k + 2), 6 + k, 0x310 + 0x10 * k, &err)) return fail(err);
        }
    }
    const auto file = vfs.read(std::format("data/stage{}.std", stage));
    if (!file) return fail(std::format("data/stage{}.std is not in the archive", stage));
    const std::vector<std::uint8_t>& b = *file;
    if (b.size() < 0x490) return fail("stage file too short");
    const int objectCount = rdI16(b, 0);
    const int quadCount = rdI16(b, 2);
    const auto instancesAt = static_cast<std::size_t>(rdI32(b, 4));
    const auto scriptAt = static_cast<std::size_t>(rdI32(b, 8));
    for (int k = 0; k < 4; ++k) {
        const std::size_t at = 0x290 + 0x80 * static_cast<std::size_t>(k);
        songs_[static_cast<std::size_t>(k)].clear();
        for (std::size_t i = at; i < at + 0x80 && b[i]; ++i) songs_[static_cast<std::size_t>(k)].push_back(static_cast<char>(b[i]));
    }
    if (objectCount < 0 || 0x490 + 4ull * static_cast<unsigned>(objectCount) > b.size()) return fail("bad object table");

    int quads = 0;
    for (int i = 0; i < objectCount; ++i) {
        const auto at = static_cast<std::size_t>(rdI32(b, 0x490 + 4 * static_cast<std::size_t>(i)));
        if (at + 0x1C > b.size()) return fail("object outside the file");
        Object o;
        o.layer = static_cast<std::int8_t>(b[at + 2]);
        o.origin = rdVec(b, at + 4);
        o.size = rdVec(b, at + 0x10);
        for (std::size_t q = at + 0x1C; q + 0x1C <= b.size();) {
            const std::int16_t type = rdI16(b, q);
            if (type < 0) break;
            const std::int16_t size = rdI16(b, q + 2);
            Quad quad;
            quad.type = type;
            quad.script = rdI16(b, q + 4);
            quad.pos = rdVec(b, q + 8);
            quad.width = rdF32(b, q + 0x14);
            quad.height = rdF32(b, q + 0x18);
            o.quads.push_back(quad);
            ++quads;
            if (size <= 0) return fail("bad quad record");
            q += static_cast<std::size_t>(size);
        }
        objects_.push_back(std::move(o));
    }
    if (quads > quadCount) log::warn("stage{}.std: {} quads, the header says {}", stage, quads, quadCount);
    for (std::size_t at = instancesAt; at + 16 <= b.size(); at += 16) {
        const std::int16_t obj = rdI16(b, at);
        if (obj < 0) break;
        if (obj >= objectCount) return fail("instance of a missing object");
        instances_.push_back({obj, rdVec(b, at + 4)});
    }
    for (std::size_t at = scriptAt; at + 20 <= b.size(); at += 20) {
        Record r{rdI32(b, at), rdI16(b, at + 4), {}};
        for (int k = 0; k < 3; ++k) std::memcpy(&r.a[static_cast<std::size_t>(k)], b.data() + at + 8 + 4 * static_cast<std::size_t>(k), 4);
        script_.push_back(r);
        if (r.frame == -1) break;
    }
    if (script_.empty() || script_.back().frame != -1) script_.push_back(Record{-1, -1, {}});

    // One VM per quad, every script started now (its first tick runs at once: RNG).
    vms_.assign(static_cast<std::size_t>(quads), AnmVm{});
    int next = 0;
    for (Object& o : objects_) {
        o.flags = 1;
        for (Quad& q : o.quads) {
            q.vm = next;
            vms_[static_cast<std::size_t>(next++)].startFresh(anm, rng, kScriptBase + q.script);
        }
    }
    fog_ = fogTarget_ = fogOrigin_ = Fog{};
    cam_ = Camera{};
    for (int i = 0; i < 3; ++i) {
        const Vec3f v = i == 0 ? cam_.pos : i == 1 ? cam_.facing : cam_.up;
        channels_[static_cast<std::size_t>(i)] = Channel{};
        channels_[static_cast<std::size_t>(i)].start = channels_[static_cast<std::size_t>(i)].target = v;
    }
    fovStart_ = fovTarget_ = cam_.fov;
    fovDuration_ = 0;
    resumeLabel_ = 0;
    spellState_ = SpellState::Inactive;
    spellCounter_ = 0;
    spellBase_ = 0;
    spellCount_ = (stage >= 4 && stage <= 6) || stage == 8 ? 2 : 1;
    spellVms_ = {};
    log::info("Stage {} background: {} objects, {} quads, {} instances, {} script records", stage, objects_.size(), quads,
              instances_.size(), script_.size());
    return true;
}

void Background::release(AnmLibrary& anm) {
    if (stage_ != 0) {
        anm.release(5);
        if (stage_ == 4) {
            for (int k = 0; k < 4; ++k) anm.release(6 + k);
        }
    }
    objects_.clear();
    instances_.clear();
    script_.clear();
    vms_.clear();
    offset_ = {};
    time_.reset();
    index_ = 0;
    resumeLabel_ = 0;
    teleported_ = false;
    clearColor_ = 0;
    fogDuration_ = 0;
    backdropA_ = AnmVm{};
    backdropB_ = AnmVm{};
    spellState_ = SpellState::Inactive;
    spellCounter_ = 0;
    spellVms_ = {};
    stage_ = 0;
}

void Background::spellStart(AnmLibrary& anm, ZunRng& rng) {
    spellState_ = SpellState::Starting;
    spellCounter_ = 0;
    for (int i = 0; i < spellCount_; ++i) spellVms_[static_cast<std::size_t>(i)].startFresh(anm, rng, kSpellScripts + spellBase_ + i);
}

void Background::spellInterrupt(int vm, int label) {
    if (vm >= 0 && vm < static_cast<int>(spellVms_.size())) spellVms_[static_cast<std::size_t>(vm)].interrupt(label);
}

void Background::setSpellBackgrounds(int base, int count) {
    spellBase_ = base;
    spellCount_ = std::clamp(count, 0, static_cast<int>(spellVms_.size()));
}

// ---- the logic pass ---------------------------------------------------------------------

void Background::update(AnmLibrary& anm, ZunRng& rng) {
    if (!loaded()) return;
    // 1. Resume at the label the ECL asked for.
    if (resumeLabel_ != 0) {
        index_ = 0;
        for (std::size_t k = 0; k < script_.size() && script_[k].frame != -1; ++k) {
            if (script_[k].op == 31 && script_[k].i(0) == resumeLabel_) {
                index_ = k + 1;
                time_.assign(script_[k].frame);
                resumeLabel_ = 0;
                break;
            }
        }
    }
    // 2. The script.
    std::int16_t stopOp = -1;
    while (true) {
        const Record& r = script_[std::min(index_, script_.size() - 1)];
        if (index_ >= script_.size() || r.frame == -1 || time_.current < r.frame) {
            stopOp = r.op;
            break;
        }
        if (r.op == 3) {  // halt
            if (resumeLabel_ != 0) {
                resumeLabel_ = 0;
                ++index_;
                continue;
            }
            stopOp = 3;
            break;
        }
        if (r.op == 4) {  // jump
            index_ = static_cast<std::size_t>(std::max(0, r.i(0)));
            time_.assign(r.i(1));
            channels_[0].duration = 0;
            teleported_ = true;
            continue;
        }
        switch (r.op) {
            case 29:
            case 30: {
                const std::int32_t n = r.i(0);
                if (n >= 0) (r.op == 29 ? backdropA_ : backdropB_).startFresh(anm, rng, kScriptBase + n);
                else backdropA_.activeSprite = -1;  // both opcodes hide backdrop A, as in the original
                break;
            }
            default:
                execute(r);
                break;
        }
        ++index_;
    }
    // 3. Camera interpolation: position, facing, up, then the field of view.
    for (int c = 0; c < 3; ++c) {
        Channel& ch = channels_[static_cast<std::size_t>(c)];
        if (ch.duration == 0) continue;
        float t;
        if (ch.counter.current < ch.duration) {
            ch.counter.tick();
            t = ch.counter.asFloat() / static_cast<float>(ch.duration);
        } else {
            ch.counter.assign(ch.duration);
            t = 1.0f;
            ch.duration = 0;
        }
        Vec3f v;
        if (ch.ease == 7) {
            const float h00 = (t - 1.0f) * (t - 1.0f) * (2.0f * t + 1.0f);
            const float h01 = t * t * (3.0f - 2.0f * t);
            const float h10 = (1.0f - t) * (1.0f - t) * t;
            const float h11 = (t - 1.0f) * t * t;
            auto herm = [&](float p0, float p1, float m0, float m1) { return h00 * p0 + h01 * p1 + h10 * m0 + h11 * m1; };
            v = {herm(ch.start.x, ch.target.x, ch.tangent0.x, ch.tangent1.x), herm(ch.start.y, ch.target.y, ch.tangent0.y, ch.tangent1.y),
                 herm(ch.start.z, ch.target.z, ch.tangent0.z, ch.tangent1.z)};
        } else {
            t = cameraEase(ch.ease, t);
            v = {t * (ch.target.x - ch.start.x) + ch.start.x, t * (ch.target.y - ch.start.y) + ch.start.y,
                 t * (ch.target.z - ch.start.z) + ch.start.z};
        }
        (c == 0 ? cam_.pos : c == 1 ? cam_.facing : cam_.up) = v;
    }
    if (fovDuration_ != 0) {
        float t;
        if (fovCounter_.current < fovDuration_) {
            fovCounter_.tick();
            t = fovCounter_.asFloat() / static_cast<float>(fovDuration_);
        } else {
            fovCounter_.assign(fovDuration_);
            t = 1.0f;
            fovDuration_ = 0;
        }
        t = cameraEase(fovEase_, t);
        cam_.fov = (fovTarget_ - fovStart_) * t + fovStart_;
    }
    // 4. The normalised facing (it feeds the weather particles).
    facingN_ = normalize(cam_.facing);
    // 5. Fog fade.
    if (fogDuration_ != 0) {
        fogCounter_.tick();
        float t = fogCounter_.asFloat() / static_cast<float>(fogDuration_);
        if (t > 1.0f) t = 1.0f;
        std::uint32_t c = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const int o = static_cast<int>((fogOrigin_.color >> shift) & 0xFF), g = static_cast<int>((fogTarget_.color >> shift) & 0xFF);
            const auto v = static_cast<std::int32_t>(static_cast<float>(g - o) * t + static_cast<float>(o));
            c |= (static_cast<std::uint32_t>(v) & 0xFFu) << shift;
        }
        fog_.color = c;
        fog_.nearZ = (fogTarget_.nearZ - fogOrigin_.nearZ) * t + fogOrigin_.nearZ;
        fog_.farZ = (fogTarget_.farZ - fogOrigin_.farZ) * t + fogOrigin_.farZ;
        if (fogCounter_.current >= fogDuration_) fogDuration_ = 0;
    }
    // 6. Script time freezes while the next pending record is a halt.
    if (stopOp != 3) time_.tick();
    // 7. Objects: their quads' VMs (types 0 and 1); an object whose scripts all ended stops.
    for (Object& o : objects_) {
        if (!(o.flags & 1)) continue;
        int running = 0;
        for (const Quad& q : o.quads) {
            AnmVm& vm = vms_[static_cast<std::size_t>(q.vm)];
            if (q.type == 0 || q.type == 1) vm.tick(anm, rng);
            if (!vm.finished()) ++running;
        }
        if (running == 0) o.flags &= static_cast<std::uint8_t>(~1u);
    }
    // 8. Spell-card backgrounds: starting becomes active on the 60th frame.
    if (spellState_ != SpellState::Inactive) {
        if (spellCounter_ == 60 && spellState_ == SpellState::Starting) spellState_ = SpellState::Active;
        ++spellCounter_;
        for (int i = 0; i < spellCount_; ++i) spellVms_[static_cast<std::size_t>(i)].tick(anm, rng);
    }
    // 9. Backdrops (a sprite index of 0 never animates, as in the original).
    if (backdropA_.activeSprite > 0) backdropA_.tick(anm, rng);
    if (backdropB_.activeSprite > 0) backdropB_.tick(anm, rng);
    ++frames_;
}

void Background::execute(const Record& r) {
    const Vec3f xyz{r.f(0), r.f(1), r.f(2)};
    auto startChannel = [&](Channel& ch, std::int32_t duration, std::int32_t ease) {
        ch.duration = duration;
        ch.counter.reset();
        ch.ease = ease;
    };
    auto setPoint = [&](Channel& ch, Vec3f& value) {
        ch.start = ch.target;
        ch.target = xyz;
        if (ch.duration == 0) value = xyz;
    };
    switch (r.op) {
        case 0: offset_ = xyz; break;
        case 1:
            fog_ = fogTarget_ = Fog{r.a[0], r.f(1), r.f(2)};
            break;
        case 2:
            fogOrigin_ = fog_;
            fogDuration_ = r.i(0);
            fogCounter_.reset();
            break;
        case 5:
            if (teleported_) {
                if (onWeatherShift) onWeatherShift(xyz - channels_[0].target);
                teleported_ = false;
            }
            setPoint(channels_[0], cam_.pos);
            break;
        case 6: startChannel(channels_[0], r.i(0), r.i(1)); break;
        case 7: setPoint(channels_[1], cam_.facing); break;
        case 8: startChannel(channels_[1], r.i(0), r.i(1)); break;
        case 9: setPoint(channels_[2], cam_.up); break;
        case 10: startChannel(channels_[2], r.i(0), r.i(1)); break;
        case 11:
            fovStart_ = fovTarget_;
            fovTarget_ = r.f(0);
            if (fovDuration_ == 0) cam_.fov = r.f(0);
            break;
        case 12:
            fovDuration_ = r.i(0);
            fovCounter_.reset();
            fovEase_ = r.i(1);
            break;
        case 13: clearColor_ = r.a[0]; break;
        default:
            if (r.op >= 14 && r.op <= 28) {
                Channel& ch = channels_[static_cast<std::size_t>((r.op - 14) / 5)];
                switch ((r.op - 14) % 5) {
                    case 0: ch.start = xyz; break;
                    case 1: ch.target = xyz; break;
                    case 2: ch.tangent0 = xyz; break;
                    case 3: ch.tangent1 = xyz; break;
                    case 4: startChannel(ch, r.i(0), 7); break;
                }
            }
            break;  // 31 (label) and unknown opcodes: no effect
    }
}

// ---- drawing ----------------------------------------------------------------------------

void Background::buildStageCamera() {
    const Vec3f at{cam_.pos.x + cam_.facing.x, cam_.pos.y + cam_.facing.y, cam_.pos.z + cam_.facing.z};
    view_ = lookAtLH(cam_.pos, at, cam_.up);
    proj_ = perspectiveLH(cam_.fov, viewport.w / viewport.h, 30.0f, 1800.0f);
    currentCamera_ = 1;
}

void Background::buildPlayfieldCamera() {
    const float w = viewport.w, h = viewport.h;
    view_ = lookAtLH({w / 2.0f, h / 2.0f, (h / 2.0f) / std::tan(kZunPi / 20.0f)}, {w / 2.0f, h / 2.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    proj_ = perspectiveLH(kZunPi / 10.0f, w / h, 1.0f, 10000.0f);
    currentCamera_ = 0;
}

void Background::drawHigh(SpriteSink& sink, AnmDrawState& state) {
    currentCamera_ = -1;
    uvCacheSprite_ = nullptr;
    if (!loaded()) return;
    const bool stageVisible = spellState_ != SpellState::Active;
    if (stageVisible && backdropA_.activeSprite > 0) drawVm(sink, backdropA_, state);
    if (stageVisible && backdropB_.activeSprite > 0) drawVm(sink, backdropB_, state);
    // The clear colour (if set) erases the backdrops; otherwise only depth is cleared.
    const std::uint32_t clear = argbToRgbaBytes(clearColor_);
    sink.clearRect(viewport.x, viewport.y, viewport.w, viewport.h, clear, clearColor_ != 0);
    if (!stageVisible) return;
    drawLayer(sink, state, 0);
    drawLayer(sink, state, 1);
}

void Background::drawLow(SpriteSink& sink, AnmDrawState& state) {
    if (!loaded()) return;
    if (spellState_ != SpellState::Active) {
        drawLayer(sink, state, 2);
        drawLayer(sink, state, 3);
        if (drawWeather) drawWeather(sink, state);
        if (spellState_ == SpellState::Starting && white) {
            const std::int32_t a = std::clamp(spellCounter_ * 255 / 60, 0, 255);
            sink.fillRect(*white, viewport.x, viewport.y, viewport.w, viewport.h, rgba(0, 0, 0, static_cast<std::uint8_t>(a)));
        }
    }
    if (spellState_ != SpellState::Inactive) {
        for (int i = 0; i < spellCount_; ++i) drawVm(sink, spellVms_[static_cast<std::size_t>(i)], state);
    }
    buildPlayfieldCamera();
}

void Background::drawLayer(SpriteSink& sink, AnmDrawState& state, int layer) {
    buildStageCamera();
    for (const Instance& inst : instances_) {
        const Object& o = objects_[static_cast<std::size_t>(inst.object)];
        if (o.layer != layer) continue;
        const Vec3f c{o.origin.x + inst.pos.x - offset_.x + o.size.x / 2.0f, o.origin.y + inst.pos.y - offset_.y + o.size.y / 2.0f,
                      o.origin.z + inst.pos.z - offset_.z + o.size.z / 2.0f};
        const Vec3f d = c - cam_.pos;
        if (dot(d, d) > 1690000.0f) continue;
        const float s = dot(d, facingN_);
        if (s > o.size.length() / 2.0f + 880.0f || s < 60.0f) continue;
        for (const Quad& q : o.quads) {
            if (q.type != 0) continue;
            AnmVm& vm = vms_[static_cast<std::size_t>(q.vm)];
            vm.position = Vec3f{vm.offset.x + q.pos.x + inst.pos.x - offset_.x, vm.offset.y + q.pos.y + inst.pos.y - offset_.y,
                                vm.offset.z + q.pos.z + inst.pos.z - offset_.z};
            if (!vm.sprite) continue;
            if (q.width != 0.0f) vm.scale.x = q.width / vm.sprite->pixelWidth;
            if (q.height != 0.0f) vm.scale.y = q.height / vm.sprite->pixelHeight;
            if (vm.autoRotate == 2) drawFacingQuad(sink, vm, state, q.width != 0.0f ? q.width : vm.sprite->pixelWidth);
            else draw3DQuad(sink, vm, state, vm.has(AnmVm::CameraMode) ? 1 : 0);
        }
    }
}

namespace {

// Direct3D clip space of the playfield viewport -> GL clip space of the whole 640x480
// target (pixel centres shifted by half a pixel, Direct3D depth 0..1 -> GL -1..1).
Vertex3D toTarget(const Vec4f& c, const Viewport& vp) {
    Vertex3D v;
    v.x = c.x * (vp.w / kTargetW) + c.w * ((2.0f * vp.x + vp.w + 1.0f) / kTargetW - 1.0f);
    v.y = c.y * (vp.h / kTargetH) + c.w * (1.0f - (2.0f * vp.y + vp.h + 1.0f) / kTargetH);
    v.z = 2.0f * c.z - c.w;
    v.w = c.w;
    return v;
}

}  // namespace

void Background::draw3DQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, int cameraMode) {
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    if (!vm.has(AnmVm::SkipTransform) && (vm.has(AnmVm::ScaleDirty) || vm.has(AnmVm::RotationDirty))) {
        Mat4 w = vm.spriteMatrix;
        w[0] *= vm.scale.x;
        w[5] *= vm.scale.y;
        vm.set(AnmVm::ScaleDirty, false);
        if (vm.rotation.x != 0.0f) w = multiply(w, rotationX(vm.rotation.x));
        if (vm.rotation.y != 0.0f) w = multiply(w, rotationY(vm.rotation.y));
        if (vm.rotation.z != 0.0f) w = multiply(w, rotationZ(vm.rotation.z));
        vm.set(AnmVm::RotationDirty, false);
        vm.worldMatrix = w;
    }
    Mat4 world = vm.worldMatrix;
    float tx = vm.position.x, ty = vm.position.y;
    if (vm.anchor() & 1u) tx += std::fabs(vm.sprite->pixelWidth * vm.scale.x / 2.0f);
    if (vm.anchor() & 2u) ty += std::fabs(vm.sprite->pixelHeight * vm.scale.y / 2.0f);
    world[12] = tx + state.offsetX;
    world[13] = ty + state.offsetY;
    world[14] = vm.position.z;
    if (cameraMode != currentCamera_) {
        if (cameraMode == 1) buildStageCamera();
        else buildPlayfieldCamera();
    }
    // The texture transform only follows a change of sprite (consecutive quads sharing a
    // sprite share the first one's scroll), as in the original.
    if (vm.sprite != uvCacheSprite_) {
        uvCacheSprite_ = vm.sprite;
        uvCacheU_ = vm.sprite->uvStartX + vm.uvScroll.x;
        uvCacheV_ = vm.sprite->uvStartY + vm.uvScroll.y;
    }
    const float su = vm.sprite->pixelWidth / vm.sprite->textureWidth * vm.sprite->cols;
    const float sv = vm.sprite->pixelHeight / vm.sprite->textureHeight * vm.sprite->rows;
    const Mat4 m = multiply(multiply(world, view_), proj_);
    const Vec3f corners[4] = {{-128, -128, 0}, {128, -128, 0}, {-128, 128, 0}, {128, 128, 0}};
    const float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const std::uint32_t color = argbToRgbaBytes(effectiveColor(vm, state));
    Vertex3D v[4];
    for (int i = 0; i < 4; ++i) {
        const Vec4f c = transform(corners[i], m);
        v[i] = toTarget(c, viewport);
        v[i].u = uvCacheU_ + uv[i][0] * su;
        v[i].v = uvCacheV_ + uv[i][1] * sv;
        v[i].color = color;
        v[i].eyeZ = cameraMode == 1 ? std::max(c.w, 0.001f) : 0.0f;
    }
    Draw3DState ds;
    ds.depthWrite = !vm.has(AnmVm::ZWriteDisable);
    ds.fogStart = fog_.nearZ;
    ds.fogEnd = fog_.farZ;
    std::uint32_t fogColor = fog_.color | 0xFF000000u;
    if (state.multiply) {
        AnmVm tmp;
        tmp.color = fogColor;
        AnmDrawState only = state;
        fogColor = (effectiveColor(tmp, only) & 0x00FFFFFFu) | 0xFF000000u;
    }
    ds.fogColor = argbToRgbaBytes(fogColor);
    ds.clipX = viewport.x;
    ds.clipY = viewport.y;
    ds.clipW = viewport.w;
    ds.clipH = viewport.h;
    sink.drawQuad3D(vm.sprite->texture, vm.has(AnmVm::Additive) ? Blend::Additive : Blend::Alpha, v, ds);
}

void Background::drawFacingQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, float width) {
    if (currentCamera_ != 1) buildStageCamera();
    const Mat4 vp = multiply(view_, proj_);
    auto project = [&](const Vec3f& p) {
        const Vec4f c = transform(p, vp);
        return Vec3f{viewport.x + (1.0f + c.x / c.w) * viewport.w / 2.0f, viewport.y + (1.0f - c.y / c.w) * viewport.h / 2.0f, c.z / c.w};
    };
    const Vec3f p0 = project(vm.position);
    const Vec3f axis = normalize({view_[0], view_[1], view_[2]});  // the world X axis in camera space, as the original takes it
    const Vec3f p1 = project(vm.position + axis * (width * vm.scale.x));
    const float k = (p1 - p0).length() / width;
    vm.scale.x = vm.scale.y = k;
    // Fog by hand on the colour.
    const std::uint32_t saved = vm.color;
    const float dist = (vm.position - cam_.pos).length();
    if (dist > fog_.nearZ) {
        const float f = (fog_.nearZ - dist) / (fog_.nearZ - fog_.farZ);
        if (f >= 1.0f) return;
        std::uint32_t c = vm.color;
        std::uint32_t out = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            const int cc = static_cast<int>((c >> shift) & 0xFF), fc = static_cast<int>((fog_.color >> shift) & 0xFF);
            const auto t = static_cast<std::int32_t>(static_cast<float>(cc - fc) * f);
            out |= (static_cast<std::uint32_t>(cc - (t & 0xFF)) & 0xFFu) << shift;
        }
        out |= static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<float>(c >> 24) * (1.0f - f)) & 0xFF) << 24;
        vm.color = out;
    }
    vm.position = p0;
    if (p0.z >= 0.0f && p0.z <= 1.0f && vm.has(AnmVm::Visible) && vm.has(AnmVm::Active) && (vm.color >> 24) != 0) {
        const float w = vm.sprite->pixelWidth * vm.scale.x, h = vm.sprite->pixelHeight * vm.scale.y;
        const float x0 = (vm.anchor() & 1u) ? p0.x : p0.x - w / 2.0f;
        const float y0 = (vm.anchor() & 2u) ? p0.y : p0.y - h / 2.0f;
        const float xs[4] = {x0, x0 + w, x0, x0 + w}, ys[4] = {y0, y0, y0 + h, y0 + h};
        const float us[4] = {vm.sprite->uvStartX, vm.sprite->uvEndX, vm.sprite->uvStartX, vm.sprite->uvEndX};
        const float vs[4] = {vm.sprite->uvStartY, vm.sprite->uvStartY, vm.sprite->uvEndY, vm.sprite->uvEndY};
        const std::uint32_t color = argbToRgbaBytes(effectiveColor(vm, state));
        Vertex3D v[4];
        for (int i = 0; i < 4; ++i) {
            v[i].x = (xs[i] + 0.5f) / kTargetW * 2.0f - 1.0f;
            v[i].y = 1.0f - (ys[i] + 0.5f) / kTargetH * 2.0f;
            v[i].z = 2.0f * p0.z - 1.0f;
            v[i].w = 1.0f;
            v[i].u = us[i] + vm.uvScroll.x;
            v[i].v = vs[i] + vm.uvScroll.y;
            v[i].color = color;
            v[i].eyeZ = 0.0f;
        }
        Draw3DState ds;
        ds.depthWrite = !vm.has(AnmVm::ZWriteDisable);
        ds.clipX = viewport.x;
        ds.clipY = viewport.y;
        ds.clipW = viewport.w;
        ds.clipH = viewport.h;
        sink.drawQuad3D(vm.sprite->texture, vm.has(AnmVm::Additive) ? Blend::Additive : Blend::Alpha, v, ds);
    }
    vm.color = saved;
}

}  // namespace touhou::th07
