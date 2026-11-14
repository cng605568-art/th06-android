#include "th08/Background.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace touhou::th08 {

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

const char* stageFileName(int stage) {
    static const char* const kNames[kStageCount] = {"stage1", "stage2", "stage3", "stage4a", "stage4b",
                                                    "stage5", "stage6", "stage7", "stage8"};
    return stage >= 0 && stage < kStageCount ? kNames[stage] : "stage1";
}

const char* stageBackgroundAnm(int stage) {
    static const char* const kNames[kStageCount] = {"stg1bg.anm",  "stg2bg.anm", "stg3bg.anm", "stg4abg.anm", "stg4abg.anm",
                                                    "stg5bg.anm",  "stg6bg.anm", "stg7bg.anm", "stg8bg.anm"};
    return stage >= 0 && stage < kStageCount ? kNames[stage] : "stg1bg.anm";
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

// Per-channel (a x b) >> 8, all four channels (the stage-3 tint, spec 1.6).
std::uint32_t mulColor(std::uint32_t a, std::uint32_t b) {
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) out |= ((((a >> shift) & 0xFFu) * ((b >> shift) & 0xFFu)) >> 8) << shift;
    return out;
}

// The manual fog of facing quads and ribbon ends (spec 1.8): the colour pulled towards the fog
// colour by f and the alpha scaled by 1 - f.
std::uint32_t fogged(std::uint32_t c, std::uint32_t fogColor, float f) {
    std::uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const int cc = static_cast<int>((c >> shift) & 0xFF), fc = static_cast<int>((fogColor >> shift) & 0xFF);
        const auto t = static_cast<std::int32_t>(static_cast<float>(cc - fc) * f);
        out |= (static_cast<std::uint32_t>(cc - (t & 0xFF)) & 0xFFu) << shift;
    }
    out |= static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<float>(c >> 24) * (1.0f - f)) & 0xFF) << 24;
    return out;
}

constexpr float kTargetW = 640.0f, kTargetH = 480.0f;

}  // namespace

float Background::Record::f(int k) const {
    float v;
    std::memcpy(&v, &a[static_cast<std::size_t>(k)], 4);
    return v;
}

std::size_t Background::ribbonCount() const {
    std::size_t n = 0;
    for (const Object& o : objects_) {
        for (const Quad& q : o.quads) n += q.type == 1 ? 1 : 0;
    }
    return n;
}

// ---- loading ----------------------------------------------------------------------------

bool Background::load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, ZunRng& rng, int stage, bool spell,
                      std::string* error) {
    release(anm);
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (stage < 0 || stage >= kStageCount) return fail(std::format("no stage {}", stage));
    stage_ = stage;
    spellPractice = spell;
    std::string err;
    if (!anm.load(vfs, textures, stageBackgroundAnm(stage), kSlotBackground, &err)) return fail(err);
    const std::string stdName = std::format("{}{}.std", stageFileName(stage), spell ? "_s" : "");
    const auto file = vfs.read(stdName);
    if (!file) return fail(stdName + " is not in the archive");
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
        for (std::size_t q = at + 0x1C; q + 4 <= b.size();) {
            const std::int16_t type = rdI16(b, q);
            if (type < 0) break;
            const std::int16_t size = rdI16(b, q + 2);
            if (size < 0x1C || q + static_cast<std::size_t>(size) > b.size()) return fail("bad quad record");
            Quad quad;
            quad.type = type;
            quad.script = rdI16(b, q + 4);
            quad.pos = rdVec(b, q + 8);
            quad.width = rdF32(b, q + 0x14);
            quad.height = rdF32(b, q + 0x18);
            if (type == 1 && size >= 0x24) {  // a ribbon: point A, point B, width
                quad.pointB = rdVec(b, q + 0x14);
                quad.ribbonWidth = rdF32(b, q + 0x20);
            }
            o.quads.push_back(quad);
            ++quads;
            q += static_cast<std::size_t>(size);
        }
        objects_.push_back(std::move(o));
    }
    if (quads > quadCount) log::warn("{}: {} quads, the header says {}", stdName, quads, quadCount);
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

    // One VM per quad (both types), every script started now (its first tick runs at once).
    vms_.assign(static_cast<std::size_t>(quads), AnmVm{});
    int next = 0;
    for (Object& o : objects_) {
        o.flags = 1;
        for (Quad& q : o.quads) {
            q.vm = next;
            vms_[static_cast<std::size_t>(next++)].startFresh(anm, rng, kSlotBackground, q.script);
        }
    }
    // The human/youkai tint VM (spec 1.6): text.anm script 33, then interrupt 2.
    tintVm_ = AnmVm{};
    tintVm_.start(anm, rng, 0, kTintScript);
    tintVm_.interrupt(2);
    tintState_ = 0;
    tintTimer_.reset();
    fog_ = fogTarget_ = fogOrigin_ = Fog{};
    cam_ = Camera{};
    for (int i = 0; i < 3; ++i) {
        const Vec3f v = i == 0 ? cam_.pos : i == 1 ? cam_.facing : cam_.up;
        channels_[static_cast<std::size_t>(i)] = Channel{};
        channels_[static_cast<std::size_t>(i)].start = channels_[static_cast<std::size_t>(i)].target = v;
    }
    fovStart_ = fovTarget_ = cam_.fov;
    fovDuration_ = 0;
    sway_ = 0;
    swayClock_.reset();
    resumeLabel_ = 0;
    // The cull distance (spec 1.1 step 9): 1150 by default, 1350 for stage 5, 1800 for 6A/6B.
    const float cull = stage == 5 ? 1350.0f : (stage == 6 || stage == 7) ? 1800.0f : 1150.0f;
    cull2_ = cull * cull;
    spellState_ = SpellState::Inactive;
    spellCounter_ = 0;
    spellBase_ = 0;
    spellCount_ = 2;
    spellVms_ = {};
    loaded_ = true;
    log::info("Stage {} background ({}): {} objects, {} quads ({} ribbons), {} instances, {} script records", stage, stdName,
              objects_.size(), quads, ribbonCount(), instances_.size(), script_.size());
    return true;
}

void Background::release(AnmLibrary& anm) {
    if (loaded_) anm.release(kSlotBackground);
    loaded_ = false;
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
    backdropC_ = AnmVm{};
    tintVm_ = AnmVm{};
    tint_ = 0x00808080u;
    darkening_ = false;
    spellState_ = SpellState::Inactive;
    spellCounter_ = 0;
    spellVms_ = {};
    glowCount_ = 0;
    glowEnable_ = false;
    frames_ = 0;
    stage_ = -1;
}

void Background::spellStart(AnmLibrary& anm, ZunRng& rng) {
    spellState_ = SpellState::Starting;
    spellCounter_ = 0;
    for (int i = 0; i < spellCount_; ++i) spellVms_[static_cast<std::size_t>(i)].start(anm, rng, kSlotEffects, spellBase_ + i);
    spellDrawCallback = nullptr;
}

void Background::spellInterrupt(int label) {
    for (int i = 0; i < spellCount_; ++i) spellVms_[static_cast<std::size_t>(i)].interrupt(label);
}

void Background::setSpellBackgrounds(int base, int count) {
    spellBase_ = base;
    spellCount_ = std::clamp(count, 0, static_cast<int>(spellVms_.size()));
}

// ---- the logic pass ---------------------------------------------------------------------

void Background::update(AnmLibrary& anm, ZunRng& rng, bool frozen) {
    if (!loaded_ || frozen) return;
    // 2. The 6B moon (an effect in special slot 12, driven by the resume labels).
    if (stage_ == 7 && moonStep) moonStep(resumeLabel_);
    // 3. Resume at the label the ECL asked for (not found: index 0, label kept [QUIRK]).
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
    // 4. The script.
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
        execute(r, anm, rng);
        ++index_;
    }
    // 5. Camera interpolation: position, facing, up, then the field of view.
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
    // 6. The normalised facing (it feeds the weather particles).
    facingN_ = normalize(cam_.facing);
    // 7. Sway (op 33): the eye offset and/or the up vector follow a sine of channel 4's clock.
    if (sway_ != 0) {
        const float period = sway_ == 3 ? 4800.0f : 480.0f;
        float a = swayClock_.asFloat() * kZunPi;
        a = a + a;
        a = a / period;
        a = a - kZunPi;
        const float s = static_cast<float>(std::sin(static_cast<double>(a)));
        switch (sway_) {
            case 1: cam_.eye.x = s * 40.0f; break;
            case 2:
                cam_.eye.x = s * 70.0f;
                cam_.up.x = -s * 0.1f;
                break;
            case 3:
                cam_.up.x = s * 1.0f;
                cam_.up.z = static_cast<float>(std::cos(static_cast<double>(a))) * 1.0f;
                break;
            default: break;
        }
        swayClock_.tick();
        if (static_cast<float>(swayClock_.current) >= period) swayClock_.reset();
    }
    // 8. Fog fade.
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
    // 9. Script time freezes while the next pending record is a halt.
    if (stopOp != 3) time_.tick();
    // 10. The tint VM and the objects.
    updateObjects(rng);
    // 11. Spell-card backgrounds: starting becomes active on the 61st update.
    if (spellState_ != SpellState::Inactive) {
        if (spellCounter_ == 60 && spellState_ == SpellState::Starting) spellState_ = SpellState::Active;
        ++spellCounter_;
        for (int i = 0; i < spellCount_; ++i) spellVms_[static_cast<std::size_t>(i)].tick(rng);
    }
    // 12. Backdrops (a sprite index of 0 never animates [QUIRK]); C drives the clear colour.
    if (backdropA_.activeSprite > 0) backdropA_.tick(rng);
    if (backdropB_.activeSprite > 0) backdropB_.tick(rng);
    if (backdropC_.activeSprite > 0) {
        backdropC_.tick(rng);
        clearColor_ = backdropC_.color;
    }
    // 13. Glow particles: twelve spawns every third frame from stage frame 700 (always in spell
    // practice), always twelve, recorded or not (spec_effects §9 corrects spec_stage_gui 1.4).
    if (frames_ % 3 == 0 && (frames_ >= 700 || spellPractice) && spellState_ != SpellState::Active) {
        for (int i = 0; i < 12; ++i) {
            if (spawnGlow) spawnGlow(glow_[static_cast<std::size_t>(i)]);
        }
    }
    glowEnable_ = true;
    if (spellState_ == SpellState::Active) glowCount_ = 0;
    ++frames_;
}

void Background::updateObjects(ZunRng& rng) {
    if (tintState_ == 1 && !youkaiFront_) {
        tintState_ = 0;
        tintTimer_.reset();
        tintVm_.interrupt(2);
    } else if (tintState_ == 0 && youkaiFront_) {
        tintState_ = 1;
        tintTimer_.reset();
        tintVm_.interrupt(1);
    }
    tintTimer_.tick();
    tintVm_.tick(rng);
    AnmVm* last = nullptr;  // the loop's last quad VM: an object without quads reuses it [QUIRK]
    for (Object& o : objects_) {
        if (!(o.flags & 1)) continue;
        int running = 0;
        for (const Quad& q : o.quads) {
            AnmVm& vm = vms_[static_cast<std::size_t>(q.vm)];
            if (q.type == 0 || q.type == 1) vm.tick(rng);
            if (!vm.finished()) ++running;
            last = &vm;
        }
        // Only the last quad's VM takes the tint (the test sits after the quad loop [QUIRK]).
        if (last && last->autoRotate == 1) {
            last->set(AnmVm::UseAltColor, true);
            last->altColor = mulColor(last->color, tintVm_.color);
        }
        if (running == 0) o.flags &= static_cast<std::uint8_t>(~1u);
    }
}

void Background::execute(const Record& r, AnmLibrary& anm, ZunRng& rng) {
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
        case 29:
        case 30: {
            const std::int32_t n = r.i(0);
            if (n >= 0) (r.op == 29 ? backdropA_ : backdropB_).startFresh(anm, rng, kSlotBackground, n);
            else backdropA_.activeSprite = -1;  // both opcodes hide backdrop A [QUIRK]
            break;
        }
        case 32: cam_.eye = xyz; break;  // eye offset, at once
        case 33:
            sway_ = static_cast<int>(r.a[0] & 0xFF);
            swayClock_.reset();
            break;
        case 34: {
            const std::int32_t n = r.i(0);
            if (n >= 0) backdropC_.startFresh(anm, rng, kSlotBackground, n);
            else backdropC_.activeSprite = -1;
            break;
        }
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
    const Vec3f at = cam_.pos + cam_.facing;  // the eye offset moves the eye, not the target
    view_ = lookAtLH(eyePosition(), at, cam_.up);
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
    glowCount_ = 0;
    for (int i = 0; i < 16; ++i) glow_[static_cast<std::size_t>(i)] = {};
    if (!loaded_) return;
    // The stage tint becomes the colour multiplier (bombs), then resets.
    if ((tint_ >> 24) != 0) {
        state.multiply = true;
        state.multiplyColor = tint_;
    }
    tint_ = 0x00808080u;
    // Nothing of the stage while a card's background covers it or the stage has finished.
    const bool stageVisible = spellState_ != SpellState::Active && !stageFinished;
    if (stageVisible) {
        if (backdropA_.activeSprite > 0) drawVm(sink, backdropA_, state);
        if (backdropB_.activeSprite > 0) drawVm(sink, backdropB_, state);
        if (drawMoon) drawMoon(sink, state);
    }
    // The clear (spec 1.10 step 5): opaque erases the backdrops; translucent is drawn over them.
    if ((clearColor_ >> 24) == 0xFF) {
        sink.clearRect(viewport.x, viewport.y, viewport.w, viewport.h, argbToRgbaBytes(clearColor_), true);
    } else {
        if (clearColor_ != 0 && white) sink.fillRect(*white, viewport.x, viewport.y, viewport.w, viewport.h, argbToRgbaBytes(clearColor_));
        sink.clearRect(viewport.x, viewport.y, viewport.w, viewport.h, 0, false);
    }
    if (!stageVisible) return;
    drawLayer(sink, state, 0);
    drawLayer(sink, state, 1);
}

void Background::drawLow(SpriteSink& sink, AnmDrawState& state) {
    if (!loaded_) return;
    if (spellState_ != SpellState::Active && !stageFinished) {
        drawLayer(sink, state, 2);
        drawLayer(sink, state, 3);
    }
    if (drawEffectsB) drawEffectsB(sink, state);
    if (spellState_ == SpellState::Starting && white) {
        const std::int32_t a = std::clamp(spellCounter_ * 255 / 60, 0, 255);
        sink.fillRect(*white, viewport.x, viewport.y, viewport.w, viewport.h, rgba(0, 0, 0, static_cast<std::uint8_t>(a)));
    }
    if (spellState_ != SpellState::Inactive) {
        for (int i = 0; i < spellCount_; ++i) drawVm(sink, spellVms_[static_cast<std::size_t>(i)], state);
        if (spellDrawCallback) spellDrawCallback(sink, state);
    }
    buildPlayfieldCamera();
    if (!darkening_) state.multiply = false;
    darkening_ = false;
    glowEnable_ = false;
}

void Background::drawMirrorWindow(SpriteSink& sink, const AnmDrawState& state, const Vec3f& centre, float outerRadius,
                                  float innerRadius) {
    const float a = outerRadius * 0.70710683f, b = innerRadius * 0.70710683f;
    const float cx = 32.0f + centre.x, cy = 16.0f + centre.y;
    const float hi = std::max(a, b), lo = std::min(a, b);
    if (!(hi > lo)) return;
    AnmVm v0 = spellVms_[0], v1 = spellVms_[1];
    v0.scale = {-1.5f, -1.75f};
    v0.position = {416.0f, 464.0f, 0.7f};
    v0.color = 0xFFE0C0C0u;
    v1.rotation.z = v1.rotation.z * -1.0f;
    v1.position.z = 0.6f;
    v1.color = 0xFFE0C0C0u;
    // A pixel is inside an unrounded edge pair [p, q) when its Direct3D centre is: ceil(p) to
    // ceil(q); then the playfield.
    auto clipDraw = [&](float x0, float y0, float x1, float y1) {
        const float l = std::max(std::ceil(x0), 32.0f), r = std::min(std::ceil(x1), 416.0f);
        const float t = std::max(std::ceil(y0), 16.0f), btm = std::min(std::ceil(y1), 464.0f);
        if (r <= l || btm <= t) return;
        sink.setClip(l, t, r - l, btm - t);
        drawVm(sink, v0, state);
        drawVm(sink, v1, state);
    };
    clipDraw(cx - hi, cy - hi, cx + hi, cy - lo);  // top
    clipDraw(cx - hi, cy + lo, cx + hi, cy + hi);  // bottom
    clipDraw(cx - hi, cy - lo, cx - lo, cy + lo);  // left
    clipDraw(cx + lo, cy - lo, cx + hi, cy + lo);  // right
    sink.clearClip();
}

void Background::drawLayer(SpriteSink& sink, AnmDrawState& state, int layer) {
    buildStageCamera();
    const Vec3f eye = eyePosition();
    for (const Instance& inst : instances_) {
        Object& o = objects_[static_cast<std::size_t>(inst.object)];
        if (o.layer != layer) continue;
        const Vec3f c{o.origin.x + inst.pos.x - offset_.x + o.size.x / 2.0f, o.origin.y + inst.pos.y - offset_.y + o.size.y / 2.0f,
                      o.origin.z + inst.pos.z - offset_.z + o.size.z / 2.0f};
        const Vec3f d = c - eye;
        if (dot(d, d) > cull2_) continue;
        const float s = dot(d, facingN_);
        if (!(s <= o.size.length() / 2.0f + 960.0f && s >= 80.0f)) continue;  // NaN skips too
        o.flags |= 2;
        const Vec3f shift = inst.pos - offset_;
        for (const Quad& q : o.quads) {
            AnmVm& vm = vms_[static_cast<std::size_t>(q.vm)];
            if (q.type == 1) {
                drawRibbon(sink, vm, state, q, shift);
                continue;
            }
            if (q.type != 0) continue;
            vm.position = vm.offset + q.pos + shift;
            if (!vm.sprite) continue;
            if (q.width != 0.0f) vm.scale.x = q.width / vm.sprite->pixelWidth;
            if (q.height != 0.0f) vm.scale.y = q.height / vm.sprite->pixelHeight;
            if ((vm.autoRotate & 0xF) == 2) {
                const bool passed = drawFacingQuad(sink, vm, state, q.width != 0.0f ? q.width : vm.sprite->pixelWidth);
                // Lantern glow points (auto-rotate 0x12): this frame's screen positions.
                if (passed && ((vm.autoRotate >> 4) & 0xF) == 1 && glowEnable_ && glowCount_ < kMaxGlowPoints) {
                    glow_[static_cast<std::size_t>(glowCount_++)] = {vm.position.x, vm.position.y, 0.0f};
                }
            } else {
                draw3DQuad(sink, vm, state, vm.has(AnmVm::CameraMode) ? 1 : 0);
            }
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
    updateWorldMatrix(vm);
    Mat4 world = vm.worldMatrix;
    float tx = vm.position.x, ty = vm.position.y;
    if (vm.anchor() & 1u) tx += std::fabs(vm.spriteWidth * vm.scale.x / 2.0f);
    if (vm.anchor() & 2u) ty += std::fabs(vm.spriteHeight * vm.scale.y / 2.0f);
    world[12] = tx + state.offsetX;
    world[13] = ty + state.offsetY;
    world[14] = vm.position.z;
    if (cameraMode != currentCamera_) {
        if (cameraMode == 1) buildStageCamera();
        else buildPlayfieldCamera();
    }
    // The texture transform follows a change of sprite, or any u scroll; a v-only scroll keeps
    // the stale one (the second test reads u again [QUIRK], spec_anm_text 4.7).
    if (vm.sprite != uvCacheSprite_ || vm.uvScroll.x != 0.0f) {
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
        fogColor = (effectiveColor(tmp, state) & 0x00FFFFFFu) | 0xFF000000u;
    }
    ds.fogColor = argbToRgbaBytes(fogColor);
    ds.clipX = viewport.x;
    ds.clipY = viewport.y;
    ds.clipW = viewport.w;
    ds.clipH = viewport.h;
    sink.drawQuad3D(vm.sprite->texture, drawBlend(vm), v, ds);
}

// A 36-byte quad (spec 1.8): a screen-space ribbon from point A to point B, as wide as the
// record says (projected at each end), hand-fogged per end, drawn without fog.
void Background::drawRibbon(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, const Quad& q, const Vec3f& shift) {
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    if (currentCamera_ != 1) buildStageCamera();
    const Mat4 vp = multiply(view_, proj_);
    auto project = [&](const Vec3f& p) {
        const Vec4f c = transform(p, vp);
        return Vec3f{viewport.x + (1.0f + c.x / c.w) * viewport.w / 2.0f, viewport.y + (1.0f - c.y / c.w) * viewport.h / 2.0f, c.z / c.w};
    };
    const Vec3f axis = normalize({view_[0], view_[1], view_[2]});
    const float width = q.ribbonWidth != 0.0f ? q.ribbonWidth : vm.sprite->pixelWidth;
    const Vec3f eye = eyePosition();
    Vec3f p[2];
    float h[2];
    std::uint32_t colors[2];
    for (int e = 0; e < 2; ++e) {
        const Vec3f world = (e == 0 ? q.pos : q.pointB) + shift;
        p[e] = project(world);
        h[e] = (project(world + axis * width) - p[e]).length() / 2.0f;
        const float dist = (world - eye).length();
        std::uint32_t c = vm.color;
        if (fog_.nearZ < dist) {
            const float f = (fog_.nearZ - dist) / (fog_.nearZ - fog_.farZ);
            c = f < 1.0f ? fogged(vm.color, fog_.color, f) : (vm.color & 0x00FFFFFFu);  // f >= 1: alpha 0 [QUIRK]
        }
        colors[e] = c;
    }
    Vec3f dir = p[1] - p[0];
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-5f) return;
    dir = dir / len;
    if (p[0].z < 0.0f || p[0].z > 1.0f || p[1].z < 0.0f || p[1].z > 1.0f) return;
    const float u0 = vm.sprite->uvStartX + vm.uvScroll.x, u1 = vm.sprite->uvEndX + vm.uvScroll.x;
    const float v0 = vm.sprite->uvStartY + vm.uvScroll.y, v1 = vm.sprite->uvEndY + vm.uvScroll.y;
    const float xs[4] = {p[0].x + dir.y * h[0], p[0].x - dir.y * h[0], p[1].x + dir.y * h[1], p[1].x - dir.y * h[1]};
    const float ys[4] = {p[0].y - dir.x * h[0], p[0].y + dir.x * h[0], p[1].y - dir.x * h[1], p[1].y + dir.x * h[1]};
    const float zs[4] = {p[0].z, p[0].z, p[1].z, p[1].z};
    const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
    const std::uint32_t cs[4] = {colors[0], colors[0], colors[1], colors[1]};
    Vertex3D v[4];
    for (int i = 0; i < 4; ++i) {
        v[i].x = (xs[i] + state.offsetX + 0.5f) / kTargetW * 2.0f - 1.0f;
        v[i].y = 1.0f - (ys[i] + state.offsetY + 0.5f) / kTargetH * 2.0f;
        v[i].z = 2.0f * zs[i] - 1.0f;
        v[i].w = 1.0f;
        v[i].u = us[i];
        v[i].v = vs[i];
        v[i].color = argbToRgbaBytes(cs[i]);
        v[i].eyeZ = 0.0f;
    }
    Draw3DState ds;
    ds.depthWrite = !vm.has(AnmVm::ZWriteDisable);
    ds.clipX = viewport.x;
    ds.clipY = viewport.y;
    ds.clipW = viewport.w;
    ds.clipH = viewport.h;
    sink.drawQuad3D(vm.sprite->texture, drawBlend(vm), v, ds);
}

// The cached world matrix (ANM 4.6 step 1): rebuilt from the sprite matrix, the scale and the
// X, Y, Z rotations only when a dirty flag is set.
void Background::updateWorldMatrix(AnmVm& vm) {
    if (vm.has(AnmVm::SkipTransform) || !(vm.has(AnmVm::ScaleDirty) || vm.has(AnmVm::RotationDirty))) return;
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

// Screen-space corners (Direct3D pixels, order top-left, top-right, bottom-left,
// bottom-right) finished as the 2D path does (no rounding): the global offset, the +0.5 GL
// mapping, the sprite's UVs plus scroll, the effective colour. Depth "always".
void Background::emitScreenQuad(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, const float xs[4], const float ys[4]) {
    const AnmSprite& sp = *vm.sprite;
    const float us[4] = {sp.uvStartX, sp.uvEndX, sp.uvStartX, sp.uvEndX};
    const float vs[4] = {sp.uvStartY, sp.uvStartY, sp.uvEndY, sp.uvEndY};
    const std::uint32_t color = argbToRgbaBytes(effectiveColor(vm, state));
    Vertex3D v[4];
    for (int i = 0; i < 4; ++i) {
        v[i].x = (xs[i] + state.offsetX + 0.5f) / kTargetW * 2.0f - 1.0f;
        v[i].y = 1.0f - (ys[i] + state.offsetY + 0.5f) / kTargetH * 2.0f;
        v[i].z = -1.0f;  // the near end of the 3D depth range: passes against anything
        v[i].w = 1.0f;   // affine across the two triangles [QUIRK]
        v[i].u = us[i] + vm.uvScroll.x;
        v[i].v = vs[i] + vm.uvScroll.y;
        v[i].color = color;
        v[i].eyeZ = 0.0f;  // no fog
    }
    Draw3DState ds;
    ds.depthWrite = false;
    ds.clipX = viewport.x;
    ds.clipY = viewport.y;
    ds.clipW = viewport.w;
    ds.clipH = viewport.h;
    sink.drawQuad3D(sp.texture, drawBlend(vm), v, ds);
}

void Background::drawProjected(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state) {
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    updateWorldMatrix(vm);
    Mat4 world = vm.worldMatrix;
    float tx = vm.position.x, ty = vm.position.y;
    if (vm.anchor() & 1u) tx += std::fabs(vm.spriteWidth * vm.scale.x / 2.0f);
    if (vm.anchor() & 2u) ty += std::fabs(vm.spriteHeight * vm.scale.y / 2.0f);
    world[12] = tx;
    world[13] = ty;
    world[14] = vm.position.z;
    buildPlayfieldCamera();
    const Mat4 m = multiply(multiply(world, view_), proj_);
    const Vec3f corners[4] = {{-128, -128, 0}, {128, -128, 0}, {-128, 128, 0}, {128, 128, 0}};
    float xs[4], ys[4];
    for (int i = 0; i < 4; ++i) {
        const Vec4f c = transform(corners[i], m);
        xs[i] = viewport.x + (1.0f + c.x / c.w) * viewport.w / 2.0f;
        ys[i] = viewport.y + (1.0f - c.y / c.w) * viewport.h / 2.0f;
    }
    emitScreenQuad(sink, vm, state, xs, ys);
}

void Background::drawPlayfieldBillboard(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state) {
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    buildPlayfieldCamera();
    const Mat4 vpm = multiply(view_, proj_);
    auto project = [&](const Vec3f& p) {
        const Vec4f c = transform(p, vpm);
        return Vec3f{viewport.x + (1.0f + c.x / c.w) * viewport.w / 2.0f, viewport.y + (1.0f - c.y / c.w) * viewport.h / 2.0f, c.z / c.w};
    };
    const Vec3f centre = project(vm.position);
    if (centre.z < 0.0f || centre.z > 1.0f) return;
    // The stage camera's right vector sets the size, as the original takes it.
    const Vec3f right = normalize(cross(cam_.facing, cam_.up));
    const float k = (project(vm.position + right) - centre).length() * 0.5f;
    const float hw = k * vm.spriteWidth * vm.scale.x, hh = k * vm.spriteHeight * vm.scale.y;
    const float c = std::cos(vm.rotation.z), s = std::sin(vm.rotation.z);
    const float lx[4] = {-hw, hw, -hw, hw}, ly[4] = {-hh, -hh, hh, hh};
    float xs[4], ys[4];
    for (int i = 0; i < 4; ++i) {
        xs[i] = (lx[i] * c - ly[i] * s) + centre.x;
        ys[i] = (lx[i] * s + ly[i] * c) + centre.y;
        if (vm.anchor() & 1u) xs[i] += hw;
        if (vm.anchor() & 2u) ys[i] += hh;
    }
    emitScreenQuad(sink, vm, state, xs, ys);
}

bool Background::drawFacingQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, float width) {
    if (currentCamera_ != 1) buildStageCamera();
    const Mat4 vp = multiply(view_, proj_);
    auto project = [&](const Vec3f& p) {
        const Vec4f c = transform(p, vp);
        return Vec3f{viewport.x + (1.0f + c.x / c.w) * viewport.w / 2.0f, viewport.y + (1.0f - c.y / c.w) * viewport.h / 2.0f, c.z / c.w};
    };
    const Vec3f p0 = project(vm.position);
    const Vec3f axis = normalize({view_[0], view_[1], view_[2]});  // view row 0, as the original takes it
    const Vec3f p1 = project(vm.position + axis * (width * vm.scale.x));
    const float k = (p1 - p0).length() / width;
    vm.scale.x = vm.scale.y = k;
    if (width < 0.0f) vm.scale.y = -vm.scale.y;
    // Fog by hand on the colour, from the eye.
    const std::uint32_t saved = vm.color;
    const float dist = (vm.position - eyePosition()).length();
    if (dist > fog_.nearZ) {
        const float f = (fog_.nearZ - dist) / (fog_.nearZ - fog_.farZ);
        if (f >= 1.0f) return false;
        vm.color = fogged(vm.color, fog_.color, f);
    }
    vm.position = p0;
    if (p0.z >= 0.0f && p0.z <= 1.0f && vm.has(AnmVm::Visible) && vm.has(AnmVm::Active) && (vm.color >> 24) != 0) {
        const float w = vm.spriteWidth * vm.scale.x, h = vm.spriteHeight * vm.scale.y;
        const float x0 = (vm.anchor() & 1u) ? p0.x : p0.x - w / 2.0f;
        const float y0 = (vm.anchor() & 2u) ? p0.y : p0.y - h / 2.0f;
        const float xs[4] = {x0, x0 + w, x0, x0 + w}, ys[4] = {y0, y0, y0 + h, y0 + h};
        const float us[4] = {vm.sprite->uvStartX, vm.sprite->uvEndX, vm.sprite->uvStartX, vm.sprite->uvEndX};
        const float vs[4] = {vm.sprite->uvStartY, vm.sprite->uvStartY, vm.sprite->uvEndY, vm.sprite->uvEndY};
        const std::uint32_t color = argbToRgbaBytes(effectiveColor(vm, state));
        Vertex3D v[4];
        for (int i = 0; i < 4; ++i) {
            v[i].x = (xs[i] + state.offsetX + 0.5f) / kTargetW * 2.0f - 1.0f;
            v[i].y = 1.0f - (ys[i] + state.offsetY + 0.5f) / kTargetH * 2.0f;
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
        sink.drawQuad3D(vm.sprite->texture, drawBlend(vm), v, ds);
    }
    vm.color = saved;
    return true;
}

}  // namespace touhou::th08
