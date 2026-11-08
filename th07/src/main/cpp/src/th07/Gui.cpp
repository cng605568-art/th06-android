#include "th07/Gui.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextRenderer.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"
#include "th07/Bullets.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {

constexpr int kFront = 0x600, kText = 0x700, kFace = 0x4A0, kStageFace = 0x4AD, kStageText = 0x800, kLoading = 0x61E;
constexpr int kCapture = 0x724;

std::uint32_t colorRefToRgb(std::uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

std::int16_t rd16(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int16_t v;
    std::memcpy(&v, b.data() + at, 2);
    return v;
}
std::int32_t rd32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

void disable(AnmVm& vm) {
    vm.current = nullptr;
    vm.set(AnmVm::Visible, false);
}

std::uint32_t argbWithAlpha(std::uint32_t rgb, std::uint32_t a) { return (rgb & 0x00FFFFFFu) | (a << 24); }

}  // namespace

// ---- set-up -------------------------------------------------------------------------------

void Gui::startVm(AnmVm& vm, int script) {
    vm = AnmVm{};
    vm.startFresh(*world_->anm, *world_->rng, script);
}

bool Gui::stageStart(const World& w, const Resources& res, bool firstStage, std::string* error) {
    world_ = &w;
    res_ = res;
    AnmLibrary& lib = *w.anm;
    const GameState& g = *w.state;
    stage_ = g.stage;
    frame_ = 0;
    auto load = [&](const std::string& path, int slot, int offset) { return lib.load(*res.vfs, *res.textures, path, slot, offset, error); };
    if (!lib.loaded(0) && !load("data/text.anm", 0, kText)) return false;
    if (!lib.loaded(1) && !load("data/ascii.anm", 1, 0x000)) return false;
    if (!lib.loaded(4) && !load("data/capture.anm", 4, kCapture)) return false;
    static constexpr const char* kFaces[3] = {"data/face_rm00.anm", "data/face_mr00.anm", "data/face_sk00.anm"};
    static constexpr const char* kLoadings[3] = {"data/loading.anm", "data/loading2.anm", "data/loading3.anm"};
    if (firstStage || !frontLoaded_) {
        if (!lib.load(*res.vfs, *res.textures, "data/front.anm", 21, kFront, error, true)) return false;
        if (!load(kFaces[std::clamp(g.character(), 0, 2)], 25, kFace)) return false;
        if (!load(kLoadings[std::clamp(g.character(), 0, 2)], 23, kLoading)) return false;
        frontLoaded_ = true;
        for (int k = 0; k < 33; ++k) startVm(front_[static_cast<std::size_t>(k)], kFront + k);
        startVm(gaugeVm_, 4);
        gaugeState_ = 0;
        startVm(cherryDigitVm_, 3);
        startVm(borderVm_, 5);
        for (AnmVm& m : markerVms_) {
            startVm(m, 6);
            m.position = {-999.0f, -999.0f, 0.0f};
        }
    }
    stageClear_ = Element{};
    showLoading_ = false;
    if (stage_ < 1 || stage_ > 8) {
        if (error) *error = "no GUI for this stage";
        return false;
    }
    if (!load(std::format("data/face_0{}_00.anm", stage_), 28, kStageFace)) return false;
    if (!load(std::format("data/std{}txt.anm", stage_), 24, kStageText)) return false;
    msg_.clear();
    msgScripts_.clear();
    if (auto bytes = res.vfs->read(std::format("data/msg{}.dat", stage_))) {
        msg_ = std::move(*bytes);
        if (msg_.size() >= 4) {
            const std::int32_t count = rd32(msg_, 0);
            for (std::int32_t i = 0; i < count && 4 + 4 * static_cast<std::size_t>(i) + 4 <= msg_.size(); ++i) {
                msgScripts_.push_back(static_cast<std::uint32_t>(rd32(msg_, 4 + 4 * static_cast<std::size_t>(i))));
            }
        }
    } else {
        log::warn("data/msg{}.dat is missing: no dialogue", stage_);
    }
    bossState_ = 0;
    bossAlpha_ = 0;
    bossFill_ = 0;
    lastSeconds_ = -1;
    startVm(bombPortrait_.vm, kFace + 1);
    startVm(enemyPortrait_.vm, kFace + 3);
    startVm(bombName_.vm, kText + 4);
    startVm(enemyName_.vm, kText + 5);
    for (int i = 0; i < 5; ++i) {
        AnmVm& vm = stageText_[static_cast<std::size_t>(i)];
        startVm(vm, kStageText + i);
        vm.baseSprite = vm.activeSprite;
        stageTextUsed_[static_cast<std::size_t>(i)] = true;
    }
    startVm(bombNameBg_, 1);
    startVm(enemyNameBg_, 0);
    startVm(captureIndicator_, 2);
    startVm(digitVm_, 3);
    for (Element* e : {&bombPortrait_, &bombDecorL_, &bombDecorR_, &bombName_, &enemyPortrait_, &enemyDecorU_, &enemyDecorD_, &enemyName_}) {
        disable(e->vm);
    }
    bombName_.vm.fontWidth = bombName_.vm.fontHeight = 15;
    enemyName_.vm.fontWidth = enemyName_.vm.fontHeight = 15;
    msgIndex_ = -1;
    clearState_ = 0;
    bonus_.shown = status_.shown = spellBonus_.shown = false;
    nextStage_ = false;
    gameFinished_ = false;
    for (Popup& p : popups_) p.live = false;
    text_.clear();
    // The ASCII manager re-sends the gauge's fade state at every stage start; on the first
    // stage of a game the fade-in (interrupt 1, which also slides it into place) follows.
    gaugeVm_.interrupt(gaugeState_ == 0 ? 3 : 2);
    if (firstStage) gaugeVm_.interrupt(1);
    for (int b = 0; b < 3; ++b) markerVms_[static_cast<std::size_t>(b)].interrupt(2);
    copyBossName(2 * (stage_ - 1));  // the mid-boss
    return true;
}

// The boss name label shows front sprite 0x609, whose texels are overwritten with the
// current boss's name sprite (0x60E + k, in front.anm's second texture).
void Gui::copyBossName(int k) {
    if (!world_ || !res_.textures) return;
    const AnmSprite* name = world_->anm->sprite(kFront + 14 + k);
    const AnmSprite* frame = world_->anm->sprite(kFront + 9);
    if (!name || !frame) return;
    Image* src = res_.textures->pixels(name->texture);
    Image* dst = res_.textures->pixels(frame->texture);
    if (!src || !dst) return;
    const int sx0 = static_cast<int>(name->startX), sy0 = static_cast<int>(name->startY);
    const int sw = static_cast<int>(name->endX) - sx0, sh = static_cast<int>(name->endY) - sy0;
    const int dx0 = static_cast<int>(frame->startX), dy0 = static_cast<int>(frame->startY);
    const int dw = static_cast<int>(frame->endX) - dx0, dh = static_cast<int>(frame->endY) - dy0;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const int px = sx0 + x * sw / dw, py = sy0 + y * sh / dh;  // a scaled copy if the sizes differ
            if (px < 0 || py < 0 || px >= src->width || py >= src->height || dx0 + x >= dst->width || dy0 + y >= dst->height) continue;
            std::memcpy(dst->pixel(dx0 + x, dy0 + y), src->pixel(px, py), 4);
        }
    }
    res_.textures->touch(frame->texture);
}

void Gui::stageEnd(const World& w) {
    for (int slot : {24, 28}) w.anm->release(slot);
    msg_.clear();
    msgScripts_.clear();
}

void Gui::gameEnd(const World& w) {
    stageEnd(w);
    for (int slot : {21, 23, 25}) w.anm->release(slot);
    frontLoaded_ = false;
    for (Popup& p : popups_) p.live = false;
    text_.clear();
}

// ---- the ASCII manager (logic order 1) -------------------------------------------------------

void Gui::asciiUpdate(const World& w) {
    for (Popup& p : popups_) {
        if (!p.live) continue;
        p.pos.y -= 0.5f * g_timing.speed;
        p.timer.tick();
        if (p.timer.current > 60) p.live = false;
    }
    const Player* pl = w.player;
    if (pl) {
        const Vec3f& pp = pl->position();
        if (gaugeState_ == 0 && pp.y >= 400.0f && pp.x < 160.0f) {
            gaugeVm_.interrupt(2);
            gaugeState_ = 1;
        } else if (gaugeState_ == 1 && ((pp.y >= 400.0f && pp.x > 160.0f) || pp.y < 400.0f)) {
            gaugeVm_.interrupt(3);
            gaugeState_ = 0;
        }
    }
    gaugeVm_.tick(*w.anm, *w.rng);
    cherryDigitVm_.tick(*w.anm, *w.rng);
    for (int b = 0; b < 4; ++b) {
        AnmVm& m = markerVms_[static_cast<std::size_t>(b)];
        m.position = {w.state->boss.markerX[static_cast<std::size_t>(b)], 472.0f, 0.0f};
        m.tick(*w.anm, *w.rng);
    }
    borderVm_.tick(*w.anm, *w.rng);
}

void Gui::score(const Vec3f& pos, std::uint32_t value, std::uint32_t color, bool small) {
    (void)small;
    Popup& p = popups_[static_cast<std::size_t>(popupCursor_)];
    popupCursor_ = (popupCursor_ + 1) % 720;
    p.live = true;
    p.count = 0;
    std::uint32_t v = value;
    do {
        p.digits[static_cast<std::size_t>(p.count++)] = static_cast<std::uint8_t>(v % 10);
        v /= 10;
    } while (v != 0 && p.count < 8);
    p.color = color;
    p.timer.assign(0);
    p.pos = {pos.x + kPlayfieldX, pos.y + kPlayfieldY, 0.0f};
}

void Gui::powerUp(const Vec3f& pos) {
    Popup& p = popups_[static_cast<std::size_t>(popupCursor_)];
    popupCursor_ = (popupCursor_ + 1) % 720;
    p.live = true;
    p.count = 1;
    p.digits[0] = 10;  // the special glyph
    p.color = 0xFFFFC0A0u;
    p.timer.assign(0);
    p.pos = {pos.x + kPlayfieldX, pos.y + kPlayfieldY, 0.0f};
}

void Gui::queueText(float x, float y, const std::string& text, std::uint32_t color, float sx, float sy, float spacing, bool playfield) {
    if (text_.size() >= 256) return;
    text_.push_back({x, y, text.substr(0, 64), color, sx, sy, spacing, playfield});
}

// ---- timed messages and banners ------------------------------------------------------------

void Gui::status(Status s, std::uint32_t value) {
    switch (s) {
        case Bonus:
            bonus_ = Timed{true, 416.0f, 48.0f, value, 0, {}};
            break;
        case SpellCardBonus:
            spellBonus_ = Timed{true, 0.0f, 0.0f, value, 0, {}};
            break;
        default:
            status_ = Timed{true, 416.0f, 168.0f, value, static_cast<int>(s), {}};
            break;
    }
}

void Gui::bonus(std::uint32_t points) { status(Bonus, points); }
void Gui::spellCardBonus(std::uint32_t points) { status(SpellCardBonus, points); }

void Gui::renderText(AnmVm& vm, const std::string& sjis, std::uint32_t colorRef, bool rightAligned) {
    vm.set(AnmVm::Visible, true);
    const AnmSprite* sp = vm.sprite;
    if (!sp || !res_.textures) return;
    Image* img = res_.textures->pixels(sp->texture);
    if (!img) return;
    const int size = vm.fontWidth != 0 ? vm.fontWidth : 15;
    const int top = static_cast<int>(sp->startY);
    Image band(img->width, 16);
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(sjis.data()), sjis.size());
    float x = sp->startX;
    if (rightAligned) {
        x = sp->startX + (sp->endX - sp->startX) * sp->cols - static_cast<float>(sjis.size()) * static_cast<float>(size) * sp->cols / 2.0f;
    }
    if (res_.text && !sjis.empty()) {
        TextStyle style;
        style.fontSize = size;
        style.rgb = colorRefToRgb(colorRef);
        res_.text->render(bytes, style, x, band);
        finishTextBand(band);
    }
    for (int y = 0; y < 16 && top + y < img->height; ++y) {
        std::memcpy(img->pixel(0, top + y), band.pixel(0, y), static_cast<std::size_t>(img->width) * 4);
    }
    res_.textures->touch(sp->texture);
}

void Gui::spellCardStart(int card, int portrait, const std::string& name) {
    (void)card;
    const World& w = *world_;
    if (portrait >= 0) {
        startVm(enemyPortrait_.vm, kFace + 3);
        enemyPortrait_.vm.selectSprite(*w.anm, kStageFace + portrait);
        const float sw = enemyPortrait_.vm.sprite ? enemyPortrait_.vm.sprite->pixelWidth : 0.0f;
        enemyPortrait_.offsetX = sw > 256.0f ? -288.0f : sw > 128.0f ? -112.0f : 0.0f;
    }
    startVm(enemyDecorU_.vm, kFace + 5);
    enemyDecorU_.vm.selectSprite(*w.anm, kFace + 12);
    startVm(enemyDecorD_.vm, kFace + 7);
    enemyDecorD_.vm.selectSprite(*w.anm, kFace + 12);
    startVm(enemyName_.vm, kText + 5);
    enemyName_.vm.fontWidth = enemyName_.vm.fontHeight = 15;
    renderText(enemyName_.vm, name, 0xFFF0F0u, true);
    enemyNameBg_.interrupt(1);
    captureIndicator_.interrupt(1);
    w.playSound(14);
}

void Gui::bombBanner(int sprite, const std::string& name) {
    const World& w = *world_;
    startVm(bombPortrait_.vm, kFace + 1);
    bombPortrait_.vm.selectSprite(*w.anm, sprite);
    startVm(bombDecorL_.vm, kFace + 4);
    bombDecorL_.vm.selectSprite(*w.anm, kFace + 12);
    startVm(bombDecorR_.vm, kFace + 6);
    bombDecorR_.vm.selectSprite(*w.anm, kFace + 12);
    startVm(bombName_.vm, kText + 4);
    bombName_.vm.fontWidth = bombName_.vm.fontHeight = 15;
    renderText(bombName_.vm, name, 0xF0F0FFu, false);
    bombNameBg_.interrupt(1);
    w.playSound(14);
}

void Gui::bombBannerEnd() {
    bombName_.vm.interrupt(1);
    bombNameBg_.interrupt(2);
}

void Gui::spellCardEnd() {
    enemyName_.vm.interrupt(1);
    enemyNameBg_.interrupt(2);
    captureIndicator_.interrupt(2);
}

// ---- dialogue ------------------------------------------------------------------------------

void Gui::startDialogue(int number) {
    if (number < 0 || static_cast<std::size_t>(number) >= msgScripts_.size() || !world_) return;
    const World& w = *world_;
    for (Element& p : portraits_) p = Element{};
    for (AnmVm& l : lines_) l = AnmVm{};
    for (AnmVm& l : introLines_) l = AnmVm{};
    msgIndex_ = number;
    msgPos_ = msgScripts_[static_cast<std::size_t>(number)];
    msgTimer_.assign(0);
    waited_ = 0;
    resume_ = 0;
    skippable_ = true;
    line1Used_ = false;
    if (w.bulletManager) {
        w.bulletManager->clearAll(w, 1);
        w.bulletManager->removeItems();
    }
    // (All enemies removed, no score: the enemy manager hooks in here.)
    if (w.onDialogueStart) w.onDialogueStart();
    if (number % 10 == 0) {
        copyBossName(2 * (stage_ - 1) + 1);
        if (w.bulletManager && stage_ >= 6) w.bulletManager->setCancelItemType(ItemField::Star);
    }
}

void Gui::runDialogue(const World& w, std::uint16_t input, std::uint16_t previousInput) {
    if (msgIndex_ < 0) return;
    if (resume_ > 0) --resume_;
    const bool skip = (input & BtnSkip) != 0;
    const bool shootPressed = (input & BtnShoot) && !(previousInput & BtnShoot);
    auto at = [&](std::size_t o) { return o < msg_.size() ? msg_[o] : 0; };
    auto instrTime = [&]() { return msgPos_ + 4 <= msg_.size() ? static_cast<std::int32_t>(static_cast<std::uint16_t>(rd16(msg_, msgPos_))) : 0; };
    if (skippable_ && skip) msgTimer_.assign(instrTime());
    if (w.player && w.player->borderFlag() != Player::BorderFlag::None) w.player->endBorderForDialogue(w);
    if (w.player && !w.player->dead() && w.bulletManager) w.bulletManager->removeItems();
    bool stalled = false;
    while (msgPos_ + 4 <= msg_.size() && msgTimer_.current >= instrTime()) {
        const std::uint8_t op = at(msgPos_ + 2);
        const std::size_t argc = at(msgPos_ + 3);
        const std::size_t a = msgPos_ + 4;
        bool advance = true;
        switch (op) {
            case 0: msgIndex_ = -1; return;
            case 1: {
                const int side = rd16(msg_, a) & 1, index = rd16(msg_, a + 2);
                Element& p = portraits_[static_cast<std::size_t>(side)];
                startVm(p.vm, index + (side == 0 ? kFace : kFace + 2));
                p.offsetX = p.vm.sprite && p.vm.sprite->pixelWidth > 128.0f ? -112.0f : 0.0f;
                p.offsetY = 0.0f;
                break;
            }
            case 2: {
                const int side = rd16(msg_, a) & 1, index = rd16(msg_, a + 2);
                Element& p = portraits_[static_cast<std::size_t>(side)];
                p.vm.selectSprite(*w.anm, index + (side == 0 ? kFace : kStageFace));
                const float sw = p.vm.sprite ? p.vm.sprite->pixelWidth : 0.0f;
                if (sw > 256.0f) p.offsetX = -208.0f, p.offsetY = -50.0f;
                else if (sw > 128.0f) p.offsetX = -80.0f;
                else p.offsetX = 0.0f;
                break;
            }
            case 3:
            case 8: {
                const int color = rd16(msg_, a) & 3, line = rd16(msg_, a + 2) & 1;
                std::string text;
                for (std::size_t i = a + 4; i < a + argc && i < msg_.size() && msg_[i]; ++i) text.push_back(static_cast<char>(msg_[i]));
                static constexpr std::uint32_t kColors[4] = {0xE8F0FFu, 0xFFE8F0u, 0u, 0u};
                if (op == 3) {
                    if (line == 0 && line1Used_) renderText(lines_[1], " ", kColors[color], false);
                    startVm(lines_[static_cast<std::size_t>(line)], kText + line);
                    lines_[static_cast<std::size_t>(line)].fontWidth = lines_[static_cast<std::size_t>(line)].fontHeight = 15;
                    renderText(lines_[static_cast<std::size_t>(line)], text, kColors[color], false);
                    if (line == 1) line1Used_ = true;
                } else {
                    startVm(introLines_[static_cast<std::size_t>(line)], kText + 2 + line);
                    introLines_[static_cast<std::size_t>(line)].fontWidth = introLines_[static_cast<std::size_t>(line)].fontHeight = 15;
                    renderText(introLines_[static_cast<std::size_t>(line)], text, kColors[color], true);
                }
                waited_ = 0;
                break;
            }
            case 4: {
                const std::int32_t frames = rd32(msg_, a);
                if (skippable_ && skip) break;
                if (shootPressed && waited_ >= 12) break;
                if (waited_ >= frames) break;
                ++waited_;
                advance = false;
                stalled = true;
                break;
            }
            case 5: {
                const int target = rd16(msg_, a);
                const int interrupt = at(a + 2);
                if (target < 2) portraits_[static_cast<std::size_t>(target & 1)].vm.interrupt(interrupt);
                else lines_[static_cast<std::size_t>((target - 2) & 1)].interrupt(interrupt);
                break;
            }
            case 6: ++resume_; break;
            case 7: {
                const std::int32_t track = rd32(msg_, a);
                startVm(stageText_[0], kStageText + (stage_ == 6 ? 5 : 4));
                stageText_[0].selectSprite(*w.anm, kStageText + 3 + track);
                if (w.onMusic) w.onMusic(track);
                break;
            }
            case 9: {
                const GameState& g = *w.state;
                clearPoints_ = g.stagePointItems;
                clearGraze_ = g.stageGraze;
                clearCherryMax_ = g.cherryMax;
                clearState_ = 1;
                if (stage_ <= 5) {
                    stageClear_ = Element{};
                    startVm(stageClear_.vm, kLoading);
                    stageClear_.active = true;
                } else {
                    w.state->extendsGranted = -1;
                }
                break;
            }
            case 10: advance = false, stalled = true; break;
            case 11: {
                w.state->guiScore = w.state->score;
                if (w.state->practice || stage_ >= 6) {
                    gameFinished_ = true;
                } else {
                    showLoading_ = true;
                    nextStage_ = true;
                    msgIndex_ = -2;
                }
                advance = false;
                stalled = true;
                break;
            }
            case 12:
                if (w.onMusicFade) w.onMusicFade(4.0f);
                break;
            case 13: skippable_ = at(a) != 0; break;
            default: break;
        }
        if (!advance) break;
        msgPos_ += 4 + argc;
    }
    if (msgIndex_ < 0 && msgIndex_ != -2) return;
    if (!stalled) msgTimer_.tick();
    for (Element& p : portraits_) p.vm.tick(*w.anm, *w.rng);
    for (AnmVm& l : lines_) l.tick(*w.anm, *w.rng);
    for (AnmVm& l : introLines_) l.tick(*w.anm, *w.rng);
    if (msgTimer_.current < 60 && skippable_ && skip) msgTimer_.assign(60);
}

// ---- the GUI update (logic order 13) ---------------------------------------------------------

void Gui::bossHudUpdate() {
    const BossHud& b = world_->state->boss;
    AnmVm& label = front_[11];
    if (b.present) {
        if (bossState_ == 0) {
            label.interrupt(1);
            bossState_ = 1;
            bossAlpha_ = 0;
        } else {
            if (label.has(AnmVm::Stopped)) bossState_ = 2;
            if (bossAlpha_ < 252) bossAlpha_ += 4;
            else bossAlpha_ = 255;
        }
    } else if (bossState_ != 0) {
        if (bossState_ <= 2) {
            label.interrupt(2);
            bossState_ = 3;
        }
        if (bossAlpha_ > 0) bossAlpha_ -= 4;
        else bossAlpha_ = 0;
        if (label.has(AnmVm::Stopped)) {
            bossState_ = 0;
            bossFill_ = 0;
            bossAlpha_ = 0;
        }
    }
    if (bossState_ >= 2) {
        if (bossFill_ < b.bar) bossFill_ = std::min(bossFill_ + 0.01f, b.bar);
        else if (bossFill_ > b.bar) bossFill_ -= 0.02f;
    }
}

void Gui::update(const World& w, std::uint16_t input, std::uint16_t previousInput) {
    world_ = &w;
    if (msgIndex_ < 0) bossHudUpdate();
    for (AnmVm& vm : front_) vm.tick(*w.anm, *w.rng);
    for (int i = 0; i < 5; ++i) {
        if (stageTextUsed_[static_cast<std::size_t>(i)]) stageText_[static_cast<std::size_t>(i)].tick(*w.anm, *w.rng);
    }
    for (Element* e : {&bombPortrait_, &bombDecorL_, &bombDecorR_, &bombName_, &enemyPortrait_, &enemyDecorU_, &enemyDecorD_, &enemyName_}) {
        e->vm.tick(*w.anm, *w.rng);
    }
    bombNameBg_.tick(*w.anm, *w.rng);
    enemyNameBg_.tick(*w.anm, *w.rng);
    captureIndicator_.tick(*w.anm, *w.rng);
    if (stageClear_.active) {
        if (stageClear_.vm.tick(*w.anm, *w.rng)) stageClear_.active = false;
    }
    for (Timed* t : {&bonus_, &status_, &spellBonus_}) {
        if (!t->shown) continue;
        const int limit = t == &bonus_ ? 250 : t == &status_ ? 180 : 280;
        if (t->timer.current >= limit) t->shown = false;
        t->timer.tick();
    }
    if (clearState_ == 1) computeStageBonus(w);
    runDialogue(w, input, previousInput);
    ++frame_;
    if (stage_ == 6 && frame_ == 300 && w.onMusic) w.onMusic(0);
}

void Gui::computeStageBonus(const World& w) {
    GameState& g = *w.state;
    std::int32_t b = g.stage * 100000 + static_cast<std::int32_t>(clearGraze_) * 50 + static_cast<std::int32_t>(clearPoints_) * 5000 + clearCherryMax_;
    if (g.stage >= 7 || (g.stage == 6 && !g.practice)) b += g.livesI() * 2000000 + g.bombsI() * 400000;
    switch (g.difficulty) {
        case Easy: b /= 2; break;
        case Hard: b = b * 12 / 10; break;
        case Lunatic: b = b * 15 / 10; break;
        case Extra:
        case Phantasm: b *= 2; break;
        default: break;
    }
    if (g.lifeCount == 3) b = b * 5 / 10;
    else if (g.lifeCount == 4) b = (b * 2) / 10;
    clearBonus_ = static_cast<std::uint32_t>(b);
    for (int i = 0; i < 10; ++i) g.addScore(clearBonus_);
    clearState_ = 2;
}

// ---- drawing ---------------------------------------------------------------------------------

void Gui::rect(SpriteSink& sink, float x0, float y0, float x1, float y1, std::uint32_t argb) {
    if (!res_.ui || x1 <= x0 || y1 <= y0) return;
    sink.fillRect(res_.ui->white, x0, y0, x1 - x0, y1 - y0, argbToRgbaBytes(argb));
}

void Gui::drawDigits(SpriteSink& sink, AnmVm& vm, float x, float y, std::int64_t value, int digits, std::int64_t divisor, float step,
                     const AnmDrawState& state) {
    bool seen = false;
    for (int i = 0; i < digits; ++i, divisor /= 10) {
        const std::int64_t d = value / divisor;
        value %= divisor;
        if (d != 0) seen = true;
        if (seen || i == digits - 1) {
            vm.selectSprite(*world_->anm, static_cast<int>(132 + std::clamp<std::int64_t>(d, 0, 9)));
            vm.position = {x, y, 0.0f};
            drawVm(sink, vm, state);
        }
        x += step;
    }
}

void Gui::drawPopups(SpriteSink& sink) {
    if (!world_) return;
    AnmDrawState none;
    const Vec3f pp = world_->player ? world_->player->position() : Vec3f{};
    for (Popup& p : popups_) {
        if (!p.live) continue;
        const float dx = pp.x - p.pos.x, dy = pp.y - p.pos.y;
        const auto q = static_cast<std::int32_t>(dx * dx + dy * dy);
        const std::uint32_t alpha = q > 4096 ? 208 : q > 1024 ? static_cast<std::uint32_t>((q - 1024) * 128 / 3072 + 80) : 80;
        float x = p.pos.x - 4.0f * static_cast<float>(p.count);
        for (int i = p.count - 1; i >= 0; --i) {
            const int d = p.digits[static_cast<std::size_t>(i)];
            int sprite = d;
            if (d != 10) sprite = p.timer.current < 52 ? d : p.timer.current < 56 ? d + 11 : d + 21;
            glyphVm_.selectSprite(*world_->anm, sprite);
            glyphVm_.flags = AnmVm::Visible | AnmVm::Active;
            glyphVm_.color = argbWithAlpha(p.color, alpha);
            glyphVm_.scale = {1.0f, 1.0f};
            glyphVm_.position = {x, p.pos.y, 0.0f};
            drawVm(sink, glyphVm_, none);
            x += 8.0f;
        }
    }
    // The cherry gauge.
    if (!gaugeVm_.has(AnmVm::Visible)) return;
    const GameState& g = *world_->state;
    drawVm(sink, gaugeVm_, none);
    const float gx = gaugeVm_.position.x, gy = gaugeVm_.position.y;
    const std::uint32_t ga = gaugeVm_.color >> 24;
    AnmVm& dv = cherryDigitVm_;
    dv.flags |= AnmVm::Visible | AnmVm::Active;
    dv.scale = {1.0f, 1.0f};
    dv.color = argbWithAlpha(g.cherry >= g.cherryMax ? 0xFFD080u : g.cherry >= 50000 ? 0xFFFF80u : 0xFFFFFFu, ga);
    drawDigits(sink, dv, gx + 46.0f, gy + 11.0f, g.cherry, 6, 100000, 7.0f, none);
    dv.color = argbWithAlpha(0xF0D0E0u, ga);
    const int maxDigits = g.cherryMax >= 1000000 ? 7 : 6;
    drawDigits(sink, dv, gx + 46.0f + 42.0f + 9.0f, gy + 11.0f, g.cherryMax, maxDigits, maxDigits == 7 ? 1000000 : 100000, 7.0f, none);
    const bool borderOn = world_->player && world_->player->borderFlag() != Player::BorderFlag::None;
    if (borderOn) {
        std::int32_t p = g.cherryPlus % 4000;
        if (p >= 2000) p = 4000 - p;
        const std::uint32_t gb = static_cast<std::uint32_t>(g.cherryPlus * 192 / 50000 + p * 64 / 2000) & 0xFFu;
        dv.color = (ga << 24) | 0xFF0000u | (gb << 8) | gb;
        dv.scale = {1.41f, 1.41f};
        drawDigits(sink, dv, gx + 53.0f + 2.0f, gy + 2.0f - 2.0f, g.cherryPlus, 5, 10000, 10.0f, none);
    } else {
        dv.color = argbWithAlpha(0xC080B0u, ga);
        drawDigits(sink, dv, gx + 53.0f, gy + 2.0f, g.cherryPlus, 5, 10000, 7.0f, none);
    }
    dv.scale = {1.0f, 1.0f};
    if (world_->player && world_->player->borderFlag() == Player::BorderFlag::Active) {
        borderVm_.position = {gx + 24.0f, gy + 8.0f, 0.0f};
        borderVm_.color = gaugeVm_.color;
        drawVm(sink, borderVm_, none);
    }
}

void Gui::drawSidePanel(SpriteSink& sink, const AnmDrawState& state) {
    const GameState& g = *world_->state;
    AnmVm& tile = front_[12];
    for (float y = 0; y <= 448.0f; y += 32.0f) {
        tile.position = {0.0f, y, 0.49f};
        drawVm(sink, tile, state);
    }
    for (float x = 416.0f; x <= 608.0f; x += 32.0f) {
        for (float y = 16.0f; y <= 448.0f; y += 32.0f) {
            tile.position = {x, y, 0.49f};
            drawVm(sink, tile, state);
        }
    }
    AnmVm& strip = front_[13];
    for (float x = 0; x <= 512.0f; x += 128.0f) {
        for (float y : {0.0f, 464.0f}) {
            strip.position = {x, y, 0.49f};
            drawVm(sink, strip, state);
        }
    }
    for (int k = 0; k <= 8; ++k) drawVm(sink, front_[static_cast<std::size_t>(k)], state);
    auto scoreLine = [&](float y, std::uint32_t value, int cont) {
        if (value >= 100000000u) {
            queueText(496.0f, y, std::format("{:09d}", value), 0xFFFFFFFFu, 0.9f, 1.0f);
            queueText(496.0f + 113.4f, y, std::format("{}", cont % 10), 0xFFFFFFFFu);
        } else {
            queueText(496.0f, y, std::format("{:08d}", value), 0xFFFFFFFFu);
            queueText(496.0f + 112.0f, y, std::format("{}", cont % 10), 0xFFFFFFFFu);
        }
    };
    scoreLine(48.0f, g.hiScore, g.hiScoreContinues);
    scoreLine(64.0f, g.guiScore, g.continues);
    for (int i = 0; i < g.livesI(); ++i) {
        front_[9].position = {496.0f + 16.0f * static_cast<float>(i), 96.0f, 0.46f};
        drawVm(sink, front_[9], state);
    }
    for (int i = 0; i < g.bombsI(); ++i) {
        front_[10].position = {496.0f + 16.0f * static_cast<float>(i), 112.0f, 0.46f};
        drawVm(sink, front_[10], state);
    }
    if (g.powerI() > 0) rect(sink, 496.0f, 144.0f, 496.0f + static_cast<float>(g.powerI()), 160.0f, 0xC0E0E0FFu);
    queueText(496.0f, 144.0f, g.powerI() < 128 ? std::format("{}", g.powerI()) : std::string("MAX"), 0xFFFFFFFFu);
    queueText(496.0f, 160.0f, std::format("{}", g.totalGraze), 0xFFFFFFFFu);
    queueText(496.0f, 176.0f, std::format("{}/{}", g.extendPoints, g.nextExtend), 0xFFFFFFFFu);
    strip.position = {32.0f, 464.0f, 0.49f};
    for (float x : {32.0f, 160.0f, 288.0f}) {
        strip.position.x = x;
        drawVm(sink, strip, state);
    }
}

void Gui::drawBossHud(SpriteSink& sink, const AnmDrawState& state) {
    const BossHud& b = world_->state->boss;
    if (msgIndex_ >= 0 || !(b.present || bossState_ > 0)) return;
    const std::uint32_t a = (bossAlpha_ & 0xFFu) << 24;
    rect(sink, 64.0f, 19.0f, 64.0f + bossFill_ * 320.0f, 23.0f, a | 0xFFFFFFu);
    for (int j = 0; j < 8; ++j) {
        const float s = b.segmentA[static_cast<std::size_t>(j)], e = b.segmentB[static_cast<std::size_t>(j)];
        if (e == 0.0f || s >= bossFill_) continue;
        rect(sink, 64.0f + s * 320.0f, 19.0f, 64.0f + std::min(e, bossFill_) * 320.0f, 23.0f,
             a | (static_cast<std::uint32_t>(b.segmentColor[static_cast<std::size_t>(j)]) & 0xFFFFFFu));
    }
    drawVm(sink, front_[11], state);
    const int n = b.lifeMarkers;
    const float gap = n <= 5 ? 2.0f : 1.0f;
    for (int j = 0; j < n; ++j) {
        const float x0 = static_cast<float>(j * 26 / n) + 35.0f, x1 = static_cast<float>((j + 1) * 26 / n) + 35.0f - gap;
        rect(sink, x0, 19.0f, x1, 23.0f, a | (0xFFFFFFu - static_cast<std::uint32_t>(j * 255 / 9)));
    }
    const std::int32_t secs = b.spellSeconds;
    const std::uint32_t col = secs >= 20 ? 0xA0D0FFu : secs >= 10 ? 0xA080FFu : secs >= 5 ? 0xE080C0u : 0xFF4040u;
    const std::int32_t shown = std::clamp(secs, 0, 99);
    queueText(384.0f, 16.0f, std::format("{:02d}", shown), a | col);
    if (shown < 10 && secs != lastSeconds_) world_->playSound(29);
    lastSeconds_ = secs;
}

void Gui::drawDialogue(SpriteSink& sink, const AnmDrawState& state) {
    if (msgIndex_ < 0) return;
    if (stage_ == 6 && (msgIndex_ == 1 || msgIndex_ == 11)) return;
    const float h = msgTimer_.current < 60 ? msgTimer_.asFloat() * 48.0f / 60.0f : 48.0f;
    drawVm(sink, portraits_[0].vm, state);
    AnmVm& p1 = portraits_[1].vm;
    const Vec3f keep = p1.position;
    p1.position.x += portraits_[1].offsetX;
    p1.position.y += portraits_[1].offsetY;
    drawVm(sink, p1, state);
    p1.position = keep;
    rect(sink, kPlayfieldX + 16.0f, 384.0f, kPlayfieldX + 368.0f, 384.0f + h, 0xB0000000u);
    for (AnmVm& l : lines_) drawVm(sink, l, state);
    for (AnmVm& l : introLines_) drawVm(sink, l, state);
}

void Gui::queueTally(const World& w) {
    const GameState& g = *w.state;
    queueText(144.0f, 128.0f, g.stage <= 5 ? "Stage Clear" : "All Clear!", 0xFFFFFF40u);
    float y = 160.0f;
    queueText(144.0f, y, std::format("Clear  = {:8d}", g.stage * 1000000), 0xFFFFFFFFu);
    queueText(144.0f, y += 16.0f, std::format("Point  = {:8d}", clearPoints_ * 50000), 0xFFE0E0FFu);
    queueText(144.0f, y += 16.0f, std::format("Graze  = {:8d}", clearGraze_ * 500), 0xFFD0D0FFu);
    queueText(144.0f, y += 16.0f, std::format("Cherry = {:8d}", clearCherryMax_ * 10), 0xFFD0D0FFu);
    if (g.stage >= 7 || (g.stage == 6 && !g.practice)) {
        queueText(144.0f, y += 16.0f, std::format("Player ={:9d}", g.livesI() * 20000000), 0xFFFFFF80u);
        queueText(144.0f, y += 16.0f, std::format("Bomb   = {:8d}", g.bombsI() * 4000000), 0xFFFFFF80u);
    }
    static constexpr const char* kRank[6] = {"Easy Rank    *0.5", "Normal Rank  *1.0", "Hard Rank    *1.2",
                                             "Lunatic Rank *1.5", "Extra Rank   *2.0", "Phantasm Rank*2.0"};
    queueText(144.0f, y += 32.0f, kRank[std::clamp(g.difficulty, 0, 5)], 0xFFFF8080u);
    if (g.difficulty < Extra && !g.practice && (g.lifeCount == 3 || g.lifeCount == 4)) {
        queueText(144.0f, y += 16.0f, g.lifeCount == 3 ? "Player Penalty*0.5" : "Player Penalty*0.2", 0xFFFF8080u);
    }
    queueText(144.0f, y += 16.0f, std::format("Total = {:8d}0", clearBonus_), 0xFFFFFFFFu);
}

void Gui::draw(SpriteSink& sink, AnmDrawState& state) {
    if (!world_) return;
    state.offsetX = state.offsetY = 0.0f;
    if (clearState_ != 0) queueTally(*world_);
    drawDialogue(sink, state);
    for (AnmVm& vm : stageText_) drawVm(sink, vm, state);
    if (bombPortrait_.vm.has(AnmVm::Visible)) {
        drawVm(sink, bombPortrait_.vm, state);
        drawVm(sink, bombDecorL_.vm, state);
        drawVm(sink, bombDecorR_.vm, state);
    }
    if (enemyPortrait_.vm.has(AnmVm::Visible)) {
        AnmVm& p = enemyPortrait_.vm;
        const Vec3f keep = p.position;
        p.position.x += enemyPortrait_.offsetX;
        drawVm(sink, p, state);
        p.position = keep;
        drawVm(sink, enemyDecorU_.vm, state);
        drawVm(sink, enemyDecorD_.vm, state);
    }
    if (bombName_.vm.has(AnmVm::Visible)) {
        bombNameBg_.position = bombName_.vm.position;
        drawVm(sink, bombNameBg_, state);
        drawVm(sink, bombName_.vm, state);
    }
    if (enemyName_.vm.has(AnmVm::Visible)) {
        enemyNameBg_.position = enemyName_.vm.position;
        drawVm(sink, enemyNameBg_, state);
        drawVm(sink, enemyName_.vm, state);
        drawVm(sink, captureIndicator_, state);
        const GameState& g = *world_->state;
        const std::int64_t v = g.capturing ? static_cast<std::int64_t>(g.captureValue) + g.grazeBonus : 0;
        digitVm_.flags |= AnmVm::Visible | AnmVm::Active;
        drawDigits(sink, digitVm_, captureIndicator_.position.x - 40.0f, captureIndicator_.position.y, v, 8, 10000000, 7.0f, state);
    }
    if (stageClear_.active) {
        drawVm(sink, stageClear_.vm, state);
        if (showLoading_) {
            loadingVm_.selectSprite(*world_->anm, 0x10C);
            loadingVm_.flags |= AnmVm::Visible | AnmVm::Active;
            loadingVm_.position = {304.0f, 448.0f, 0.0f};
            drawVm(sink, loadingVm_, state);
        }
    }
    drawBossHud(sink, state);
    drawSidePanel(sink, state);
    if (bonus_.shown) {
        const float x = bonus_.timer.current < 30 ? static_cast<float>(bonus_.timer.current * -312 / 30 + 416) : 104.0f;
        queueText(x, bonus_.y, std::format("BONUS {:8d}", bonus_.value), 0xFFFFFF80u, 1.0f, 1.0f, 14.0f, true);
    }
    if (status_.shown) {
        const float x = status_.timer.current < 30 ? static_cast<float>(status_.timer.current * -312 / 30 + 416) : 104.0f;
        switch (status_.type) {
            case FullPowerMode: queueText(x, 168.0f, "Full Power Mode!", 0xFFC0B0FFu, 1.0f, 1.0f, 14.0f, true); break;
            case SupernaturalBorder: queueText(x, 168.0f, "Supernatural Border!!", 0xFFE0B0FFu, 0.9f, 1.0f, 11.0f, true); break;
            case CherryMax: queueText(x, 168.0f, "CherryPoint Max!", 0xFFC0B0FFu, 1.0f, 1.0f, 14.0f, true); break;
            case BorderBonus:
                queueText(x, 168.0f, std::format("Border Bonus {:7d}", status_.value), 0xFFE0B0FFu, 0.9f, 1.0f, 11.0f, true);
                break;
            default: break;
        }
    }
    if (spellBonus_.shown) {
        queueText(88.0f, 80.0f, "Spell Card Bonus!", 0xFFFF0000u, 1.0f, 1.0f, 14.0f, true);
        const std::string v = std::format("+{}", spellBonus_.value);
        queueText(static_cast<float>((384 - static_cast<int>(v.size()) * 32) / 2 + 32), 96.0f, v, 0xFFFF8080u, 2.0f, 2.0f, 14.0f, true);
    }
}

void Gui::drawText(SpriteSink& sink) {
    if (!world_) return;
    AnmDrawState none;
    for (const TextItem& t : text_) {
        float x = t.x, y = t.y;
        for (unsigned char c : t.text) {
            if (c == '\n') {
                y += 16.0f * t.sy;
                x = t.x;
                continue;
            }
            if (c != ' ' && c > 0) {
                glyphVm_.selectSprite(*world_->anm, c - 1);
                glyphVm_.flags = AnmVm::Visible | AnmVm::Active | AnmVm::AnchorLeft | AnmVm::AnchorTop;
                glyphVm_.color = t.color;
                glyphVm_.scale = {t.sx, t.sy};
                glyphVm_.position = {x, y, 0.0f};
                drawVm(sink, glyphVm_, none);
            }
            x += t.spacing * t.sx;
        }
    }
    text_.clear();
    const Vec3f pp = world_->player ? world_->player->position() : Vec3f{};
    for (int b = 0; b < 4; ++b) {
        AnmVm& m = markerVms_[static_cast<std::size_t>(b)];
        const float x = m.position.x;
        if (x < 56.0f || x > 392.0f) continue;
        const float d = std::fabs(x - 32.0f - pp.x);
        const std::uint32_t alpha = d < 64.0f ? static_cast<std::uint32_t>(d * 128.0f / 64.0f + 48.0f) : 176u;
        m.color = world_->state->boss.markerTint[static_cast<std::size_t>(b)] ? 0x804040FFu : argbWithAlpha(0xFFFFFFu, alpha);
        drawVm(sink, m, none);
    }
}

}  // namespace touhou::th07
