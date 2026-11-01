#include "game/StageMenu.hpp"

#include "game/Gui.hpp"
#include "game/World.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {
namespace {

// ascii.anm scripts: pause title, Resume, Quit, confirmation title, Yes, No; the continue
// menu's title and credit label.
constexpr int kScriptPauseFirst = kAnmOffsetAscii + 2;
constexpr int kScriptContinueTitle = kAnmOffsetAscii + 8;
constexpr int kScriptCreditLabel = kAnmOffsetAscii + 9;
constexpr int kScriptYes = kAnmOffsetAscii + 6;
constexpr int kScriptNo = kAnmOffsetAscii + 7;
constexpr int kScriptCapture = kAnmOffsetCapture + 0;

constexpr std::int16_t kShow = 1, kHide = 2, kRelease = 1;  // ANM interrupts
constexpr std::uint32_t kHighlightColor = 0xffff8080u, kDimColor = 0x80808080u;
constexpr float kHighlightScale = 1.7f, kDimScale = 1.5f;

void startCapture(AnmManager& anm, AnmVm& bg) {
    if (!anm.hasScript(kScriptCapture)) return;
    anm.setAndExecuteScript(bg, kScriptCapture);
    bg.pos = {kGameRegionX, kGameRegionY, 0.0f};
}

}  // namespace

void StageMenus::blank(Record& r) {
    r.state = 0;
    r.timer = 0;
    for (AnmVm& vm : r.slots) {
        vm = AnmVm{};
        vm.visible = false;
    }
    r.background = AnmVm{};
}

void StageMenus::reset() {
    blank(pause_);
    blank(cont_);
    pauseActive_ = continueActive_ = false;
    captureRequest_ = false;
}

void StageMenus::highlight(Record& r, int on, int off) {
    AnmVm& a = r.slots[static_cast<std::size_t>(on)];
    AnmVm& b = r.slots[static_cast<std::size_t>(off)];
    a.color = kHighlightColor;
    a.scaleX = a.scaleY = kHighlightScale;
    b.color = kDimColor;
    b.scaleX = b.scaleY = kDimScale;
}

void StageMenus::tick(AnmManager& anm, Record& r, int slots) {
    for (int i = 0; i < slots; ++i) anm.executeScript(r.slots[static_cast<std::size_t>(i)]);
    anm.executeScript(r.background);
    ++r.timer;
}

StageMenus::Result StageMenus::update(AnmManager& anm, const MenuInput& in) {
    if (pauseActive_) return updatePause(anm, in);
    if (continueActive_) return updateContinue(anm, in);
    return Result::None;
}

StageMenus::Result StageMenus::updatePause(AnmManager& anm, const MenuInput& in) {
    Record& r = pause_;
    auto hideVisible = [&] {
        for (AnmVm& vm : r.slots) {
            if (vm.visible) vm.pendingInterrupt = kHide;
        }
    };
    auto signal = [&](int first, int last, std::int16_t interrupt) {
        for (int i = first; i <= last; ++i) r.slots[static_cast<std::size_t>(i)].pendingInterrupt = interrupt;
    };
    auto finish = [&](Result result) {
        pauseActive_ = false;
        r.state = PauseOpening;
        for (AnmVm& vm : r.slots) vm.visible = false;
        return result;
    };

    // Menu resumes and Q quits from any state, ignoring the input lock (Menu first, so both
    // in one pass quit).
    if (in.menu) {
        hideVisible();
        r.state = PauseClosing;
        r.timer = 0;
        r.background.pendingInterrupt = kRelease;
    }
    if (in.quit) {
        hideVisible();
        r.state = PauseQuitting;
        r.timer = 0;
    }

    switch (r.state) {
        case PauseOpening:
            for (int i = 0; i < 6; ++i) anm.setAndExecuteScript(r.slots[static_cast<std::size_t>(i)], kScriptPauseFirst + i);
            signal(0, 2, kShow);
            captureRequest_ = true;
            startCapture(anm, r.background);
            r.state = PauseResume;
            r.timer = 0;
            [[fallthrough]];
        case PauseResume:
        case PauseQuit: {
            const int start = r.state;
            highlight(r, start == PauseResume ? 1 : 2, start == PauseResume ? 2 : 1);
            if (r.timer >= 4) {
                if (in.up || in.down) r.state = start == PauseResume ? PauseQuit : PauseResume;
                if (in.shoot) {
                    r.timer = 0;
                    signal(0, 2, kHide);
                    if (start == PauseResume) {
                        r.state = PauseClosing;
                        r.background.pendingInterrupt = kRelease;
                    } else {
                        r.state = PauseNo;  // "really quit?" opens on No
                        signal(3, 5, kShow);
                    }
                }
            }
            break;
        }
        case PauseYes:
        case PauseNo: {
            const int start = r.state;
            highlight(r, start == PauseYes ? 4 : 5, start == PauseYes ? 5 : 4);
            if (r.timer >= 4) {
                if (in.up || in.down) r.state = start == PauseYes ? PauseNo : PauseYes;
                if (in.shoot) {
                    r.timer = 0;
                    signal(3, 5, kHide);
                    if (start == PauseYes) {
                        r.state = PauseQuitting;
                    } else {
                        r.state = PauseQuit;  // back to the list, on Quit
                        signal(0, 2, kShow);
                    }
                }
            }
            break;
        }
        case PauseClosing:
            if (r.timer >= 20) return finish(Result::Resume);
            break;
        case PauseQuitting:
            if (r.timer >= 20) return finish(Result::Quit);
            break;
        default:
            break;
    }
    tick(anm, r, 6);
    return Result::None;
}

StageMenus::Result StageMenus::updateContinue(AnmManager& anm, const MenuInput& in) {
    Record& r = cont_;
    auto signalAll = [&](std::int16_t interrupt) {
        for (int i = 0; i < 4; ++i) r.slots[static_cast<std::size_t>(i)].pendingInterrupt = interrupt;
    };
    auto finish = [&](Result result) {
        continueActive_ = false;
        r.state = ContOpening;
        r.timer = 0;
        for (AnmVm& vm : r.slots) vm.visible = false;
        return result;
    };
    switch (r.state) {
        case ContOpening: {
            const int scripts[4] = {kScriptContinueTitle, kScriptCreditLabel, kScriptYes, kScriptNo};
            for (int i = 0; i < 4; ++i) anm.setAndExecuteScript(r.slots[static_cast<std::size_t>(i)], scripts[i]);
            signalAll(kShow);
            captureRequest_ = true;
            startCapture(anm, r.background);
            highlight(r, 2, 3);  // the first frame shows Yes lit; the cursor is on No
            r.state = ContNo;
            r.timer = 0;
            break;
        }
        case ContYes:
        case ContNo: {
            const int start = r.state;
            highlight(r, start == ContYes ? 2 : 3, start == ContYes ? 3 : 2);
            if (r.timer >= (start == ContYes ? 4 : 30)) {
                if (in.up || in.down) r.state = start == ContYes ? ContNo : ContYes;
                if (in.shoot) {
                    signalAll(kHide);
                    r.timer = 0;
                    if (start == ContYes) {
                        r.state = ContClosing;
                        r.background.pendingInterrupt = kRelease;
                    } else {
                        r.state = ContGivingUp;
                    }
                }
            }
            break;
        }
        case ContClosing:
            if (r.timer >= 30) return finish(Result::Continue);
            break;
        case ContGivingUp:
            if (r.timer >= 20) return finish(Result::GiveUp);
            break;
        default:
            break;
    }
    tick(anm, r, 4);
    return Result::None;
}

void StageMenus::draw(AnmManager& anm, SpriteSink& sink, int creditsLeft, bool captureReady) {
    if (!frozen()) return;
    sink.setClip(kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight);
    if (pauseActive_) {
        if (pause_.state != PauseOpening && captureReady) anm.drawScreen(sink, pause_.background);
        for (const AnmVm& vm : pause_.slots) anm.drawScreen(sink, vm);
    } else {
        if (cont_.state != ContOpening && captureReady) anm.drawScreen(sink, cont_.background);
        if (cont_.state == ContYes || cont_.state == ContNo) {
            // "Credit N": the label's sprite state, moved right and showing the digit glyph.
            AnmVm digit = cont_.slots[1];
            digit.pos.x += 8.0f * digit.scaleX;
            if (anm.setActiveSprite(digit, kAnmOffsetAscii + ('0' + creditsLeft) - 21)) anm.drawScreen(sink, digit);
        }
        for (int i = 0; i < 4; ++i) anm.drawScreen(sink, cont_.slots[static_cast<std::size_t>(i)]);
    }
    sink.clearClip();
}

}  // namespace th06
