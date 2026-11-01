#include "gfx/AnmManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Log.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

constexpr int kMaxInstructionsPerTick = 4096;  // guards against self-jumping data

struct RawInstr {
    std::int16_t time = 0;
    std::uint8_t opcode = 0;
    std::uint8_t argBytes = 0;
    const std::uint8_t* args = nullptr;
    const std::uint8_t* next = nullptr;
};

bool decode(const std::uint8_t* p, const std::uint8_t* end, RawInstr& out) {
    if (!p || p + 4 > end) return false;
    std::memcpy(&out.time, p, 2);
    out.opcode = p[2];
    out.argBytes = p[3];
    out.args = p + 4;
    out.next = p + 4 + out.argBytes;
    return out.next <= end;
}

std::int32_t argI32(const RawInstr& in, int index) {
    std::int32_t v = 0;
    if (static_cast<int>(in.argBytes) >= 4 * (index + 1)) std::memcpy(&v, in.args + 4 * index, 4);
    return v;
}
float argF32(const RawInstr& in, int index) {
    float v = 0;
    if (static_cast<int>(in.argBytes) >= 4 * (index + 1)) std::memcpy(&v, in.args + 4 * index, 4);
    return v;
}
std::uint16_t argU16At(const RawInstr& in, int byteOffset) {
    std::uint16_t v = 0;
    if (static_cast<int>(in.argBytes) >= byteOffset + 2) std::memcpy(&v, in.args + byteOffset, 2);
    return v;
}

std::uint8_t component(std::uint32_t c, int i) { return static_cast<std::uint8_t>(c >> (8 * i)); }

}  // namespace

void AnmVm::initialize() {
    uvScrollX = uvScrollY = 0.0f;
    scaleInterpFinalY = scaleInterpFinalX = 0.0f;
    angleVel = {};
    rotation = {};
    scaleY = scaleX = 1.0f;
    scaleInterpEndTime = 0;
    alphaInterpEndTime = 0;
    color = 0xffffffffu;
    // flags = Visible | VisibleOverride; every other flag bit cleared
    visible = true;
    visibleOverride = true;
    additive = false;
    colorAdd = false;
    usePosOffset = false;
    zWriteDisable = false;
    stopped = false;
    flip = 0;
    anchor = AnmAnchor::Center;
    moveInterpMode = AnmInterp::Linear;
    autoRotate = 0;
    pendingInterrupt = 0;
    posInterpEndTime = 0;
    timeInScript = 0;
}

AnmManager::AnmManager(TextureLibrary& textures, ZunRng& rng)
    : textures_(textures), rng_(rng), sprites_(kMaxSprites), scripts_(kMaxScripts) {}

bool AnmManager::loadAnm(const Vfs& vfs, int fileSlot, std::string_view path, int offset,
                         std::string* error) {
    auto bytes = vfs.read(path);
    if (!bytes) {
        if (error) *error = std::string(path) + " not found";
        return false;
    }
    auto loaded = th06::loadAnm(vfs, textures_, path, error);  // registers the texture
    if (!loaded) return false;
    return loadAnmFromBytes(std::move(*bytes), loaded->texture, fileSlot, offset, error);
}

bool AnmManager::loadAnmFromBytes(std::vector<std::uint8_t> bytes, std::uint16_t texture, int fileSlot,
                                  int offset, std::string* error) {
    if (fileSlot < 0 || fileSlot >= kMaxFiles) {
        if (error) *error = "ANM file slot out of range";
        return false;
    }
    auto anm = parseAnm(bytes, error);
    if (!anm) return false;

    releaseAnm(fileSlot);
    FileSlot& slot = files_[static_cast<std::size_t>(fileSlot)];
    slot.bytes = std::move(bytes);
    slot.loaded = true;
    slot.texture = texture;
    const std::uint8_t* base = slot.bytes.data();
    const std::uint8_t* end = base + slot.bytes.size();

    const float texW = anm->textureWidth > 0 ? static_cast<float>(anm->textureWidth)
                                             : static_cast<float>(textures_.width(texture));
    const float texH = anm->textureHeight > 0 ? static_cast<float>(anm->textureHeight)
                                              : static_cast<float>(textures_.height(texture));
    int registeredSprites = 0;
    for (const AnmSprite& s : anm->sprites) {
        const std::int64_t index = static_cast<std::int64_t>(s.id) + offset;
        if (index < 0 || index >= kMaxSprites) continue;
        AnmLoadedSprite& out = sprites_[static_cast<std::size_t>(index)];
        out.sourceFile = static_cast<std::int16_t>(fileSlot);
        out.texture = texture;
        out.startX = s.x;
        out.startY = s.y;
        out.endX = s.x + s.w;
        out.endY = s.y + s.h;
        out.textureWidth = texW;
        out.textureHeight = texH;
        out.u0 = out.startX / texW;
        out.u1 = out.endX / texW;
        out.v0 = out.startY / texH;
        out.v1 = out.endY / texH;
        out.widthPx = out.endX - out.startX;
        out.heightPx = out.endY - out.startY;
        ++registeredSprites;
    }
    int registeredScripts = 0;
    for (const AnmScript& sc : anm->scripts) {
        const std::int64_t index = static_cast<std::int64_t>(sc.id) + offset;
        if (index < 0 || index >= kMaxScripts || sc.offset >= slot.bytes.size()) continue;
        scripts_[static_cast<std::size_t>(index)] = ScriptRef{base + sc.offset, end, offset, fileSlot};
        ++registeredScripts;
    }
    log::info("ANM slot {} @0x{:x}: {} sprites, {} scripts", fileSlot, offset, registeredSprites,
              registeredScripts);
    return true;
}

void AnmManager::releaseAnm(int fileSlot) {
    if (fileSlot < 0 || fileSlot >= kMaxFiles) return;
    for (AnmLoadedSprite& s : sprites_) {
        if (s.sourceFile == fileSlot) s = AnmLoadedSprite{};
    }
    for (ScriptRef& r : scripts_) {
        if (r.fileSlot == fileSlot) r = ScriptRef{};
    }
    FileSlot& slot = files_[static_cast<std::size_t>(fileSlot)];
    slot.bytes.clear();
    slot.bytes.shrink_to_fit();
    slot.loaded = false;
    lastSprite_ = -1;
}

bool AnmManager::fileLoaded(int fileSlot) const {
    return fileSlot >= 0 && fileSlot < kMaxFiles && files_[static_cast<std::size_t>(fileSlot)].loaded;
}

bool AnmManager::hasScript(int scriptIndex) const {
    return scriptIndex >= 0 && scriptIndex < kMaxScripts &&
           scripts_[static_cast<std::size_t>(scriptIndex)].begin != nullptr;
}

const AnmLoadedSprite* AnmManager::sprite(int spriteIndex) const {
    if (spriteIndex < 0 || spriteIndex >= kMaxSprites) return nullptr;
    const AnmLoadedSprite& s = sprites_[static_cast<std::size_t>(spriteIndex)];
    return s.sourceFile >= 0 ? &s : nullptr;
}

std::size_t AnmManager::loadedSpriteCount() const {
    return static_cast<std::size_t>(std::count_if(sprites_.begin(), sprites_.end(),
                                                  [](const AnmLoadedSprite& s) { return s.sourceFile >= 0; }));
}

bool AnmManager::setActiveSprite(AnmVm& vm, int spriteIndex) const {
    if (!sprite(spriteIndex)) return false;  // original: ZUN_ERROR, VM keeps its sprite
    vm.activeSpriteIndex = static_cast<std::int16_t>(spriteIndex);
    return true;
}

void AnmManager::initializeAndSetSprite(AnmVm& vm, int spriteIndex) const {
    vm.initialize();
    setActiveSprite(vm, spriteIndex);
}

void AnmManager::setAndExecuteScript(AnmVm& vm, int scriptIndex) {
    vm.anmFileIndex = static_cast<std::int16_t>(scriptIndex);
    vm.flip = 0;
    vm.initialize();
    const ScriptRef* ref = hasScript(scriptIndex) ? &scripts_[static_cast<std::size_t>(scriptIndex)] : nullptr;
    vm.scriptBegin = ref ? ref->begin : nullptr;
    vm.currentInstruction = vm.scriptBegin;
    vm.scriptEnd = ref ? ref->end : nullptr;
    vm.timeInScript = 0;
    vm.visible = false;
    if (vm.scriptBegin) executeScript(vm);
}

int AnmManager::executeScript(AnmVm& vm) {
    if (!vm.currentInstruction) return 1;
    const int spriteOffset = (vm.anmFileIndex >= 0 && vm.anmFileIndex < kMaxScripts)
                                 ? scripts_[static_cast<std::size_t>(vm.anmFileIndex)].spriteOffset
                                 : 0;
    RawInstr in;
    int budget = kMaxInstructionsPerTick;
    bool runInterrupt = vm.pendingInterrupt != 0;

    while (true) {
        if (!runInterrupt) {
            if (--budget < 0) {
                if (runawayWarnings_++ < 4) log::warn("ANM script {} runs away; stopping it this tick", vm.anmFileIndex);
                break;
            }
            if (!decode(vm.currentInstruction, vm.scriptEnd, in)) {
                vm.currentInstruction = nullptr;  // malformed / truncated: treat as end
                return 1;
            }
            if (in.time > vm.timeInScript) break;
        }

        if (!runInterrupt) {
            switch (in.opcode) {
                case anm_op::Delete:
                    vm.visible = false;
                    [[fallthrough]];
                case anm_op::Static:
                    vm.currentInstruction = nullptr;
                    return 1;
                case anm_op::SetSprite:
                    vm.visible = true;
                    setActiveSprite(vm, argI32(in, 0) + spriteOffset);
                    vm.timeOfLastSpriteSet = vm.timeInScript;
                    break;
                case anm_op::SpriteSetRandom: {
                    vm.visible = true;
                    const auto base = static_cast<std::uint32_t>(argI32(in, 0));
                    const auto range = static_cast<std::uint16_t>(static_cast<std::uint32_t>(argI32(in, 1)));
                    setActiveSprite(vm, static_cast<int>(base + rng_.u16InRange(range)) + spriteOffset);
                    vm.timeOfLastSpriteSet = vm.timeInScript;
                    break;
                }
                case anm_op::Scale:
                    vm.scaleX = argF32(in, 0);
                    vm.scaleY = argF32(in, 1);
                    break;
                case anm_op::Alpha:
                    vm.color = (vm.color & 0x00ffffffu) | ((static_cast<std::uint32_t>(argI32(in, 0)) & 0xffu) << 24);
                    break;
                case anm_op::Color:
                    vm.color = colorCombineAlpha(static_cast<std::uint32_t>(argI32(in, 0)), vm.color);
                    break;
                case anm_op::Jump: {
                    const std::int32_t target = argI32(in, 0);
                    const std::uint8_t* dest = vm.scriptBegin + target;
                    RawInstr probe;
                    if (target < 0 || !decode(dest, vm.scriptEnd, probe)) {
                        vm.currentInstruction = nullptr;
                        return 1;
                    }
                    vm.currentInstruction = dest;
                    vm.timeInScript = probe.time;
                    continue;
                }
                case anm_op::ScaleFlipX:
                    vm.flip ^= 1;
                    vm.scaleX *= -1.0f;
                    break;
                case anm_op::PositionMode:
                    vm.usePosOffset = argI32(in, 0) != 0;
                    break;
                case anm_op::ScaleFlipY:
                    vm.flip ^= 2;
                    vm.scaleY *= -1.0f;
                    break;
                case anm_op::Rotation:
                    vm.rotation = {argF32(in, 0), argF32(in, 1), argF32(in, 2)};
                    break;
                case anm_op::RotationSpeed:
                    vm.angleVel = {argF32(in, 0), argF32(in, 1), argF32(in, 2)};
                    break;
                case anm_op::ScaleSpeed:
                    vm.scaleInterpFinalX = argF32(in, 0);
                    vm.scaleInterpFinalY = argF32(in, 1);
                    vm.scaleInterpEndTime = 0;
                    break;
                case anm_op::ScaleInterpLinear:
                    vm.scaleInterpFinalX = argF32(in, 0);
                    vm.scaleInterpFinalY = argF32(in, 1);
                    vm.scaleInterpEndTime = static_cast<std::int16_t>(argU16At(in, 8));
                    vm.scaleInterpTime = 0;
                    vm.scaleInterpInitialX = vm.scaleX;
                    vm.scaleInterpInitialY = vm.scaleY;
                    break;
                case anm_op::AlphaInterpLinear:
                    vm.alphaInterpInitial = vm.color;
                    vm.alphaInterpFinal = (vm.color & 0x00ffffffu) | (static_cast<std::uint32_t>(argI32(in, 0)) << 24);
                    vm.alphaInterpEndTime = static_cast<std::int16_t>(argI32(in, 1));
                    vm.alphaInterpTime = 0;
                    break;
                case anm_op::BlendAdditive:
                    vm.additive = true;
                    break;
                case anm_op::BlendNormal:
                    vm.additive = false;
                    break;
                case anm_op::MovePosition:
                    (vm.usePosOffset ? vm.posOffset : vm.pos) = Vec3f{argF32(in, 0), argF32(in, 1), argF32(in, 2)};
                    break;
                case anm_op::MovePositionInterpDecelerateFast:
                case anm_op::MovePositionInterpDecelerateSlow:
                case anm_op::MovePositionInterpLinear:
                    vm.moveInterpMode = in.opcode == anm_op::MovePositionInterpDecelerateFast ? AnmInterp::DecelerateFast
                                        : in.opcode == anm_op::MovePositionInterpDecelerateSlow ? AnmInterp::DecelerateSlow
                                                                                                : AnmInterp::Linear;
                    vm.posInterpInitial = vm.usePosOffset ? vm.posOffset : vm.pos;
                    vm.posInterpFinal = {argF32(in, 0), argF32(in, 1), argF32(in, 2)};
                    vm.posInterpEndTime = static_cast<std::int16_t>(argI32(in, 3));
                    vm.posInterpTime = 0;
                    break;
                case anm_op::HaltInvisible:
                    vm.visible = false;
                    [[fallthrough]];
                case anm_op::Halt:
                    if (vm.pendingInterrupt == 0) {
                        vm.stopped = true;
                        vm.timeInScript--;
                        goto break_parser;
                    }
                    runInterrupt = true;
                    continue;
                case anm_op::FlagVisible:
                    vm.visible = argI32(in, 0) != 0;
                    break;
                case anm_op::AnchorTopLeft:
                    vm.anchor = AnmAnchor::TopLeft;
                    break;
                case anm_op::SetAutoRotate:
                    vm.autoRotate = static_cast<std::int16_t>(argI32(in, 0));
                    break;
                case anm_op::ScrollX:
                    vm.uvScrollX += argF32(in, 0);
                    if (vm.uvScrollX >= 1.0f) vm.uvScrollX -= 1.0f;
                    else if (vm.uvScrollX < 0.0f) vm.uvScrollX += 1.0f;
                    break;
                case anm_op::ScrollY:
                    vm.uvScrollY += argF32(in, 0);
                    if (vm.uvScrollY >= 1.0f) vm.uvScrollY -= 1.0f;
                    else if (vm.uvScrollY < 0.0f) vm.uvScrollY += 1.0f;
                    break;
                case anm_op::FlagDisableZWrite:
                    vm.zWriteDisable = argI32(in, 0) != 0;
                    break;
                case anm_op::Nop:
                case anm_op::InterruptLabel:
                default:
                    break;
            }
            vm.currentInstruction = in.next;
            continue;
        }

        // Interrupt: find "label <pendingInterrupt>" (or the catch-all label -1) and
        // resume right after it.
        {
            runInterrupt = false;
            const std::uint8_t* p = vm.scriptBegin;
            const std::uint8_t* fallback = nullptr;
            RawInstr scan;
            bool found = false;
            while (decode(p, vm.scriptEnd, scan)) {
                if (scan.opcode == anm_op::InterruptLabel && argI32(scan, 0) == vm.pendingInterrupt) {
                    found = true;
                    break;
                }
                if (scan.opcode == anm_op::Delete || scan.opcode == anm_op::Static) break;
                if (scan.opcode == anm_op::InterruptLabel && argI32(scan, 0) == -1) fallback = p;
                p = scan.next;
            }
            vm.pendingInterrupt = 0;
            vm.stopped = false;
            if (!found) {
                if (!fallback) {
                    vm.timeInScript--;
                    goto break_parser;
                }
                decode(fallback, vm.scriptEnd, scan);
            }
            RawInstr resume;
            if (!decode(scan.next, vm.scriptEnd, resume)) {
                vm.currentInstruction = nullptr;
                return 1;
            }
            vm.currentInstruction = scan.next;
            vm.timeInScript = resume.time;
            vm.visible = true;
        }
    }

break_parser:
    if (vm.angleVel.x != 0.0f) vm.rotation.x = addNormalizeAngle(vm.rotation.x, vm.angleVel.x);
    if (vm.angleVel.y != 0.0f) vm.rotation.y = addNormalizeAngle(vm.rotation.y, vm.angleVel.y);
    if (vm.angleVel.z != 0.0f) vm.rotation.z = addNormalizeAngle(vm.rotation.z, vm.angleVel.z);

    if (vm.scaleInterpEndTime > 0) {
        vm.scaleInterpTime++;
        if (vm.scaleInterpTime >= vm.scaleInterpEndTime) {
            vm.scaleY = vm.scaleInterpFinalY;
            vm.scaleX = vm.scaleInterpFinalX;
            vm.scaleInterpEndTime = 0;
            vm.scaleInterpFinalY = 0.0f;
            vm.scaleInterpFinalX = 0.0f;
        } else {
            const float t = static_cast<float>(vm.scaleInterpTime);
            vm.scaleX = (vm.scaleInterpFinalX - vm.scaleInterpInitialX) * t / vm.scaleInterpEndTime + vm.scaleInterpInitialX;
            vm.scaleY = (vm.scaleInterpFinalY - vm.scaleInterpInitialY) * t / vm.scaleInterpEndTime + vm.scaleInterpInitialY;
        }
        if (vm.flip & 1) vm.scaleX *= -1.0f;
        if (vm.flip & 2) vm.scaleY *= -1.0f;
    } else {
        vm.scaleY = vm.scaleInterpFinalY + vm.scaleY;
        vm.scaleX = vm.scaleInterpFinalX + vm.scaleX;
    }

    if (vm.alphaInterpEndTime > 0) {
        vm.alphaInterpTime++;
        float t = static_cast<float>(vm.alphaInterpTime) / static_cast<float>(vm.alphaInterpEndTime);
        if (t >= 1.0f) t = 1.0f;
        std::uint32_t out = 0;
        for (int i = 0; i < 4; ++i) {
            const float a = component(vm.alphaInterpInitial, i);
            const float b = component(vm.alphaInterpFinal, i);
            int c = static_cast<int>((b - a) * t + a);
            c = std::clamp(c, 0, 255);
            out |= static_cast<std::uint32_t>(c) << (8 * i);
        }
        vm.color = out;
        if (vm.alphaInterpTime >= vm.alphaInterpEndTime) vm.alphaInterpEndTime = 0;
    }

    if (vm.posInterpEndTime != 0) {
        float t = static_cast<float>(vm.posInterpTime) / static_cast<float>(vm.posInterpEndTime);
        if (t >= 1.0f) t = 1.0f;
        if (vm.moveInterpMode == AnmInterp::DecelerateSlow) {
            t = 1.0f - t;
            t *= t;
            t = 1.0f - t;
        } else if (vm.moveInterpMode == AnmInterp::DecelerateFast) {
            t = 1.0f - t;
            t = t * t * t * t;
            t = 1.0f - t;
        }
        const Vec3f v{t * vm.posInterpFinal.x + (1.0f - t) * vm.posInterpInitial.x,
                      t * vm.posInterpFinal.y + (1.0f - t) * vm.posInterpInitial.y,
                      t * vm.posInterpFinal.z + (1.0f - t) * vm.posInterpInitial.z};
        (vm.usePosOffset ? vm.posOffset : vm.pos) = v;
        if (vm.posInterpTime >= vm.posInterpEndTime) vm.posInterpEndTime = 0;
        vm.posInterpTime++;
    }
    vm.timeInScript++;
    return 0;
}

void AnmManager::beginFrame() { lastSprite_ = -1; }

bool AnmManager::uvFor(const AnmVm& vm, const AnmLoadedSprite& s, float& u0, float& v0, float& u1, float& v1) {
    if (vm.activeSpriteIndex != lastSprite_) {
        lastSprite_ = vm.activeSpriteIndex;
        lastU0_ = s.u0 + vm.uvScrollX;
        lastU1_ = s.u1 + vm.uvScrollX;
        lastV0_ = s.v0 + vm.uvScrollY;
        lastV1_ = s.v1 + vm.uvScrollY;
    }
    u0 = lastU0_;
    v0 = lastV0_;
    u1 = lastU1_;
    v1 = lastV1_;
    return true;
}

void AnmManager::drawWorld(SpriteSink& sink, const AnmVm& vm, float screenX, float screenY, bool roundPosition) {
    if (!vm.visible || !vm.visibleOverride) return;
    const AnmLoadedSprite* s = sprite(vm);
    if (!s) return;
    if (vm.color == 0) return;
    const bool rotated = vm.rotation.x != 0.0f || vm.rotation.y != 0.0f || vm.rotation.z != 0.0f;

    // The 3D path draws a 256-unit quad scaled by (sprite size / texture size).
    float w = 256.0f * s->widthPx / s->textureWidth * vm.scaleX;
    float h = 256.0f * s->heightPx / s->textureHeight * vm.scaleY;
    float cx, cy;
    if (!rotated) {  // Draw2: rounded position
        cx = roundPosition ? std::rint(vm.pos.x) : vm.pos.x;
        cy = roundPosition ? std::rint(vm.pos.y) : vm.pos.y;
        if (static_cast<std::uint8_t>(vm.anchor) & 1) cx += s->widthPx * vm.scaleX / 2.0f;
        if (static_cast<std::uint8_t>(vm.anchor) & 2) cy += s->heightPx * vm.scaleY / 2.0f;
    } else {  // Draw3
        cx = (static_cast<std::uint8_t>(vm.anchor) & 1) ? std::fabs(s->widthPx * vm.scaleX / 2.0f) + vm.pos.x : vm.pos.x;
        cy = (static_cast<std::uint8_t>(vm.anchor) & 2) ? vm.pos.y + std::fabs(s->heightPx * vm.scaleY / 2.0f) : vm.pos.y;
        // Rotation about X / Y seen through the orthographic playfield camera foreshortens.
        if (vm.rotation.x != 0.0f) h *= std::cos(vm.rotation.x);
        if (vm.rotation.y != 0.0f) w *= std::cos(vm.rotation.y);
    }
    SpriteInstance inst;
    inst.x = screenX + cx;
    inst.y = screenY + cy;
    inst.w = w;
    inst.h = h;
    uvFor(vm, *s, inst.u0, inst.v0, inst.u1, inst.v1);
    inst.color = argbToRgbaBytes(vm.color);
    inst.angle = -vm.rotation.z;  // D3D rotates counter-clockwise on screen; SpriteInstance clockwise
    sink.draw(s->texture, makeBlend(vm.additive, vm.colorAdd), inst);
}

void AnmManager::drawScreen(SpriteSink& sink, const AnmVm& vm) {
    if (!vm.visible || !vm.visibleOverride) return;
    const AnmLoadedSprite* s = sprite(vm);
    if (!s) return;
    if (vm.color == 0) return;
    SpriteInstance inst;
    const float halfW = s->widthPx * vm.scaleX / 2.0f;
    const float halfH = s->heightPx * vm.scaleY / 2.0f;
    if (vm.rotation.z == 0.0f) {  // DrawNoRotation: rounded vertex positions
        float x0, x1, y0, y1;
        if (!(static_cast<std::uint8_t>(vm.anchor) & 1)) {
            x0 = vm.pos.x - halfW;
            x1 = halfW + vm.pos.x;
        } else {
            x0 = vm.pos.x;
            x1 = halfW + vm.pos.x + halfW;
        }
        if (!(static_cast<std::uint8_t>(vm.anchor) & 2)) {
            y0 = vm.pos.y - halfH;
            y1 = halfH + vm.pos.y;
        } else {
            y0 = vm.pos.y;
            y1 = halfH + vm.pos.y + halfH;
        }
        x0 = std::rint(x0);
        x1 = std::rint(x1);
        y0 = std::rint(y0);
        y1 = std::rint(y1);
        inst.x = (x0 + x1) * 0.5f;
        inst.y = (y0 + y1) * 0.5f;
        inst.w = x1 - x0;
        inst.h = y1 - y0;
        inst.angle = 0.0f;
    } else {  // Draw: rounded centre and half-extent, rotated
        const float cxr = std::rint(halfW);
        const float cyr = std::rint(halfH);
        inst.x = std::rint(vm.pos.x) + ((static_cast<std::uint8_t>(vm.anchor) & 1) ? cxr : 0.0f);
        inst.y = std::rint(vm.pos.y) + ((static_cast<std::uint8_t>(vm.anchor) & 2) ? cyr : 0.0f);
        inst.w = 2.0f * cxr;
        inst.h = 2.0f * cyr;
        inst.angle = -vm.rotation.z;
    }
    uvFor(vm, *s, inst.u0, inst.v0, inst.u1, inst.v1);
    inst.color = argbToRgbaBytes(vm.color);
    sink.draw(s->texture, makeBlend(vm.additive, vm.colorAdd), inst);
}

}  // namespace th06
