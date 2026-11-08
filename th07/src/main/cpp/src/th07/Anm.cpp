#include "th07/Anm.hpp"

#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace touhou::th07 {

// ---- timers -----------------------------------------------------------------------------

void Timer::tick() {
    previous = current;
    if (g_timing.speed <= 0.99f) {
        sub += g_timing.speed;
        if (sub >= 1.0f) {
            ++current;
            sub -= 1.0f;
        }
    } else {
        ++current;
    }
}

void Timer::decrement(std::int32_t v) {
    if (g_timing.forceInteger) {
        --current;
        sub = 0.0f;
        previous = -999;
    }
    if (g_timing.speed > 0.99f) {
        current -= v;
    } else if (v < 0) {
        increment(-v);
    } else {
        previous = current;
        sub -= static_cast<float>(v) * g_timing.speed;
        while (sub < 0.0f) {
            --current;
            sub += 1.0f;
        }
    }
}

void Timer::increment(std::int32_t v) {
    if (g_timing.forceInteger) {
        ++current;
        sub = 0.0f;
        previous = -999;
    }
    if (g_timing.speed > 0.99f) {
        current += v;
    } else if (v < 0) {
        decrement(-v);
    } else {
        previous = current;
        sub += static_cast<float>(v) * g_timing.speed;
        while (sub >= 1.0f) {
            ++current;
            sub -= 1.0f;
        }
    }
}

// ---- file data --------------------------------------------------------------------------

namespace {

constexpr int kHeaderSize = 0x40;
constexpr int kMaxStepsPerTick = 65536;

std::int32_t rdI32(const std::uint8_t* p) {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::int16_t rdI16(const std::uint8_t* p) {
    std::int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
std::uint16_t rdU16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
float rdF32(const std::uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
void wrI32(std::uint8_t* p, std::int32_t v) { std::memcpy(p, &v, 4); }
void wrF32(std::uint8_t* p, float v) { std::memcpy(p, &v, 4); }

std::uint8_t expand5(unsigned v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }
std::uint8_t expand6(unsigned v) { return static_cast<std::uint8_t>((v << 2) | (v >> 4)); }

}  // namespace

bool decodeThtx(std::span<const std::uint8_t> px, int format, int width, int height, std::vector<std::uint8_t>& rgba) {
    if (width <= 0 || height <= 0) return false;
    const int bpp = format == 1 ? 4 : format == 4 ? 3 : (format == 2 || format == 3 || format == 5) ? 2 : 0;
    if (bpp == 0) return false;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (px.size() < count * static_cast<std::size_t>(bpp)) return false;
    rgba.resize(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* s = px.data() + i * static_cast<std::size_t>(bpp);
        std::uint8_t* d = rgba.data() + i * 4;
        switch (format) {
            case 1:  // A8R8G8B8: bytes B, G, R, A
                d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
                break;
            case 2: {  // A1R5G5B5
                const unsigned v = rdU16(s);
                d[0] = expand5((v >> 10) & 31), d[1] = expand5((v >> 5) & 31), d[2] = expand5(v & 31);
                d[3] = (v & 0x8000) ? 255 : 0;
                break;
            }
            case 3: {  // R5G6B5
                const unsigned v = rdU16(s);
                d[0] = expand5((v >> 11) & 31), d[1] = expand6((v >> 5) & 63), d[2] = expand5(v & 31), d[3] = 255;
                break;
            }
            case 4:  // R8G8B8: bytes B, G, R
                d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
                break;
            case 5: {  // A4R4G4B4
                const unsigned v = rdU16(s);
                d[0] = static_cast<std::uint8_t>(((v >> 8) & 15) * 17), d[1] = static_cast<std::uint8_t>(((v >> 4) & 15) * 17);
                d[2] = static_cast<std::uint8_t>((v & 15) * 17), d[3] = static_cast<std::uint8_t>(((v >> 12) & 15) * 17);
                break;
            }
        }
    }
    return true;
}

// ---- the library ------------------------------------------------------------------------

bool AnmLibrary::load(const Vfs& vfs, TextureLibrary& textures, const std::string& path, int slot, int indexOffset,
                      std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = path + ": " + why;
        return false;
    };
    auto data = vfs.read(path);
    if (!data) return fail("not in the archive");
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(*data));
    const std::vector<std::uint8_t>& b = *bytes;
    std::size_t at = 0;
    int base = indexOffset;
    int entries = 0;
    const Vfs* vfsPtr = &vfs;
    while (true) {
        const int s = slot + entries;
        if (s < 0 || s >= kSlots) return fail("too many entries for the slots (slot " + std::to_string(s) + ")");
        if (at + kHeaderSize > b.size()) return fail("entry header cut off");
        release(s);
        const std::uint8_t* h = b.data() + at;
        if (rdI32(h + 0x28) != 2) return fail("not a version-2 ANM entry");
        const int spriteCount = rdI32(h + 0x00), scriptCount = rdI32(h + 0x04);
        const int nominalW = rdI32(h + 0x0C), nominalH = rdI32(h + 0x10);
        if (spriteCount < 0 || scriptCount < 0 || nominalW <= 0 || nominalH <= 0 ||
            at + kHeaderSize + 4ull * static_cast<unsigned>(spriteCount) + 8ull * static_cast<unsigned>(scriptCount) > b.size()) {
            return fail("bad entry header");
        }
        const int nameOff = rdI32(h + 0x1C);
        std::string texName;
        if (nameOff > 0 && at + static_cast<std::size_t>(nameOff) < b.size()) {
            for (std::size_t i = at + static_cast<std::size_t>(nameOff); i < b.size() && b[i]; ++i) texName.push_back(static_cast<char>(b[i]));
        }

        // The texture: embedded THTX, a blank '@' texture, or (unused by the shipped data) a file.
        float texW = static_cast<float>(nominalW), texH = static_cast<float>(nominalH);
        std::uint16_t texture = 0;
        const std::string id = std::format("th07:{}#{}", path, entries);
        if (h[0x34] != 0) {
            const std::size_t t = at + static_cast<std::size_t>(rdI32(h + 0x30));
            if (t + 0x10 > b.size()) return fail("THTX block cut off");
            const int format = rdI16(b.data() + t + 6), w = rdI16(b.data() + t + 8), hgt = rdI16(b.data() + t + 10);
            std::vector<std::uint8_t> probe;
            if (!decodeThtx(std::span(b).subspan(t + 0x10), format, w, hgt, probe)) return fail("unreadable THTX texture");
            texW = static_cast<float>(w);
            texH = static_cast<float>(hgt);
            const std::size_t entryAt = at;
            texture = textures.add(id, w, hgt, [vfsPtr, path, entryAt]() -> std::optional<Image> {
                    const auto file = vfsPtr->read(path);
                    if (!file || entryAt + kHeaderSize > file->size()) return std::nullopt;
                    const std::size_t thtx = entryAt + static_cast<std::size_t>(rdI32(file->data() + entryAt + 0x30));
                    if (thtx + 0x10 > file->size()) return std::nullopt;
                    const int f = rdI16(file->data() + thtx + 6), ww = rdI16(file->data() + thtx + 8), hh = rdI16(file->data() + thtx + 10);
                    Image img(ww, hh);
                    if (!decodeThtx(std::span(*file).subspan(thtx + 0x10), f, ww, hh, img.rgba)) return std::nullopt;
                    return img;
                }, TextureFilter::LinearWrap);
        } else {
            if (texName.empty() || texName[0] != '@') log::warn("{}: entry {} names an external image ({}); drawn blank", path, entries, texName);
            texture = textures.addDynamic(id, nominalW, nominalH);
        }

        Slot& sl = slots_[static_cast<std::size_t>(s)];
        sl.used = true;
        sl.bytes = bytes;
        sl.entryOffset = at;
        sl.indexOffset = base;
        sl.texture = texture;
        sl.name = path;
        sl.childCount = 0;

        // Sprites, in table order: the derived values the game computes at load.
        int maxId = 0;
        const float cols = texW / static_cast<float>(nominalW), rows = texH / static_cast<float>(nominalH);
        for (int i = 0; i < spriteCount; ++i) {
            const std::size_t rec = at + static_cast<std::size_t>(rdI32(h + kHeaderSize + 4 * i));
            if (rec + 20 > b.size()) return fail("sprite record cut off");
            const std::uint8_t* r = b.data() + rec;
            const int sid = rdI32(r);
            const int index = sid + base;
            if (index < 0 || index >= kIndices) return fail("sprite index out of range");
            const float x = rdF32(r + 4), y = rdF32(r + 8), w = rdF32(r + 12), hh = rdF32(r + 16);
            AnmSprite sp;
            sp.sourceSlot = s;
            sp.texture = texture;
            sp.cols = cols;
            sp.rows = rows;
            sp.startX = cols * x;
            sp.startY = rows * y;
            sp.endX = (x + w) * cols;
            sp.endY = (y + hh) * rows;
            sp.textureWidth = texW;
            sp.textureHeight = texH;
            sp.uvStartX = sp.startX / texW;
            sp.uvStartY = sp.startY / texH;
            sp.uvEndX = sp.endX / texW;
            sp.uvEndY = sp.endY / texH;
            sp.pixelWidth = (sp.endX - sp.startX) / cols;
            sp.pixelHeight = (sp.endY - sp.startY) / rows;
            sprites_[static_cast<std::size_t>(index)] = sp;
            if (sid > maxId) maxId = sid;
        }
        // Scripts, in table order.
        for (int i = 0; i < scriptCount; ++i) {
            const std::uint8_t* pair = h + kHeaderSize + 4 * spriteCount + 8 * i;
            const int sid = rdI32(pair), off = rdI32(pair + 4);
            const int index = sid + base;
            if (index < 0 || index >= kIndices) return fail("script index out of range");
            if (off < 0 || at + static_cast<std::size_t>(off) + 8 > b.size()) return fail("script outside the file");
            scripts_[static_cast<std::size_t>(index)] = ScriptRef{&sl, at + static_cast<std::size_t>(off), base};
            if (sid > maxId) maxId = sid;
        }
        ++entries;
        base += maxId + 1;
        const int next = rdI32(h + 0x38);
        if (next == 0) break;
        if (next < 0) return fail("bad entry chain");
        at += static_cast<std::size_t>(next);
    }
    slots_[static_cast<std::size_t>(slot)].childCount = entries;
    return true;
}

void AnmLibrary::release(int slot) {
    if (slot < 0 || slot >= kSlots || !slots_[static_cast<std::size_t>(slot)].used) return;
    Slot& sl = slots_[static_cast<std::size_t>(slot)];
    for (int k = 1; k < sl.childCount; ++k) release(slot + k);
    for (AnmSprite& sp : sprites_) {
        if (sp.sourceSlot == slot) sp = AnmSprite{};
    }
    for (ScriptRef& r : scripts_) {
        if (r.slot == &sl) r = ScriptRef{};
    }
    sl = Slot{};
}

const AnmSprite* AnmLibrary::sprite(int index) const {
    if (index < 0 || index >= kIndices) return nullptr;
    const AnmSprite& sp = sprites_[static_cast<std::size_t>(index)];
    return sp.sourceSlot < 0 ? nullptr : &sp;
}

const std::uint8_t* AnmLibrary::script(int index) const {
    if (index < 0 || index >= kIndices) return nullptr;
    const ScriptRef& r = scripts_[static_cast<std::size_t>(index)];
    return r.slot ? r.slot->bytes->data() + r.offset : nullptr;
}

int AnmLibrary::scriptEntryOffset(int index) const {
    return index >= 0 && index < kIndices ? scripts_[static_cast<std::size_t>(index)].entryOffset : 0;
}

const std::uint8_t* AnmLibrary::bufferEnd(int index) const {
    if (index < 0 || index >= kIndices) return nullptr;
    const ScriptRef& r = scripts_[static_cast<std::size_t>(index)];
    return r.slot ? r.slot->bytes->data() + r.slot->bytes->size() : nullptr;
}

std::uint16_t AnmLibrary::slotTexture(int slot) const { return loaded(slot) ? slots_[static_cast<std::size_t>(slot)].texture : 0; }
int AnmLibrary::slotIndexOffset(int slot) const { return loaded(slot) ? slots_[static_cast<std::size_t>(slot)].indexOffset : 0; }
std::vector<std::uint8_t>* AnmLibrary::slotBytes(int slot) {
    return loaded(slot) ? slots_[static_cast<std::size_t>(slot)].bytes.get() : nullptr;
}

// ---- the VM -----------------------------------------------------------------------------

namespace {

std::array<float, 16> identity() {
    std::array<float, 16> m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

float easeValue(int mode, float t) {
    switch (mode) {
        case 1: return t * t;
        case 2: return (t * t) * t;
        case 3: {
            const float s = t * t;
            return s * s;
        }
        case 4: {
            float u = 1.0f - t;
            u = u * u;
            return 1.0f - u;
        }
        case 5: {
            float u = 1.0f - t;
            u = (u * u) * u;
            return 1.0f - u;
        }
        case 6: {
            float u = 1.0f - t;
            u = u * u;
            u = u * u;
            return 1.0f - u;
        }
        default: return t;
    }
}

std::uint8_t lerpByte(std::uint8_t s, std::uint8_t e, float t) {
    const float v = static_cast<float>(static_cast<int>(e) - static_cast<int>(s)) * t + static_cast<float>(s);
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(v) & 0xFF);
}

std::uint8_t byteOf(std::uint32_t c, int shift) { return static_cast<std::uint8_t>(c >> shift); }

// The register ids of the instruction operands.
constexpr std::int32_t kI0 = 10000, kF0 = 10004, kJ0 = 10008;

}  // namespace

void AnmVm::initialise() {
    rotation = {};
    angularVelocity = {};
    scale = {1, 1};
    scaleGrowth = {};
    uvScroll = {};
    clock.reset();
    waitTimer.reset();
    for (int i = 0; i < 5; ++i) {
        progress[i].reset();
        duration[i].reset();
    }
    ease = {};
    intRegs = {};
    floatRegs = {};
    intRegs2 = {};
    uvVelocity = {};
    spriteMatrix = identity();
    worldMatrix = {};
    uvMatrix = {};
    color = 0xFFFFFFFFu;
    altColor = 0;
    flags = Visible | Active | RotationDirty;
    autoRotate = 0;
    pendingInterrupt = 0;
}

namespace {

void startScript(AnmVm& vm, AnmLibrary& lib, ZunRng& rng, int index) {
    std::uint8_t* p = const_cast<std::uint8_t*>(lib.script(index));
    if (!p) {
        vm = AnmVm{};
        vm.scale = {0, 0};
        vm.color = 0;
        vm.scriptIndex = 0;
        return;
    }
    vm.flags &= ~(AnmVm::FlipX | AnmVm::FlipY);
    vm.initialise();
    vm.scriptStart = vm.current = p;
    vm.scriptEnd = lib.bufferEnd(index);
    vm.clock.reset();
    vm.set(AnmVm::Visible, false);
    vm.tick(lib, rng);
}

}  // namespace

void AnmVm::start(AnmLibrary& lib, ZunRng& rng, int index) {
    scriptIndex = static_cast<std::int16_t>(index);
    startScript(*this, lib, rng, index);
}

void AnmVm::startFresh(AnmLibrary& lib, ZunRng& rng, int index) {
    scriptIndex = static_cast<std::int16_t>(index);
    position = {};
    offset = {};
    fontWidth = fontHeight = 15;
    startScript(*this, lib, rng, index);
}

bool AnmVm::selectSprite(const AnmLibrary& lib, int index) {
    const AnmSprite* sp = lib.sprite(index);
    if (!sp) return false;
    activeSprite = static_cast<std::int16_t>(index);
    sprite = sp;
    spriteMatrix = identity();
    spriteMatrix[0] = sp->pixelWidth / 256.0f;
    spriteMatrix[5] = sp->pixelHeight / 256.0f;
    uvMatrix = identity();
    uvMatrix[0] = sp->pixelWidth / sp->textureWidth * sp->cols;
    uvMatrix[5] = sp->pixelHeight / sp->textureHeight * sp->rows;
    worldMatrix = spriteMatrix;
    return true;
}

namespace {

// Operand access for one instruction (3.2 of the spec): literals, registers, and writes to
// literal cells that change the script bytes.
struct Ops {
    AnmVm& vm;
    std::uint8_t* ins;
    std::uint16_t mask;
    std::uint8_t* cell(int k) const { return ins + 8 + 4 * k; }
    bool reg(int k) const { return (mask >> k) & 1u; }
    std::int32_t lit(int k) const { return rdI32(cell(k)); }
    std::uint8_t byte(int k, int j) const { return cell(k)[j]; }

    std::int32_t iv(int k) const {
        if (!reg(k)) return lit(k);
        const std::int32_t id = lit(k);
        if (id >= kI0 && id < kI0 + 4) return vm.intRegs[static_cast<std::size_t>(id - kI0)];
        if (id >= kF0 && id < kF0 + 4) return static_cast<std::int32_t>(vm.floatRegs[static_cast<std::size_t>(id - kF0)]);
        if (id >= kJ0 && id < kJ0 + 2) return vm.intRegs2[static_cast<std::size_t>(id - kJ0)];
        return id;
    }
    float fv(int k) const {
        const float raw = rdF32(cell(k));
        if (!reg(k)) return raw;
        const auto id = static_cast<std::int32_t>(raw);
        if (id >= kF0 && id < kF0 + 4) return vm.floatRegs[static_cast<std::size_t>(id - kF0)];
        if (id >= kI0 && id < kI0 + 4) return static_cast<float>(vm.intRegs[static_cast<std::size_t>(id - kI0)]);
        if (id >= kJ0 && id < kJ0 + 2) return static_cast<float>(vm.intRegs2[static_cast<std::size_t>(id - kJ0)]);
        return raw;
    }
    std::int32_t* intReg(int k) const {
        if (!reg(k)) return nullptr;
        const std::int32_t id = lit(k);
        if (id >= kI0 && id < kI0 + 4) return &vm.intRegs[static_cast<std::size_t>(id - kI0)];
        if (id >= kJ0 && id < kJ0 + 2) return &vm.intRegs2[static_cast<std::size_t>(id - kJ0)];
        return nullptr;  // anything else, a float register id included, writes the literal cell
    }
    float* floatReg(int k) const {
        if (!reg(k)) return nullptr;
        const auto id = static_cast<std::uint32_t>(rdF32(cell(k)));
        if (id >= static_cast<std::uint32_t>(kF0) && id < static_cast<std::uint32_t>(kF0 + 4)) return &vm.floatRegs[id - kF0];
        return nullptr;
    }
    std::int32_t getI(int k) const {
        const std::int32_t* r = intReg(k);
        return r ? *r : lit(k);
    }
    void setI(int k, std::int32_t v) const {
        if (std::int32_t* r = intReg(k)) *r = v;
        else wrI32(cell(k), v);
    }
    float getF(int k) const {
        const float* r = floatReg(k);
        return r ? *r : rdF32(cell(k));
    }
    void setF(int k, float v) const {
        if (float* r = floatReg(k)) *r = v;
        else wrF32(cell(k), v);
    }
};

float lowRound(float v) { return static_cast<float>(std::nearbyint(v)); }

}  // namespace

bool AnmVm::tick(AnmLibrary& lib, ZunRng& rng) {
    if (!current) return true;
    int steps = 0;
    auto bad = [&](const char* why) {
        log::warn("ANM script {}: {}; stopped", scriptIndex, why);
        current = nullptr;
        return true;
    };
    auto jumpTo = [&](std::int32_t target, std::int32_t time) {
        current = scriptStart + target;
        clock.assign(time);
    };
    bool hold = false;  // a stop or wait: the clock is held, straight to the post-tick

    auto runInterrupt = [&]() -> bool {  // false: no label, hold
        const int p = pendingInterrupt;
        std::uint8_t* found = nullptr;
        std::uint8_t* fallback = nullptr;
        for (std::uint8_t* q = scriptStart; q && q + 8 <= scriptEnd;) {
            const std::int16_t op = rdI16(q);
            if (op == -1) break;
            if (op == 21 && q + 12 <= scriptEnd) {
                const std::int32_t label = rdI32(q + 8);
                if (label == p) {
                    found = q;
                    break;
                }
                if (label == -1) fallback = q;
            }
            const std::uint16_t size = rdU16(q + 2);
            if (size < 8) break;
            q += size;
        }
        pendingInterrupt = 0;
        set(Stopped, false);
        std::uint8_t* label = found ? found : fallback;
        if (!label) {
            clock.decrement(1);
            return false;
        }
        current = label + rdU16(label + 2);
        if (current + 8 > scriptEnd) {
            current = nullptr;
            return false;
        }
        clock.assign(rdI16(current + 4));
        set(Visible, true);
        return true;
    };

    if (pendingInterrupt != 0 && !runInterrupt()) hold = true;

    while (!hold && current) {
        if (current + 8 > scriptEnd) return bad("instruction past the end of the file");
        std::uint8_t* ins = current;
        if (rdI16(ins + 4) > clock.current) break;
        if (++steps > kMaxStepsPerTick) return bad("runs away (too many instructions in one tick)");
        const std::int16_t op = rdI16(ins);
        const std::uint16_t size = rdU16(ins + 2);
        if (op != -1 && (size < 8 || ins + size > scriptEnd)) return bad("malformed instruction");
        const Ops o{*this, ins, rdU16(ins + 6)};
        bool step = true;
        switch (op) {
            case -1:
            case 1:
                set(Visible, false);
                current = nullptr;
                return true;
            case 2:
                current = nullptr;
                return true;
            case 3:
                set(Visible, true);
                selectSprite(lib, o.iv(0) + lib.scriptEntryOffset(scriptIndex));
                lastSpriteTime = clock.current;
                break;
            case 4:
                jumpTo(o.lit(0), o.lit(1));
                step = false;
                break;
            case 5:
                o.setI(0, o.getI(0) - 1);
                if (o.iv(0) > 0) {
                    jumpTo(o.lit(1), o.lit(2));
                    step = false;
                }
                break;
            case 6: {
                const Vec3f v{o.fv(0), o.fv(1), o.fv(2)};
                if (has(UseOffset)) offset = v;
                else position = v;
                break;
            }
            case 7:
                scale = {o.fv(0), o.fv(1)};
                set(ScaleDirty, true);
                break;
            case 8:
                color = (color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(o.lit(0) & 0xFF) << 24);
                break;
            case 9:
                color = (color & 0xFF000000u) | (static_cast<std::uint32_t>(o.lit(0)) & 0x00FFFFFFu);
                break;
            case 10:
                flags ^= FlipX;
                scale.x = -scale.x;
                set(ScaleDirty, true);
                break;
            case 11:
                flags ^= FlipY;
                scale.y = -scale.y;
                set(ScaleDirty, true);
                break;
            case 12:
                rotation = {o.fv(0), o.fv(1), o.fv(2)};
                set(RotationDirty, true);
                break;
            case 13:
                angularVelocity = {o.fv(0), o.fv(1), o.fv(2)};
                set(RotationDirty, true);
                break;
            case 14:
                scaleGrowth = {o.fv(0), o.fv(1)};
                break;
            case 15:
                progress[Alpha].reset();
                duration[Alpha].assign(o.iv(1));
                ease[Alpha] = 0;
                colorStart = (colorStart & 0x00FFFFFFu) | (color & 0xFF000000u);
                colorEnd = (colorEnd & 0x00FFFFFFu) | (static_cast<std::uint32_t>(o.byte(0, 0)) << 24);
                break;
            case 16:
                set(Additive, (o.lit(0) & 1) != 0);
                break;
            case 17:
            case 18:
            case 19:
                progress[Position].reset();
                duration[Position].assign(o.iv(3));
                ease[Position] = op == 17 ? 0 : op == 18 ? 4 : 6;
                posStart = has(UseOffset) ? offset : position;
                posEnd = {o.fv(0), o.fv(1), o.fv(2)};
                break;
            case 20:
            case 23:
                if (op == 23) set(Visible, false);
                if (pendingInterrupt == 0) {
                    set(Stopped, true);
                    clock.decrement(1);
                    hold = true;
                } else if (!runInterrupt()) {
                    hold = true;
                }
                step = false;
                break;
            case 21:
                break;
            case 22:
                flags |= AnchorLeft | AnchorTop;
                break;
            case 24:
                set(UseOffset, (o.lit(0) & 1) != 0);
                break;
            case 25:
                autoRotate = static_cast<std::int16_t>(rdU16(o.cell(0)));
                break;
            case 26:
            case 27: {
                float& s = op == 26 ? uvScroll.x : uvScroll.y;
                s += o.fv(0);
                if (s >= 1.0f) s -= 1.0f;
                else if (s < 0.0f) s += 1.0f;
                break;
            }
            case 28:
                set(Visible, (o.lit(0) & 1) != 0);
                break;
            case 29:
                progress[Scale].reset();
                duration[Scale].assign(o.iv(2));
                ease[Scale] = 0;
                scaleStart = scale;
                scaleEnd = {o.fv(0), o.fv(1)};
                break;
            case 30:
                set(ZWriteDisable, (o.lit(0) & 1) != 0);
                break;
            case 31:
                set(CameraMode, (o.lit(0) & 1) != 0);
                break;
            case 32:
                progress[Position].reset();
                duration[Position].assign(o.iv(0));
                ease[Position] = o.byte(1, 0);
                posStart = has(UseOffset) ? offset : position;
                posEnd = {o.fv(2), o.fv(3), o.fv(4)};
                break;
            case 33:
                progress[Color].reset();
                duration[Color].assign(o.iv(0));
                ease[Color] = o.byte(1, 0);
                colorStart = (colorStart & 0xFF000000u) | (color & 0x00FFFFFFu);
                colorEnd = (colorEnd & 0xFF000000u) | (static_cast<std::uint32_t>(o.byte(2, 0)) << 16) |
                           (static_cast<std::uint32_t>(o.byte(2, 1)) << 8) | o.byte(2, 2);
                break;
            case 34:
                progress[Alpha].reset();
                duration[Alpha].assign(o.iv(0));
                ease[Alpha] = o.byte(1, 0);
                colorStart = (colorStart & 0x00FFFFFFu) | (color & 0xFF000000u);
                colorEnd = (colorEnd & 0x00FFFFFFu) | (static_cast<std::uint32_t>(o.byte(2, 0)) << 24);
                break;
            case 35:
                progress[Rotation].reset();
                duration[Rotation].assign(o.iv(0));
                ease[Rotation] = o.byte(1, 0);
                rotStart = rotation;
                rotEnd = {o.fv(2), o.fv(3), o.fv(4)};
                set(RotationDirty, true);
                break;
            case 36:
                progress[Scale].reset();
                duration[Scale].assign(o.iv(0));
                ease[Scale] = o.byte(1, 0);
                scaleStart = scale;
                scaleEnd = {o.fv(2), o.fv(3)};
                set(ScaleDirty, true);
                break;
            // Registers and arithmetic.
            case 37: o.setI(0, o.iv(1)); break;
            case 38: o.setF(0, o.fv(1)); break;
            case 39: o.setI(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.getI(0)) + static_cast<std::uint32_t>(o.iv(1)))); break;
            case 40: o.setF(0, o.getF(0) + o.fv(1)); break;
            case 41: o.setI(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.getI(0)) - static_cast<std::uint32_t>(o.iv(1)))); break;
            case 42: o.setF(0, o.getF(0) - o.fv(1)); break;
            case 43: o.setI(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.getI(0)) * static_cast<std::uint32_t>(o.iv(1)))); break;
            case 44: o.setF(0, o.getF(0) * o.fv(1)); break;
            case 45:
                if (o.iv(1) == 0) log::warn("ANM script {}: division by zero ignored", scriptIndex);
                else o.setI(0, o.getI(0) / o.iv(1));
                break;
            case 46: o.setF(0, o.getF(0) / o.fv(1)); break;
            case 47:
                if (o.iv(1) == 0) log::warn("ANM script {}: division by zero ignored", scriptIndex);
                else o.setI(0, o.getI(0) % o.iv(1));
                break;
            case 48: o.setF(0, std::fmod(o.fv(0), o.fv(1))); break;
            case 49: o.setI(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.iv(1)) + static_cast<std::uint32_t>(o.iv(2)))); break;
            case 50: o.setF(0, o.fv(1) + o.fv(2)); break;
            case 51: o.setI(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.iv(1)) - static_cast<std::uint32_t>(o.iv(2)))); break;
            case 52: o.setF(0, o.fv(1) - o.fv(2)); break;
            case 53: o.setI(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.iv(1)) * static_cast<std::uint32_t>(o.iv(2)))); break;
            case 54: o.setF(0, o.fv(1) * o.fv(2)); break;
            case 55:
                if (o.iv(2) == 0) log::warn("ANM script {}: division by zero ignored", scriptIndex);
                else o.setI(0, o.iv(1) / o.iv(2));
                break;
            case 56: o.setF(0, o.fv(1) / o.fv(2)); break;
            case 57:
                if (o.iv(2) == 0) log::warn("ANM script {}: division by zero ignored", scriptIndex);
                else o.setI(0, o.iv(1) % o.iv(2));
                break;
            case 58: o.setF(0, std::fmod(o.fv(1), o.fv(2))); break;
            case 59: o.setI(0, static_cast<std::int32_t>(rng.u32InRange(static_cast<std::uint32_t>(o.iv(1))))); break;
            case 60: o.setF(0, static_cast<float>(rng.u32()) / 4294967296.0f * o.fv(1)); break;
            case 61: o.setF(0, static_cast<float>(std::sin(static_cast<double>(o.fv(1))))); break;
            case 62: o.setF(0, static_cast<float>(std::cos(static_cast<double>(o.fv(1))))); break;
            case 63: o.setF(0, static_cast<float>(std::tan(static_cast<double>(o.fv(1))))); break;
            case 64: o.setF(0, static_cast<float>(std::acos(static_cast<double>(o.fv(1))))); break;
            case 65: o.setF(0, static_cast<float>(std::atan(static_cast<double>(o.fv(1))))); break;
            case 66: o.setF(0, addNormalizeAngle(o.fv(0), 0.0f)); break;
            case 67: case 68: case 69: case 70: case 71: case 72:
            case 73: case 74: case 75: case 76: case 77: case 78: {
                bool yes = false;
                const int cmp = (op - 67) / 2;  // ==, !=, <, <=, >, >=
                if ((op & 1) != 0) {
                    const std::int32_t a = o.iv(0), b = o.iv(1);
                    yes = cmp == 0 ? a == b : cmp == 1 ? a != b : cmp == 2 ? a < b : cmp == 3 ? a <= b : cmp == 4 ? a > b : a >= b;
                } else {
                    const float a = o.fv(0), b = o.fv(1);
                    yes = cmp == 0 ? a == b : cmp == 1 ? a != b : cmp == 2 ? a < b : cmp == 3 ? a <= b : cmp == 4 ? a > b : a >= b;
                }
                if (yes) {
                    jumpTo(o.lit(2), o.lit(3));
                    step = false;
                }
                break;
            }
            case 79:
                if (waitTimer.current == 0) waitTimer.assign(o.iv(0));
                else waitTimer.decrement(1);
                if (waitTimer.current <= 0) {
                    waitTimer.assign(0);
                } else {
                    clock.decrement(1);
                    hold = true;
                    step = false;
                }
                break;
            case 80: uvVelocity.x = o.fv(0); break;
            case 81: uvVelocity.y = o.fv(0); break;
            default:
                break;  // 0 and every unlisted opcode: no-op
        }
        if (step && current == ins) current = ins + size;
    }

    // Post-tick: angular velocity, interpolation channels, scale growth, UV scroll.
    const float m = g_timing.speed;
    float* rot[3] = {&rotation.x, &rotation.y, &rotation.z};
    const float vel[3] = {angularVelocity.x, angularVelocity.y, angularVelocity.z};
    for (int i = 0; i < 3; ++i) {
        if (vel[i] != 0.0f) {
            *rot[i] = addNormalizeAngle(*rot[i], m * vel[i]);
            set(RotationDirty, true);
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (duration[i].current <= 0) continue;
        progress[i].tick();
        float t;
        if (progress[i].current >= duration[i].current) {
            t = 1.0f;
            duration[i].assign(0);
        } else {
            t = progress[i].asFloat() / duration[i].asFloat();
        }
        t = easeValue(ease[i], t);
        switch (i) {
            case Position: {
                const Vec3f v{(posEnd.x - posStart.x) * t + posStart.x, (posEnd.y - posStart.y) * t + posStart.y,
                              (posEnd.z - posStart.z) * t + posStart.z};
                if (has(UseOffset)) offset = v;
                else position = v;
                break;
            }
            case Color:
                color = (color & 0xFF000000u) | (static_cast<std::uint32_t>(lerpByte(byteOf(colorStart, 16), byteOf(colorEnd, 16), t)) << 16) |
                        (static_cast<std::uint32_t>(lerpByte(byteOf(colorStart, 8), byteOf(colorEnd, 8), t)) << 8) |
                        lerpByte(byteOf(colorStart, 0), byteOf(colorEnd, 0), t);
                break;
            case Alpha:
                color = (color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(lerpByte(byteOf(colorStart, 24), byteOf(colorEnd, 24), t)) << 24);
                break;
            case Rotation:
                rotation = {addNormalizeAngle((rotEnd.x - rotStart.x) * t + rotStart.x, 0.0f),
                            addNormalizeAngle((rotEnd.y - rotStart.y) * t + rotStart.y, 0.0f),
                            addNormalizeAngle((rotEnd.z - rotStart.z) * t + rotStart.z, 0.0f)};
                set(RotationDirty, true);
                break;
            case Scale:
                scale = {(scaleEnd.x - scaleStart.x) * t + scaleStart.x, (scaleEnd.y - scaleStart.y) * t + scaleStart.y};
                set(ScaleDirty, true);
                break;
        }
    }
    if (scaleGrowth.y != 0.0f) {
        scale.y += m * scaleGrowth.y;
        set(ScaleDirty, true);
    }
    if (scaleGrowth.x != 0.0f) {
        scale.x += m * scaleGrowth.x;
        set(ScaleDirty, true);
        set(RotationDirty, true);
    }
    for (float* s : {&uvScroll.x, &uvScroll.y}) {
        *s += s == &uvScroll.x ? uvVelocity.x : uvVelocity.y;
        if (*s >= 1.0f) *s -= 1.0f;
        else if (*s < 0.0f) *s += 1.0f;
    }
    clock.tick();
    return false;
}

// ---- drawing ----------------------------------------------------------------------------

std::uint32_t effectiveColor(const AnmVm& vm, const AnmDrawState& state) {
    std::uint32_t c = vm.has(AnmVm::UseAltColor) ? vm.altColor : vm.color;
    if (state.multiply) {
        std::uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const std::uint32_t v = ((c >> shift) & 0xFFu) * ((state.multiplyColor >> shift) & 0xFFu) >> 7;
            out |= (v > 255 ? 255u : v) << shift;
        }
        c = out;
    }
    return c;
}

namespace {

bool drawable(const AnmVm& vm) {
    return vm.sprite && vm.has(AnmVm::Visible) && vm.has(AnmVm::Active) && (vm.color >> 24) != 0;
}

// Direct3D puts pixel centres on integers, GL at +0.5: an unrounded Direct3D coordinate v
// covers what the GL coordinate v + 0.5 does; a rounded one (round(v) - 0.5) is round(v).
void emit(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, float cx, float cy, float w, float h, float angle) {
    SpriteInstance s;
    s.x = cx;
    s.y = cy;
    s.w = w;
    s.h = h;
    s.u0 = vm.sprite->uvStartX + vm.uvScroll.x;
    s.u1 = vm.sprite->uvEndX + vm.uvScroll.x;
    s.v0 = vm.sprite->uvStartY + vm.uvScroll.y;
    s.v1 = vm.sprite->uvEndY + vm.uvScroll.y;
    s.color = argbToRgbaBytes(effectiveColor(vm, state));
    s.angle = angle;
    sink.draw(vm.sprite->texture, vm.has(AnmVm::Additive) ? Blend::Additive : Blend::Alpha, s);
}

void drawUnrotated(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, bool round) {
    const float hw = vm.sprite->pixelWidth * vm.scale.x / 2.0f;
    const float hh = vm.sprite->pixelHeight * vm.scale.y / 2.0f;
    float left, right, top, bottom;
    if (vm.anchor() & 1u) {
        left = vm.position.x;
        right = hw + vm.position.x + hw;
    } else {
        left = vm.position.x - hw;
        right = hw + vm.position.x;
    }
    if (vm.anchor() & 2u) {
        top = vm.position.y;
        bottom = hh + vm.position.y + hh;
    } else {
        top = vm.position.y - hh;
        bottom = hh + vm.position.y;
    }
    left += state.offsetX;
    right += state.offsetX;
    top += state.offsetY;
    bottom += state.offsetY;
    if (round) {
        left = lowRound(left), right = lowRound(right), top = lowRound(top), bottom = lowRound(bottom);
    } else {
        left += 0.5f, right += 0.5f, top += 0.5f, bottom += 0.5f;
    }
    emit(sink, vm, state, (left + right) * 0.5f, (top + bottom) * 0.5f, right - left, bottom - top, 0.0f);
}

}  // namespace

void drawVm(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state) {
    if (!drawable(vm)) return;
    if (vm.rotation.z == 0.0f) {
        drawUnrotated(sink, vm, state, true);
        return;
    }
    const float hw = vm.sprite->pixelWidth * vm.scale.x / 2.0f;
    const float hh = vm.sprite->pixelHeight * vm.scale.y / 2.0f;
    float cx = vm.position.x + state.offsetX + 0.5f, cy = vm.position.y + state.offsetY + 0.5f;
    if (vm.anchor() & 1u) cx += hw;
    if (vm.anchor() & 2u) cy += hh;
    emit(sink, vm, state, cx, cy, hw * 2.0f, hh * 2.0f, vm.rotation.z);
}

void drawVmFacingCamera(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, float) {
    if (!drawable(vm)) return;
    drawUnrotated(sink, vm, state, false);
}

}  // namespace touhou::th07
