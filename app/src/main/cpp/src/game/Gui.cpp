#include "game/Gui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "io/Vfs.hpp"

namespace th06 {
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

Gui::Gui(TextureLibrary& textures, TextRenderer* text) : textures_(textures), text_(text) {}

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

    // Stage title and first song from the stage file's header.
    std::vector<std::uint8_t> title, song;
    if (auto header = vfs.read(std::format("data/stage{}.std", stage_))) {
        title = fixedString(*header, 0x10, 128);
        song = fixedString(*header, 0x90, 128);
    }
    anm.setAndExecuteScript(stageName_, kScriptStageName);
    renderText(world, stageName_, title, 15, kStageNameRgb, Align::Centre);
    anm.setAndExecuteScript(songName_, kScriptSongName);
    std::vector<std::uint8_t> note(std::begin(kNoteSign), std::end(kNoteSign));
    note.insert(note.end(), song.begin(), song.end());
    renderText(world, songName_, note, 16, kStageNameRgb, Align::Right);

    bonus_ = fullPower_ = spellBonus_ = Slide{};
    popups_ = {};
    smallPopups_ = {};
    nextPopup_ = nextSmallPopup_ = 0;
    strings_.clear();
}

bool Gui::renderText(World& world, AnmVm& element, std::span<const std::uint8_t> sjis, int fontSize,
                     std::uint32_t rgb, Align align) {
    const AnmLoadedSprite* s = world.anm->sprite(element);
    if (!s || !text_) return false;
    Image* img = textures_.pixels(s->texture);
    if (!img) return false;
    const int w = img->width;
    const float n = static_cast<float>(sjis.size());
    float startX = s->startX;
    if (align == Align::Right) startX += static_cast<float>(w) - n * static_cast<float>(fontSize + 1) / 2.0f;
    else if (align == Align::Centre) startX += static_cast<float>(w) / 2.0f - n * static_cast<float>(fontSize + 1) / 4.0f;
    startX = std::floor(startX);
    Image band(w, 16);
    TextStyle style;
    style.fontSize = fontSize;
    style.rgb = rgb;
    text_->render(sjis, style, startX, band);
    finishTextBand(band);
    const int top = static_cast<int>(s->startY);
    for (int y = 0; y < 16; ++y) {
        if (top + y < 0 || top + y >= img->height) continue;
        std::memcpy(img->pixel(0, top + y), band.pixel(0, y), static_cast<std::size_t>(w) * 4);
    }
    textures_.touch(s->texture);
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
    const auto name = bombNameBytes(t);
    renderText(world, bombName_, name, 15, kBombNameRgb, Align::Left);
    bombNameLength_ = static_cast<float>(name.size()) * 7.5f + 16.0f;
    // (Sound 14 is played by the bomb itself.)
}

void Gui::endBombName() { bombName_.pendingInterrupt = 1; }

void Gui::spellStart(World& world, int portrait, std::span<const std::uint8_t> name) {
    world.playSound(kSoundBombName);
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    anm.setAndExecuteScript(spellPortrait_, kScriptSpellPortrait);
    anm.setActiveSprite(spellPortrait_, 0x4a8 + portrait);
    anm.setAndExecuteScript(spellName_, kScriptSpellName);
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

void Gui::update(World& world) {
    if (!ready_) return;
    AnmManager& anm = *world.anm;
    for (int k = 0; k < kFrontSlots; ++k) {
        if (k == kSlotBossLabel) updateBossHud(world);  // (dialogue is not ported: never active)
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
    hiScore_ = std::max(hiScore_, world.state->displayedScore);

    // The timer tick (the original checks it while drawing; at 60 Hz this is the same).
    const GameState& gs = *world.state;
    if (gs.bossPresent || bossState_ != BossHud::Idle) {
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

    // Stage elements.
    if (stageName_.visible) {
        anm.drawWorld(sink, stageName_, gs.playfieldX, gs.playfieldY);
        const std::uint32_t alpha = stageName_.color >> 24;
        if (stage_ <= 5) queueString(168.0f, 198.0f, std::format("STAGE {}", stage_), withAlpha(0xffff40, alpha));
        else queueString(136.0f, 198.0f, stage_ == 6 ? "FINAL STAGE" : "EXTRA STAGE", withAlpha(0xffff40, alpha));
    }
    if (songName_.visible) anm.drawWorld(sink, songName_, gs.playfieldX, gs.playfieldY);
    anm.drawScreen(sink, bombPortrait_);
    anm.drawScreen(sink, spellPortrait_);
    drawNameWithBar(world, sink, bombName_, bombBar_, bombNameLength_ * 8.0f / 15.0f - 144.0f, bombNameLength_);
    drawNameWithBar(world, sink, spellName_, spellBar_, 128.0f - spellNameLength_ * 8.0f / 15.0f, spellNameLength_);
    if (loadingActive_) anm.drawScreen(sink, loading_);

    drawBossHud(world, sink);
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

void Gui::drawStrings(World& world, SpriteSink& sink) {
    if (!ready_) {
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

}  // namespace th06
