#include "th08/MainMenu.hpp"

#include <algorithm>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "th08/Bgm.hpp"
#include "th08/ExeTables.hpp"

namespace touhou::th08 {
namespace {

constexpr int kDescScript = 9;              // text.anm
constexpr std::uint32_t kDescColor = 0xFFF0E0;  // GDI 0x00BBGGRR (shadow 0x300000: the thick outline)
constexpr std::uint32_t kGrey = 0xFF404040u;
constexpr int kFadeFrames = 70;

int wrap(int v, int n) { return ((v % n) + n) % n; }

}  // namespace

bool MainMenu::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx_.anm;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "title01.anm", kSlotTitle, error)) return false;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, "title/title00.png")) log::warn("title00.png could not be loaded");
    // The 14 description VMs: text.anm script 9, each on that script's sprite + i.
    for (int i = 0; i < kDescriptions; ++i) {
        AnmVm& d = desc_[static_cast<std::size_t>(i)];
        d.startFresh(lib, *ctx_.rng, kSlotText, kDescScript);
        d.selectSprite(d.activeSprite + i);
        d.set(AnmVm::Visible, false);
    }
    currentDesc_ = 0;
    descShown_ = false;
    // The cursor from the previous scene (spec 3.2 step 8).
    switch (ctx_.previous) {
        case Scene::ResultViewer: cursor_ = PlayerData; break;
        case Scene::MusicRoom: cursor_ = MusicRoom; break;
        default: cursor_ = GameStart; break;
    }
    if (ctx_.music && ctx_.previous != Scene::ResultViewer) ctx_.music->load(8, "bgm/th08_01.mid");
    fade_ = kFadeFrames;  // the white fade-in (screen effect type 0, 70 frames)
    sub_ = 0;
    firstEntry_ = true;
    return true;
}

bool MainMenu::rowLocked(int row) const {
    if (row == ExtraStart) return !ctx_.extraUnlocked;
    if (row == SpellPractice) return !ctx_.spellPracticeUnlocked;
    return false;
}

void MainMenu::sendAll(int interrupt) {
    for (int i = 0; i < kInstances; ++i) {
        if (has_[static_cast<std::size_t>(i)]) vm_[static_cast<std::size_t>(i)].interrupt(interrupt);
    }
}

// Rows 1-9: the cursor row on its base sprite, the others on base + 1; locked rows grey.
void MainMenu::highlightRows() {
    for (int r = 0; r < kTitleRows; ++r) {
        const auto k = static_cast<std::size_t>(1 + r);
        if (!has_[k]) continue;
        vm_[k].selectSprite(base_[k] + (r == cursor_ ? 0 : 1));
        if (rowLocked(r)) vm_[k].color = kGrey;
    }
}

void MainMenu::showDescription(int row) {
    currentDesc_ = std::clamp(row, 0, kDescriptions - 1);
    desc_[static_cast<std::size_t>(currentDesc_)].interrupt(1);
    descShown_ = true;
}

void MainMenu::enterTitle() {
    AnmLibrary& lib = *ctx_.anm;
    if (firstEntry_ && ctx_.music && ctx_.previous != Scene::ResultViewer) ctx_.music->playLoaded(8);
    if (!instancesMade_) {
        for (int i = 0; i < kInstances; ++i) {
            const auto k = static_cast<std::size_t>(i);
            vm_[k] = AnmVm{};
            has_[k] = lib.file(kSlotTitle) && lib.file(kSlotTitle)->script(i) != nullptr;
            if (!has_[k]) continue;
            vm_[k].startFresh(lib, *ctx_.rng, kSlotTitle, i);
            base_[k] = vm_[k].activeSprite;
        }
        instancesMade_ = true;
    }
    firstEntry_ = false;
    sendAll(2);
    highlightRows();
}

void MainMenu::title(const MenuInput& in) {
    if (sub_ == 0) {
        if (inputTimer_ == 0) enterTitle();
        // One description per pass, rows 0..8 (TH07 rendered them all at once).
        const int k = inputTimer_;
        if (k < kTitleRows) {
            const std::string_view text = ctx_.tables ? std::string_view(ctx_.tables->titleText[static_cast<std::size_t>(k)].sjis) : "";
            renderSpriteText(ctx_.textures, ctx_.text, desc_[static_cast<std::size_t>(k)], text, kDescColor, TextAlign::Centre);
            desc_[static_cast<std::size_t>(k)].set(AnmVm::Visible, false);
            return;
        }
        inputTimer_ = 0;
        stateTimer_ = 0;
        idle_ = 0;
        remembered_ = -1;
        sub_ = 1;
    }
    if (sub_ == 2) {  // quitting
        if (stateTimer_ >= 60) {
            removed_ = true;
            if (ctx_.request) ctx_.request(Scene::Exit);
        }
        return;
    }
    if (sub_ != 1) return;

    // Vertical move over the nine rows, skipping the locked ones.
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        cursor_ = wrap(cursor_ + d, kTitleRows);
        for (int guard = 0; guard < kTitleRows && rowLocked(cursor_); ++guard) cursor_ = wrap(cursor_ + d, kTitleRows);
        ctx_.playSound(kSndMove);
        highlightRows();
    }
    // Idle: the attract demo after 1500 passes (not ported yet: the counter starts again).
    if (in.current() != 0) idle_ = 0;
    else if (++idle_ > 1500) idle_ = 0;
    if (cursor_ != remembered_) {
        showDescription(cursor_);
        remembered_ = cursor_;
    }
    if (inputTimer_ < 10) return;
    if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        if (cursor_ == Quit) {
            sub_ = 2;
            stateTimer_ = 0;
            sendAll(1);
            return;
        }
        notice_ = 150;  // every other screen is still being ported
        return;
    }
    if (in.pressed(kCancel)) {
        cursor_ = Quit;
        highlightRows();
        ctx_.playSound(kSndBack);
    }
}

void MainMenu::update(const MenuInput& in) {
    if (removed_) return;
    title(in);
    // After the state handler: all 142 VMs and the current description advance.
    for (int i = 0; i < kInstances; ++i) {
        if (has_[static_cast<std::size_t>(i)]) vm_[static_cast<std::size_t>(i)].tick(*ctx_.rng);
    }
    if (descShown_) desc_[static_cast<std::size_t>(currentDesc_)].tick(*ctx_.rng);
    ++inputTimer_;
    ++stateTimer_;
    if (fade_ > 0) --fade_;
    if (notice_ > 0) --notice_;
}

void MainMenu::draw(SpriteSink& sink) {
    if (removed_) return;
    background_.blit(sink, 0, 0);
    const AnmDrawState none;
    if (instancesMade_) {
        for (int i = 0; i < kInstances; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (has_[k]) drawVm(sink, vm_[k], none);
        }
    }
    if (descShown_) drawVm(sink, desc_[static_cast<std::size_t>(currentDesc_)], none);
    if (notice_ > 0) {
        const std::uint32_t a = static_cast<std::uint32_t>(std::min(255, notice_ * 8)) << 24;
        ascii_.add(160, 448, "NOT PORTED YET", a | 0x00FFE0A0u);
    }
    if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);
    if (fade_ > 0) fillScreen(sink, ctx_.ui, (static_cast<std::uint32_t>(fade_ * 255 / kFadeFrames) << 24) | 0x00FFFFFFu);
}

}  // namespace touhou::th08
