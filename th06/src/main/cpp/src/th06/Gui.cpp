#include "th06/Gui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "audio/MusicPlayer.hpp"
#include "th06/EffectManager.hpp"
#include "th06/Resources.hpp"
#include "th06/Translation.hpp"
#include "io/Vfs.hpp"

namespace touhou::th06 {
namespace {

// Sound table indices.
constexpr int kSoundBombName = 14, kSoundTimeout = 29;

// front.anm slot roles.
constexpr int kSlotFrameTile = 6, kSlotTopTile = 7, kSlotBottomTile = 8, kSlotLifeStar = 16, kSlotBombStar = 17,
              kSlotPowerMax = 18, kSlotBossLabel = 19, kSlotBossBar = 21, kSlotPatch = 22;
constexpr int kScriptBossEnter = kAnmOffsetFront + 0x13, kScriptBossLeave = kAnmOffsetFront + 0x14;

// text.anm scripts.
constexpr int kScriptStageName = 0x700, kScriptSongName = 0x701, kScriptBombName = 0x706, kScriptSpellName = 0x707;
constexpr int kScriptBombBar = kAnmOffsetFront + 0x17, kScriptSpellBar = kAnmOffsetFront + 0x18;
constexpr int kScriptBombPortrait = 0x4a1, kScriptSpellPortrait = 0x4a3;

// Rendered-text colours as they appear on screen (the original's Windows text API reads
// its colour constants with red and blue swapped).
constexpr std::uint32_t kStageNameRgb = 0xffffe0, kBombNameRgb = 0xfff0f0, kSpellNameRgb = 0xf0f0ff;

// The bomb names, Shift-JIS.
constexpr std::uint8_t kBombNameReimuA[] = {0x97, 0xec, 0x95, 0x84, 0x81, 0x75, 0x96, 0xb2,
                                            0x91, 0x7a, 0x95, 0x95, 0x88, 0xf3, 0x81, 0x76};
constexpr std::uint8_t kBombNameReimuB[] = {0x96, 0xb2, 0x95, 0x84, 0x81, 0x75, 0x95, 0x95, 0x96, 0x82, 0x90, 0x77, 0x81, 0x76};
constexpr std::uint8_t kBombNameMarisaA[] = {0x96, 0x82, 0x95, 0x84, 0x81, 0x75, 0x83, 0x58, 0x83, 0x5e,
                                             0x81, 0x5b, 0x83, 0x5f, 0x83, 0x58, 0x83, 0x67, 0x83, 0x8c,
                                             0x83, 0x94, 0x83, 0x40, 0x83, 0x8a, 0x83, 0x47, 0x81, 0x76};
constexpr std::uint8_t kBombNameMarisaB[] = {0x97, 0xf6, 0x95, 0x84, 0x81, 0x75, 0x83, 0x7d, 0x83, 0x58, 0x83, 0x5e,
                                             0x81, 0x5b, 0x83, 0x58, 0x83, 0x70, 0x81, 0x5b, 0x83, 0x4e, 0x81, 0x76};
constexpr std::uint8_t kNoteSign[] = {0x81, 0xf4};  // "♪"

// Dialogue: text scripts for the two lines and the two intro (name/title) lines, and the
// speaker colours as they appear on screen.
constexpr int kScriptMsgLine = 0x702, kScriptMsgIntro = 0x704;
constexpr std::uint32_t kMsgColors[4] = {0xfff0e8, 0xf0e8ff, 0x000000, 0x000000};

template <class T>
T readLe(const std::vector<std::uint8_t>& b, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

std::span<const std::uint8_t> bombNameBytes(int shotType) {
    switch (shotType & 3) {
        case 0: return kBombNameReimuA;
        case 1: return kBombNameReimuB;
        case 2: return kBombNameMarisaA;
        default: return kBombNameMarisaB;
    }
}

constexpr const char* kFaceChara[2][3] = {{"data/face00a.anm", "data/face00b.anm", "data/face00c.anm"},
                                          {"data/face01a.anm", "data/face01b.anm", "data/face01c.anm"}};
constexpr const char* kFaceStage[7][3] = {
    {"data/face03a.anm", "data/face03b.anm", nullptr}, {"data/face05a.anm", nullptr, nullptr},
    {"data/face06a.anm", "data/face06b.anm", nullptr}, {"data/face08a.anm", "data/face08b.anm", nullptr},
    {"data/face09a.anm", "data/face09b.anm", nullptr}, {"data/face09b.anm", "data/face10a.anm", "data/face10b.anm"},
    {"data/face08a.anm", "data/face12a.anm", "data/face12b.anm"},
};

// A NUL-terminated string from a fixed field.
std::vector<std::uint8_t> fixedString(const std::vector<std::uint8_t>& bytes, std::size_t at, std::size_t size) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = at; i < at + size && i < bytes.size() && bytes[i] != 0; ++i) out.push_back(bytes[i]);
    return out;
}

// Draws `texW` x `texH` texels from the sprite's top-left corner as a w x h quad whose
// top-left is (x, y), optionally cropped to a clip rectangle.
void drawTexels(SpriteSink& sink, const AnmLoadedSprite& s, float x, float y, float w, float h, float texW, float texH,
                std::uint32_t argb, const float* clip = nullptr) {
    float u0 = s.startX / s.textureWidth, v0 = s.startY / s.textureHeight;
    float u1 = (s.startX + texW) / s.textureWidth, v1 = (s.startY + texH) / s.textureHeight;
    float x0 = std::rint(x), y0 = std::rint(y), x1 = std::rint(x + w), y1 = std::rint(y + h);
    if (clip) {
        const float cx0 = clip[0], cy0 = clip[1], cx1 = clip[0] + clip[2], cy1 = clip[1] + clip[3];
        if (x1 <= cx0 || x0 >= cx1 || y1 <= cy0 || y0 >= cy1) return;
        auto crop = [](float& a0, float& a1, float& t0, float& t1, float lo, float hi) {
            const float span = a1 - a0, tspan = t1 - t0;
            if (a0 < lo) {
                t0 += tspan * (lo - a0) / span;
                a0 = lo;
            }
            if (a1 > hi) {
                t1 -= tspan * (a1 - hi) / span;
                a1 = hi;
            }
        };
        crop(x0, x1, u0, u1, cx0, cx1);
        crop(y0, y1, v0, v1, cy0, cy1);
    }
    SpriteInstance inst;
    inst.x = (x0 + x1) / 2.0f;
    inst.y = (y0 + y1) / 2.0f;
    inst.w = x1 - x0;
    inst.h = y1 - y0;
    inst.u0 = u0;
    inst.v0 = v0;
    inst.u1 = u1;
    inst.v1 = v1;
    inst.color = argbToRgbaBytes(argb);
    sink.draw(s.texture, Blend::Alpha, inst);
}

std::uint32_t withAlpha(std::uint32_t rgb, std::uint32_t alpha) { return (rgb & 0xffffffu) | ((alpha & 0xffu) << 24); }

}  // namespace

Gui::Gui(TextureLibrary& textures, TextRenderer* text) : textures_(&textures), text_(text) {}

bool Gui::loadRun(World& world, const Vfs& vfs, int character) {
    AnmManager& anm = *world.anm;
    std::string err;
    auto load = [&](int slot, const char* path, int offset, bool required) {
        anm.releaseAnm(slot);
        if (anm.loadAnm(vfs, slot, path, offset, &err)) return true;
        if (required) log::error("GUI: {}: {}", path, err);
        else log::warn("GUI: {}: {}", path, err);
        return false;
    };
    if (!anm.fileLoaded(kAnmFileAscii)) load(kAnmFileAscii, "data/ascii.anm", kAnmOffsetAscii, true);
    if (!anm.fileLoaded(kAnmFileText)) load(kAnmFileText, "data/text.anm", kAnmOffsetText, true);
    const bool front = load(kAnmFileFront, "data/front.anm", kAnmOffsetFront, true);
    load(kAnmFileLoading, "data/loading.anm", kAnmOffsetLoading, false);
    const int c = character & 1;
    load(kAnmFileFaceCharaA, kFaceChara[c][0], kAnmOffsetFaceCharaA, false);
    load(kAnmFileFaceCharaB, kFaceChara[c][1], kAnmOffsetFaceCharaB, false);
    load(kAnmFileFaceCharaC, kFaceChara[c][2], kAnmOffsetFaceCharaC, false);
    character_ = character;
    vfs_ = &vfs;
    ready_ = front && anm.fileLoaded(kAnmFileAscii) && anm.fileLoaded(kAnmFileText);
    if (!ready_) return false;
    for (int k = 0; k < kFrontSlots; ++k) {
        front_[static_cast<std::size_t>(k)] = AnmVm{};
        anm.setAndExecuteScript(front_[static_cast<std::size_t>(k)], kAnmOffsetFront + k);
    }
    loadingActive_ = false;
    bossOpacity_ = 0;
    hiScore_ = 0;
    world.state->bossLifeCount = 0;
    world.state->bossSeconds = 0;
    return true;
}

void Gui::stopElement(AnmVm& vm) {
    vm.currentInstruction = nullptr;
    vm.visible = false;
}

void Gui::startStage(World& world, const Vfs& vfs, int stage, bool firstStage) {
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    GameState& gs = *world.state;
    stage_ = std::clamp(stage, 1, 7);
    if (!firstStage && anm.fileLoaded(kAnmFileLoading)) {  // the previous stage's loading screen leaves
        anm.setAndExecuteScript(loading_, kAnmOffsetLoading);
        loading_.pendingInterrupt = 1;
        loadingActive_ = true;
    }
    std::string err;
    const int s = stage_ - 1;
    const int slots[3] = {kAnmFileFaceStageA, kAnmFileFaceStageB, kAnmFileFaceStageC};
    const int offsets[3] = {kAnmOffsetFaceStageA, kAnmOffsetFaceStageB, kAnmOffsetFaceStageC};
    for (int i = 0; i < 3; ++i) {
        anm.releaseAnm(slots[i]);
        if (kFaceStage[s][i] && !anm.loadAnm(vfs, slots[i], kFaceStage[s][i], offsets[i], &err)) {
            log::warn("GUI: {}: {}", kFaceStage[s][i], err);
        }
    }

    gs.bossFillTarget = 0.0f;
    bossState_ = BossHud::Idle;
    bossFill_ = 0.0f;
    anm.setAndExecuteScript(bombPortrait_, kScriptBombPortrait);
    anm.setAndExecuteScript(spellPortrait_, kScriptSpellPortrait);
    anm.setAndExecuteScript(bombName_, kScriptBombName);
    anm.setAndExecuteScript(spellName_, kScriptSpellName);
    anm.setAndExecuteScript(bombBar_, kScriptBombBar);
    anm.setAndExecuteScript(spellBar_, kScriptSpellBar);
    stopElement(bombPortrait_);
    stopElement(spellPortrait_);
    stopElement(bombName_);
    stopElement(spellName_);

    // Stage title and song names from the stage file's header.
    std::vector<std::uint8_t> title;
    songs_ = {};
    songPaths_ = {};
    if (auto header = vfs.read(std::format("data/stage{}.std", stage_))) {
        title = fixedString(*header, 0x10, 128);
        for (int i = 0; i < 4; ++i) {
            songs_[static_cast<std::size_t>(i)] = fixedString(*header, 0x90 + 128 * static_cast<std::size_t>(i), 128);
            const auto path = fixedString(*header, 0x290 + 128 * static_cast<std::size_t>(i), 128);
            songPaths_[static_cast<std::size_t>(i)].assign(path.begin(), path.end());
        }
    }
    // Dialogue scripts: a count, then one offset per script.
    msg_ = Msg{};
    if (auto file = vfs.read(std::format("data/msg{}.dat", stage_))) {
        msg_.file = std::move(*file);
        if (world.translation) world.translation->patchMsg(msg_.file, stage_);
        const auto count = readLe<std::int32_t>(msg_.file, 0);
        for (std::int32_t i = 0; i < count && 4 + 4 * static_cast<std::size_t>(i + 1) <= msg_.file.size(); ++i) {
            msg_.offsets.push_back(readLe<std::uint32_t>(msg_.file, 4 + 4 * static_cast<std::size_t>(i)));
        }
    }
    tallyOn_ = tallyPending_ = false;
    stageEndRequested_ = gameClearRequested_ = false;
    anm.setAndExecuteScript(stageName_, kScriptStageName);
    if (world.translation) {
        if (auto t = world.translation->stageTitle(stage_)) title = utf8GameText(*t);
    }
    renderText(world, stageName_, title, 15, kStageNameRgb, Align::Centre);
    anm.setAndExecuteScript(songName_, kScriptSongName);
    renderText(world, songName_, songCaption(world, 0), 16, kStageNameRgb, Align::Right);

    bonus_ = fullPower_ = spellBonus_ = Slide{};
    popups_ = {};
    smallPopups_ = {};
    nextPopup_ = nextSmallPopup_ = 0;
    strings_.clear();
}

// The music caption ("♪" and the title) of song t (0 the stage theme, 1 the boss theme).
std::vector<std::uint8_t> Gui::songCaption(World& world, int t) const {
    if (world.translation) {
        if (auto title = world.translation->musicTitle(stage_ * 2 + t)) return utf8GameText(world.translation->musicCaption(*title));
    }
    std::vector<std::uint8_t> note(std::begin(kNoteSign), std::end(kNoteSign));
    if (t >= 0 && t < 4) note.insert(note.end(), songs_[static_cast<std::size_t>(t)].begin(), songs_[static_cast<std::size_t>(t)].end());
    return note;
}

bool Gui::renderTextAcross(World& world, AnmVm& left, AnmVm& right, std::span<const std::uint8_t> text, int fontSize,
                           std::uint32_t rgb, std::uint32_t shadowRgb) {
    const AnmLoadedSprite* a = world.anm->sprite(left);
    const AnmLoadedSprite* b = world.anm->sprite(right);
    if (!a || !b || !text_ || !isUtf8Text(text)) return false;
    Image* ia = textures_->pixels(a->texture);
    Image* ib = textures_->pixels(b->texture);
    if (!ia || !ib) return false;
    const int wa = std::max(1, static_cast<int>(a->endX - a->startX)), wb = std::max(1, static_cast<int>(b->endX - b->startX));
    Image band(wa + wb, 16);
    TextStyle style;
    style.fontSize = fontSize;
    style.rgb = rgb;
    style.shadow = shadowRgb;
    const std::string_view utf8(reinterpret_cast<const char*>(text.data()) + 1, text.size() - 1);
    lastTextWidth_ = text_->renderUtf8(utf8, style, 0.0f, static_cast<float>(wa + wb), TextAlign::Left, band);
    finishTextBand(band);
    auto copy = [&](Image& img, const AnmLoadedSprite& s, int from, int width) {
        const int x0 = static_cast<int>(s.startX), top = static_cast<int>(s.startY);
        for (int y = 0; y < 16; ++y) {
            if (top + y < 0 || top + y >= img.height) continue;
            for (int x = 0; x < width && x0 + x < img.width; ++x) std::memcpy(img.pixel(x0 + x, top + y), band.pixel(from + x, y), 4);
        }
        textures_->touch(s.texture);
    };
    copy(*ia, *a, 0, wa);
    copy(*ib, *b, wa, wb);
    left.visible = right.visible = true;
    return true;
}

bool Gui::renderText(World& world, AnmVm& element, std::span<const std::uint8_t> sjis, int fontSize,
                     std::uint32_t rgb, Align align, std::uint32_t shadowRgb) {
    const AnmLoadedSprite* s = world.anm->sprite(element);
    if (!s || !text_) return false;
    Image* img = textures_->pixels(s->texture);
    if (!img) return false;
    const int w = img->width;
    Image band(w, 16);
    TextStyle style;
    style.fontSize = fontSize;
    style.rgb = rgb;
    style.shadow = shadowRgb;
    if (isUtf8Text(sjis)) {  // a translated line: laid out within the sprite's own rectangle
        const std::string_view utf8(reinterpret_cast<const char*>(sjis.data()) + 1, sjis.size() - 1);
        const float width = std::max(1.0f, s->endX - s->startX);
        const TextAlign a = align == Align::Right ? TextAlign::Right : align == Align::Centre ? TextAlign::Centre : TextAlign::Left;
        lastTextWidth_ = text_->renderUtf8(utf8, style, s->startX, width, a, band);
    } else {
        const float n = static_cast<float>(sjis.size());
        float startX = s->startX;
        if (align == Align::Right) startX += static_cast<float>(w) - n * static_cast<float>(fontSize + 1) / 2.0f;
        else if (align == Align::Centre) startX += static_cast<float>(w) / 2.0f - n * static_cast<float>(fontSize + 1) / 4.0f;
        startX = std::floor(startX);
        text_->render(sjis, style, startX, band);
        lastTextWidth_ = n * static_cast<float>(fontSize + 1) / 2.0f;
    }
    finishTextBand(band);
    const int top = static_cast<int>(s->startY);
    for (int y = 0; y < 16; ++y) {
        if (top + y < 0 || top + y >= img->height) continue;
        std::memcpy(img->pixel(0, top + y), band.pixel(0, y), static_cast<std::size_t>(w) * 4);
    }
    textures_->touch(s->texture);
    element.visible = true;
    return true;
}

// ---- events ---------------------------------------------------------------------------

void Gui::popup(const Vec3f& fieldPos, std::int32_t value, std::uint32_t argb, bool small) {
    Popup& p = small ? smallPopups_[static_cast<std::size_t>(nextSmallPopup_)] : popups_[static_cast<std::size_t>(nextPopup_)];
    if (small) nextSmallPopup_ = (nextSmallPopup_ + 1) % kSmallPopups;
    else nextPopup_ = (nextPopup_ + 1) % kPopups;
    p = Popup{};
    p.pos = fieldPos;
    p.color = argb;
    p.inUse = true;
    if (value < 0) {
        p.special = true;
        p.count = 1;
    } else {
        const std::string d = std::to_string(std::min<std::int32_t>(value, 99'999'999));
        std::memcpy(p.digits.data(), d.data(), d.size());
        p.count = static_cast<std::uint8_t>(d.size());
    }
}

void Gui::showBomb(World& world, int shotType) {
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    anm.setAndExecuteScript(bombPortrait_, kScriptBombPortrait);
    const int t = shotType & 3;
    anm.setActiveSprite(bombPortrait_, t == 0 || t == 3 ? 0x4a1 : 0x4a3);
    anm.setAndExecuteScript(bombName_, kScriptBombName);
    static constexpr const char* kBombIds[4] = {"th06 Bomb Reimu A", "th06 Bomb Reimu B", "th06 Bomb Marisa A", "th06 Bomb Marisa B"};
    std::optional<std::string> translated;
    if (world.translation) translated = world.translation->stringDef(kBombIds[t]);
    if (translated) {
        renderText(world, bombName_, utf8GameText(*translated), 15, kBombNameRgb, Align::Left);
        bombNameLength_ = lastTextWidth_ + 16.0f;
    } else {
        const auto name = bombNameBytes(t);
        renderText(world, bombName_, name, 15, kBombNameRgb, Align::Left);
        bombNameLength_ = static_cast<float>(name.size()) * 7.5f + 16.0f;
    }
    // (Sound 14 is played by the bomb itself.)
}

void Gui::endBombName() { bombName_.pendingInterrupt = 1; }

void Gui::spellStart(World& world, int portrait, std::span<const std::uint8_t> name, int spellId) {
    world.playSound(kSoundBombName);
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    anm.setAndExecuteScript(spellPortrait_, kScriptSpellPortrait);
    anm.setActiveSprite(spellPortrait_, 0x4a8 + portrait);
    anm.setAndExecuteScript(spellName_, kScriptSpellName);
    if (world.translation && spellId >= 0) {
        if (auto t = world.translation->spellName(spellId)) {
            renderText(world, spellName_, utf8GameText(*t), 15, kSpellNameRgb, Align::Right);
            spellNameLength_ = lastTextWidth_ + 16.0f;
            return;
        }
    }
    renderText(world, spellName_, name, 15, kSpellNameRgb, Align::Right);
    spellNameLength_ = static_cast<float>(name.size()) * 7.5f + 16.0f;
}

void Gui::spellNameExit() { spellName_.pendingInterrupt = 1; }

void Gui::showBonus(std::int32_t total) {
    bonus_ = Slide{true, 0, total, 416.0f};
}

void Gui::showFullPower() { fullPower_ = Slide{true, 0, 0, 416.0f}; }

void Gui::showSpellBonus(std::int32_t value) { spellBonus_ = Slide{true, 0, value, 0.0f}; }

void Gui::queueString(float x, float y, std::string_view text, std::uint32_t argb, float scale, bool clipped) {
    if (static_cast<int>(strings_.size()) >= kMaxStrings) return;
    strings_.push_back({x, y, std::string(text.substr(0, 63)), argb, scale, clipped});
}

int Gui::activePopups() const {
    int n = 0;
    for (const Popup& p : popups_) n += p.inUse;
    for (const Popup& p : smallPopups_) n += p.inUse;
    return n;
}

// ---- update ---------------------------------------------------------------------------

void Gui::updatePopups() {
    auto step = [](Popup& p) {
        if (!p.inUse) return;
        p.pos.y -= 0.5f;
        if (++p.timer > 60) p.inUse = false;
    };
    for (Popup& p : popups_) step(p);
    for (Popup& p : smallPopups_) step(p);
}

void Gui::updateSlide(Slide& s, int limit) {
    if (!s.shown) return;
    s.x = s.timer < 30 ? 416.0f - 312.0f * static_cast<float>(s.timer) / 30.0f : 104.0f;
    if (s.timer >= limit) s.shown = false;
    ++s.timer;
}

void Gui::updateBossHud(World& world) {
    const GameState& gs = *world.state;
    AnmManager& anm = *world.anm;
    AnmVm& label = front_[kSlotBossLabel];
    if (gs.bossPresent) {
        if (bossState_ == BossHud::Idle) {
            anm.setAndExecuteScript(label, kScriptBossEnter);
            bossState_ = BossHud::Entering;
            bossOpacity_ = 0;
            return;
        }
        if (anm.executeScript(label) != 0) bossState_ = BossHud::Shown;
        if (bossOpacity_ < 252) bossOpacity_ += 4;
        else bossOpacity_ = 255;
    } else if (bossState_ != BossHud::Idle) {
        if (bossState_ == BossHud::Entering || bossState_ == BossHud::Shown) {
            anm.setAndExecuteScript(label, kScriptBossLeave);
            bossState_ = BossHud::Leaving;
        }
        if (bossOpacity_ > 0) bossOpacity_ -= 4;  // from 3 this wraps around, as in the original
        else bossOpacity_ = 0;
        if (anm.executeScript(label) != 0) {
            bossState_ = BossHud::Idle;
            bossFill_ = 0.0f;
            bossOpacity_ = 0;
        }
    }
    if (bossState_ == BossHud::Shown || bossState_ == BossHud::Leaving) {
        const float target = gs.bossFillTarget;
        if (bossFill_ < target) bossFill_ = std::min(bossFill_ + 0.01f, target);
        else if (bossFill_ > target) bossFill_ = std::max(bossFill_ - 0.02f, target);
    }
}

void Gui::update(World& world, const InputState& input) {
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    for (int k = 0; k < kFrontSlots; ++k) {
        if (k == kSlotBossLabel && !msg_.active) updateBossHud(world);  // frozen during dialogue
        else anm.executeScript(front_[static_cast<std::size_t>(k)]);
    }
    anm.executeScript(stageName_);
    anm.executeScript(songName_);
    anm.executeScript(bombPortrait_);
    anm.executeScript(bombName_);
    anm.executeScript(spellPortrait_);
    anm.executeScript(spellName_);
    if (loadingActive_ && anm.executeScript(loading_) != 0) loadingActive_ = false;
    updateSlide(bonus_, 250);
    updateSlide(fullPower_, 180);
    updateSlide(spellBonus_, 280);
    if (tallyPending_) {  // the stage-clear bonus, one update after the results opcode
        tallyPending_ = false;
        tallyTotal_ = static_cast<std::int32_t>(stageClearBonus(*world.state, stage_, world.state->startingLives));
        world.state->score += static_cast<std::uint32_t>(tallyTotal_);
    }
    msgStep(world, input);
    previousShoot_ = input.shoot;
    hiScore_ = std::max(hiScore_, world.state->displayedScore);

    // The timer tick (the original checks it while drawing; at 60 Hz this is the same).
    const GameState& gs = *world.state;
    if (!msg_.active && (gs.bossPresent || bossState_ != BossHud::Idle)) {
        const std::int32_t s = gs.bossSeconds;
        if (std::min(s, 99) < 10 && s != lastTimerSeen_) world.playSound(kSoundTimeout);
        lastTimerSeen_ = s;
    }
}

// ---- drawing --------------------------------------------------------------------------

void Gui::drawPopups(World& world, SpriteSink& sink) {
    if (!ready_) return;
    const GameState& gs = *world.state;
    const float clip[4] = {gs.playfieldX, gs.playfieldY, gs.playfieldW, gs.playfieldH};
    auto draw = [&](const Popup& p) {
        if (!p.inUse) return;
        float x = gs.playfieldX + p.pos.x - 4.0f * static_cast<float>(p.count);
        const float y = gs.playfieldY + p.pos.y;
        if (p.special) {
            if (const AnmLoadedSprite* s = world.anm->sprite(kAnmOffsetAscii + 10)) {
                drawTexels(sink, *s, x, y, 48.0f, 8.0f, 48.0f, 8.0f, p.color, clip);
            }
            return;
        }
        for (int i = 0; i < p.count; ++i, x += 8.0f) {
            if (const AnmLoadedSprite* s = world.anm->sprite(kAnmOffsetAscii + (p.digits[static_cast<std::size_t>(i)] - '0'))) {
                drawTexels(sink, *s, x, y, 8.0f, 8.0f, 8.0f, 8.0f, p.color, clip);
            }
        }
    };
    for (const Popup& p : popups_) draw(p);
    for (const Popup& p : smallPopups_) draw(p);
}

void Gui::drawNameWithBar(World& world, SpriteSink& sink, AnmVm& name, AnmVm& bar, float barDx, float length) {
    if (!name.visible) return;
    bar.pos = {name.pos.x + barDx, name.pos.y, name.pos.z};
    bar.scaleX = length / 14.0f;
    world.anm->drawScreen(sink, bar);
    world.anm->drawScreen(sink, name);
}

void Gui::draw(World& world, SpriteSink& sink, const SpriteRegion& white, float fps) {
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    const GameState& gs = *world.state;

    if (tallyOn_) drawTally(world);
    drawDialogue(world, sink, white);

    // Stage elements.
    if (stageName_.visible) {
        const std::uint32_t alpha = stageName_.color >> 24;
        if (demo_) {  // the attract demo: only the caption, no stage or song name
            queueString(136.0f, 198.0f, " DEMO PLAY", withAlpha(0xffff40, alpha));
        } else {
            anm.drawWorld(sink, stageName_, gs.playfieldX, gs.playfieldY);
            if (stage_ <= 5) queueString(168.0f, 198.0f, std::format("STAGE {}", stage_), withAlpha(0xffff40, alpha));
            else queueString(136.0f, 198.0f, stage_ == 6 ? "FINAL STAGE" : "EXTRA STAGE", withAlpha(0xffff40, alpha));
        }
    }
    if (songName_.visible && !demo_) anm.drawWorld(sink, songName_, gs.playfieldX, gs.playfieldY);
    anm.drawScreen(sink, bombPortrait_);
    anm.drawScreen(sink, spellPortrait_);
    drawNameWithBar(world, sink, bombName_, bombBar_, bombNameLength_ * 8.0f / 15.0f - 144.0f, bombNameLength_);
    drawNameWithBar(world, sink, spellName_, spellBar_, 128.0f - spellNameLength_ * 8.0f / 15.0f, spellNameLength_);
    if (loadingActive_) anm.drawScreen(sink, loading_);

    if (!msg_.active) drawBossHud(world, sink);
    drawPanel(world, sink, white, fps);

    // Sliding messages (clipped to the playfield).
    if (bonus_.shown) queueString(bonus_.x, 32.0f, std::format("BONUS {:>8}", bonus_.value), 0xffffff80u, 1.0f, true);
    if (fullPower_.shown) queueString(fullPower_.x, 232.0f, "Full Power Mode!!", 0xffc0b0ffu, 1.0f, true);
    if (spellBonus_.shown) {
        queueString(88.0f, 80.0f, "Spell Card Bonus!", 0xffff0000u, 1.0f, true);
        const std::string v = std::format("+{}", spellBonus_.value);
        queueString((384.0f - 32.0f * static_cast<float>(v.size())) / 2.0f + 32.0f, 96.0f, v, 0xffff8080u, 2.0f, true);
    }
}

void Gui::drawBossHud(World& world, SpriteSink& sink) {
    const GameState& gs = *world.state;
    if (!gs.bossPresent && bossState_ == BossHud::Idle) return;
    AnmManager& anm = *world.anm;
    anm.drawScreen(sink, front_[kSlotBossLabel]);
    AnmVm bar = front_[kSlotBossBar];
    bar.anchor = AnmAnchor::TopLeft;
    bar.pos = {96.0f, 24.0f, 0.0f};
    bar.scaleX = bossFill_ * 288.0f / 14.0f;
    anm.drawScreen(sink, bar);
    const std::uint32_t alpha = bossOpacity_ & 0xffu;
    queueString(80.0f, 16.0f, std::format("{}", gs.bossLifeCount), withAlpha(0xffff80, alpha));
    const std::int32_t s = gs.bossSeconds;
    const std::uint32_t rgb = s >= 20 ? 0xa0d0ff : s >= 10 ? 0xa080ff : s >= 5 ? 0xe080c0 : 0xff4040;
    queueString(384.0f, 16.0f, std::format("{:02d}", std::min(s, 99)), withAlpha(rgb, alpha));
}

void Gui::drawPanel(World& world, SpriteSink& sink, const SpriteRegion& white, float fps) {
    AnmManager& anm = *world.anm;
    const GameState& gs = *world.state;
    auto at = [&](int slot, float x, float y) {
        AnmVm& vm = front_[static_cast<std::size_t>(slot)];
        vm.pos = {x, y, 0.49f};
        anm.drawScreen(sink, vm);
    };
    // Frame tiles, borders, decorations and labels (redrawn every frame in the port).
    for (int y = 0; y <= 448; y += 32) at(kSlotFrameTile, 0.0f, static_cast<float>(y));
    for (int x = 416; x <= 608; x += 32) {
        for (int y = 0; y <= 448; y += 32) at(kSlotFrameTile, static_cast<float>(x), static_cast<float>(y));
    }
    for (int x = 32; x <= 384; x += 32) {
        at(kSlotTopTile, static_cast<float>(x), 0.0f);
        at(kSlotBottomTile, static_cast<float>(x), 464.0f);
    }
    for (int slot : {5, 0, 1, 3, 4, 2}) anm.drawScreen(sink, front_[static_cast<std::size_t>(slot)]);
    for (int slot = 9; slot <= 15; ++slot) anm.drawScreen(sink, front_[static_cast<std::size_t>(slot)]);
    for (const auto& [x, y] : std::initializer_list<std::pair<float, float>>{
             {496, 58}, {496, 82}, {496, 122}, {496, 146}, {496, 186}, {496, 206}, {496, 226}, {488, 464}, {0, 464}}) {
        at(kSlotPatch, x, y);
    }
    // Values.
    for (int i = 0; i < std::min(gs.livesRemaining, 8); ++i) at(kSlotLifeStar, 496.0f + 16.0f * static_cast<float>(i), 122.0f);
    for (int i = 0; i < std::min(gs.bombs, 8); ++i) at(kSlotBombStar, 496.0f + 16.0f * static_cast<float>(i), 146.0f);
    if (gs.power > 0) {  // a bar whose alpha fades from E0 to 80 across its length
        const int len = gs.power;
        for (int x = 0; x < len; x += 4) {
            const int w = std::min(4, len - x);
            const float t = (static_cast<float>(x) + static_cast<float>(w) / 2.0f) / static_cast<float>(len);
            const auto a = static_cast<std::uint8_t>(0xe0 - (0xe0 - 0x80) * t);
            sink.fillRect(white, 496.0f + static_cast<float>(x), 186.0f, static_cast<float>(w), 16.0f, rgba(0xe0, 0xe0, 0xff, a));
        }
    }
    if (gs.power >= GameState::kMaxPower) at(kSlotPowerMax, 496.0f, 186.0f);
    else queueString(496.0f, 186.0f, std::format("{}", gs.power), 0xffffffffu);
    queueString(496.0f, 82.0f, std::format("{:09d}", gs.displayedScore), 0xffffffffu);
    queueString(496.0f, 58.0f, std::format("{:09d}", hiScore_), 0xffffffffu);
    queueString(496.0f, 206.0f, std::format("{}", gs.grazes), 0xffffffffu);
    queueString(496.0f, 226.0f, std::format("{}", gs.stagePointItems), 0xffffffffu);
    queueString(512.0f, 464.0f, std::format("{:.2f}fps", fps), 0xffffffffu);
}

// ---- dialogue -------------------------------------------------------------------------

void Gui::msgRead(World& world, int n) {
    const GameState& gs = *world.state;
    if (gs.difficulty == Easy && stage_ == 5 && n == 1) n = 3;
    const int script = (gs.shotType / 2) * 10 + n;
    if (script < 0 || script >= static_cast<int>(msg_.offsets.size())) return;
    std::vector<std::uint8_t> file = std::move(msg_.file);
    std::vector<std::uint32_t> offsets = std::move(msg_.offsets);
    msg_ = Msg{};
    msg_.file = std::move(file);
    msg_.offsets = std::move(offsets);
    msg_.active = true;
    msg_.script = script;
    msg_.pc = msg_.offsets[static_cast<std::size_t>(script)];
    // Stage 6 and Extra swap in their final-boss art when the first dialogue starts.
    if (n == 0 && vfs_ && (stage_ == 6 || stage_ == 7)) {
        std::string err;
        AnmManager& anm = *world.anm;
        anm.releaseAnm(kAnmFileEffects);
        if (!anm.loadAnm(*vfs_, kAnmFileEffects, stage_ == 6 ? "data/eff06.anm" : "data/eff07.anm", kAnmOffsetEffects, &err)) {
            log::warn("GUI: {}", err);
        }
        if (stage_ == 7) {
            anm.releaseAnm(kAnmFileFaceStageA);
            if (!anm.loadAnm(*vfs_, kAnmFileFaceStageA, "data/face12c.anm", kAnmOffsetFaceStageA, &err)) log::warn("GUI: {}", err);
        }
    }
}

void Gui::msgStep(World& world, const InputState& input) {
    if (!msg_.active) return;
    AnmManager& anm = *world.anm;
    GameState& gs = *world.state;
    const std::vector<std::uint8_t>& f = msg_.file;
    const bool skipHeld = input.skip || input.bomb;  // bombs are off during dialogue: BOMB doubles as skip
    const bool shootPressed = input.shoot && !previousShoot_;
    if (msg_.resume > 0) --msg_.resume;
    if (msg_.skippable && skipHeld && msg_.pc + 4 <= f.size()) msg_.timer = readLe<std::uint16_t>(f, msg_.pc);

    bool advanceTimer = true;
    while (!msg_.halted) {
        if (msg_.pc + 4 > f.size()) {  // ran off the file: treat as the end
            msg_.active = false;
            return;
        }
        const auto time = readLe<std::uint16_t>(f, msg_.pc);
        if (msg_.timer < time) break;
        const std::uint8_t op = f[msg_.pc + 2];
        const std::uint8_t argSize = f[msg_.pc + 3];
        const std::size_t a = msg_.pc + 4;
        auto text = [&](std::size_t from) {
            std::vector<std::uint8_t> t;
            for (std::size_t i = from; i < a + argSize && i < f.size() && f[i] != 0; ++i) t.push_back(f[i]);
            return t;
        };
        bool stop = false;
        switch (op) {
            case 0:  // end: everything disappears at once
                msg_.active = false;
                return;
            case 1: {
                const auto p = readLe<std::int16_t>(f, a), n = readLe<std::int16_t>(f, a + 2);
                anm.setAndExecuteScript(msg_.portraits[p ? 1 : 0], (p ? kAnmOffsetFaceCharaB : kAnmOffsetFaceCharaA) + n);
                break;
            }
            case 2: {
                const auto p = readLe<std::int16_t>(f, a), n = readLe<std::int16_t>(f, a + 2);
                anm.setActiveSprite(msg_.portraits[p ? 1 : 0], (p ? kAnmOffsetFaceStageA : kAnmOffsetFaceCharaA) + n);
                break;
            }
            case 3: {
                const auto c = readLe<std::int16_t>(f, a) & 3, line = readLe<std::int16_t>(f, a + 2) & 1;
                if (line == 0 && msg_.line1Used) {
                    const std::uint8_t space[] = {' '};
                    renderText(world, msg_.lines[1], space, msg_.fontSize, kMsgColors[c], Align::Left);
                }
                anm.setAndExecuteScript(msg_.lines[static_cast<std::size_t>(line)], kScriptMsgLine + line);
                renderText(world, msg_.lines[static_cast<std::size_t>(line)], text(a + 4), msg_.fontSize, kMsgColors[c], Align::Left);
                msg_.framesWaited = 0;
                if (line == 1) msg_.line1Used = true;
                break;
            }
            case 4: {
                const auto frames = readLe<std::int32_t>(f, a);
                if ((msg_.skippable && skipHeld) || (shootPressed && msg_.framesWaited >= 8) || msg_.framesWaited >= frames) break;
                ++msg_.framesWaited;
                stop = true;
                advanceTimer = false;
                break;
            }
            case 5: {
                const auto target = readLe<std::int16_t>(f, a);
                const auto k = static_cast<std::int16_t>(readLe<std::uint8_t>(f, a + 2));
                if (target == 0 || target == 1) msg_.portraits[static_cast<std::size_t>(target)].pendingInterrupt = k;
                else if (target == 2 || target == 3) msg_.lines[static_cast<std::size_t>(target - 2)].pendingInterrupt = k;
                break;
            }
            case 6: ++msg_.resume; break;
            case 7: {
                const auto t = readLe<std::int32_t>(f, a);
                anm.setAndExecuteScript(songName_, kScriptSongName);
                renderText(world, songName_, songCaption(world, t), 16, kStageNameRgb, Align::Right);
                if (world.music) world.music->play(songPath(t));  // the boss theme
                break;
            }
            case 8: {
                const auto c = readLe<std::int16_t>(f, a) & 3, line = readLe<std::int16_t>(f, a + 2) & 1;
                anm.setAndExecuteScript(msg_.intro[static_cast<std::size_t>(line)], kScriptMsgIntro + line);
                renderText(world, msg_.intro[static_cast<std::size_t>(line)], text(a + 4), 15, kMsgColors[c], Align::Right);
                msg_.framesWaited = 0;
                break;
            }
            case 9:  // stage results
                tallyOn_ = true;
                tallyPending_ = true;
                if (stage_ <= 5) {
                    if (anm.fileLoaded(kAnmFileLoading)) {
                        anm.setAndExecuteScript(loading_, kAnmOffsetLoading);
                        loadingActive_ = true;
                    }
                } else {
                    gs.extendIndex = -1;
                }
                break;
            case 10:  // wait here until another script starts
                msg_.halted = true;
                advanceTimer = false;
                stop = true;
                break;
            case 11:  // stage end
                gs.displayedScore = gs.score;
                if (stage_ <= 4 || (stage_ == 5 && gs.difficulty != Easy)) stageEndRequested_ = true;
                else gameClearRequested_ = true;  // stage 6, Easy's stage 5, or the Extra stage
                msg_.halted = true;
                advanceTimer = false;
                stop = true;
                break;
            case 12:  // after the boss: the music fades over 4 seconds
                if (world.music) world.music->fadeOut(4.0f);
                break;
            case 13: msg_.skippable = readLe<std::int32_t>(f, a) != 0; break;
            default: break;
        }
        if (stop) break;
        msg_.pc += 4 + static_cast<std::uint32_t>(argSize);
    }
    if (msg_.halted) advanceTimer = false;
    if (advanceTimer) ++msg_.timer;
    for (AnmVm& vm : msg_.portraits) anm.executeScript(vm);
    for (AnmVm& vm : msg_.lines) anm.executeScript(vm);
    for (AnmVm& vm : msg_.intro) anm.executeScript(vm);
    if (msg_.timer < 60 && msg_.skippable && skipHeld) msg_.timer = 60;
}

void Gui::drawDialogue(World& world, SpriteSink& sink, const SpriteRegion& white) {
    if (!msg_.active || (stage_ == 6 && (msg_.script == 1 || msg_.script == 11))) return;
    AnmManager& anm = *world.anm;
    const GameState& gs = *world.state;
    for (const AnmVm& vm : msg_.portraits) anm.drawScreen(sink, vm);
    // The box: opens over the first second, darker at the top.
    const float x0 = gs.playfieldX + (gs.playfieldW - 256.0f) / 2.0f - 16.0f;
    const float h = msg_.timer < 60 ? static_cast<float>(msg_.timer) * 48.0f / 60.0f : 48.0f;
    for (float y = 0.0f; y < h; y += 4.0f) {
        const float sliceH = std::min(4.0f, h - y);
        const float t = h > 0.0f ? (y + sliceH / 2.0f) / h : 0.0f;
        const auto a = static_cast<std::uint8_t>(0xd0 - (0xd0 - 0x90) * t);
        sink.fillRect(white, x0, 384.0f + y, 288.0f, sliceH, rgba(0, 0, 0, a));
    }
    for (const AnmVm& vm : msg_.lines) anm.drawScreen(sink, vm);
    for (const AnmVm& vm : msg_.intro) anm.drawScreen(sink, vm);
}

void Gui::drawTally(World& world) {
    const GameState& gs = *world.state;
    const bool last = stage_ >= 6;
    const float x = 74.0f;
    queueString(x, 128.0f, last ? "All Clear!" : "Stage Clear", 0xffffff40u);
    queueString(x, 160.0f, std::format("Stage * 1000 = {:>5}", stage_ * 1000), 0xffffffffu);
    queueString(x, 176.0f, std::format("Power *  100 = {:>5}", gs.power * 100), 0xffe0e0ffu);
    queueString(x, 192.0f, std::format("Graze *   10 = {:>5}", gs.grazes * 10), 0xffd0d0ffu);
    queueString(x, 208.0f, std::format("    * Point Item {:>3}", gs.stagePointItems), 0xffff8080u);
    if (last) {
        queueString(x, 224.0f, std::format("Player    = {:>8}", gs.livesRemaining * 3'000'000), 0xffffff80u);
        queueString(x, 240.0f, std::format("Bomb      = {:>8}", gs.bombs * 1'000'000), 0xffffff80u);
    }
    static constexpr const char* kRanks[5] = {"Easy Rank      * 0.5", "Normal Rank    * 1.0", "Hard Rank      * 1.2",
                                              "Lunatic Rank   * 1.5", "Extra Rank     * 2.0"};
    queueString(x, last ? 272.0f : 240.0f, kRanks[std::clamp(gs.difficulty, 0, 4)], 0xffff8080u);
    // The starting-lives penalty (applied in every mode, shown only in the main game).
    float totalY = last ? 288.0f : 256.0f;
    if (gs.difficulty != Extra && !gs.practice && (gs.startingLives == 3 || gs.startingLives == 4)) {
        queueString(x, totalY, gs.startingLives == 3 ? "Player Penalty * 0.5" : "Player Penalty * 0.2", 0xffff8080u);
        totalY += 16.0f;
    }
    queueString(x, totalY, std::format("Total     = {:>8}", tallyTotal_), 0xffffffffu);
}

void Gui::drawStrings(World& world, SpriteSink& sink) {
    if (!world.anm->fileLoaded(kAnmFileAscii)) {  // (the title draws text before any game)
        strings_.clear();
        return;
    }
    const GameState& gs = *world.state;
    const float clip[4] = {gs.playfieldX, gs.playfieldY, gs.playfieldW, gs.playfieldH};
    for (const QueuedString& q : strings_) {
        float x = q.x, y = q.y;
        for (const char ch : q.text) {
            if (ch == '\n') {
                x = q.x;
                y += 16.0f * q.scale;
                continue;
            }
            if (ch != ' ') {
                if (const AnmLoadedSprite* s = world.anm->sprite(kAnmOffsetAscii + static_cast<unsigned char>(ch) - 21)) {
                    drawTexels(sink, *s, x, y, s->widthPx * q.scale, s->heightPx * q.scale, s->widthPx, s->heightPx, q.color,
                               q.clipped ? clip : nullptr);
                }
            }
            x += 14.0f * q.scale;
        }
    }
    strings_.clear();
}

}  // namespace touhou::th06
