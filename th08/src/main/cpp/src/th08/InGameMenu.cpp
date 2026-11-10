#include "th08/InGameMenu.hpp"

#include <algorithm>

#include "gfx/SpriteSink.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"
#include "th08/Spellcard.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {
constexpr int kPauseScripts = 12, kContinueScripts = 22;  // ascii.anm
constexpr int kSlotTimes = Gui::kSlotTimes;
constexpr int kSeMove = 0, kSeSelect = 10, kSePaused = 34;
constexpr std::uint32_t kLit = 0xFFFFFFFFu, kDim = 0xFF505050u, kYesNoLit = 0xFFFF8080u;
}  // namespace

void InGameMenu::interruptAll(int which, int from, int to) {
    for (int i = from; i <= to; ++i) {
        if (visible_[static_cast<std::size_t>(i)]) vms_[static_cast<std::size_t>(i)].interrupt(which);
    }
}

void InGameMenu::openPause(const World& w) {
    mode_ = Mode::Pause;
    setState(State::Opening);
    w.playSound(kSePaused);
}

void InGameMenu::openContinue(const World& w) {
    (void)w;
    mode_ = Mode::Continue;
    setState(State::Opening);
}

void InGameMenu::startSnapshot(AnmLibrary& lib, ZunRng& rng) {
    snapshot_ = AnmVm{};
    if (!lib.loaded(kSlotCapture)) return;
    snapshot_.startFresh(lib, rng, kSlotCapture, 0);
    snapshot_.position = {kPlayfieldX, kPlayfieldY, 0.0f};  // (anchored top-left, scaled 3 x 3.5)
    captureRequest_ = true;
    captureReady_ = false;
}

void InGameMenu::captureRect(int& x, int& y, int& w, int& h) const {
    x = y = w = h = 0;
    if (!snapshot_.sprite) return;
    const AnmSprite& sp = *snapshot_.sprite;
    x = static_cast<int>(sp.startX);
    y = static_cast<int>(sp.startY);
    w = static_cast<int>(sp.endX - sp.startX);
    h = static_cast<int>(sp.endY - sp.startY);
}

InGameMenu::Request InGameMenu::update(const World& w, std::uint16_t pressed) {
    AnmLibrary& lib = *w.anm;
    lib_ = &lib;
    ZunRng& rng = *w.rng;
    GameState& g = *w.state;
    Request done = Request::None;
    const bool select = (pressed & (BtnShoot | BtnEnter)) != 0;
    const bool up = (pressed & BtnUp) != 0, down = (pressed & BtnDown) != 0;
    const bool mainGame = !g.practice && !g.spellPractice && g.difficulty < 4;
    if (mode_ == Mode::Pause) {
        bool hot = false;
        if ((pressed & BtnMenu) && state_ != State::ClosingResume) {
            w.playSound(kSeSelect);
            interruptAll(2, 0, 9);
            setState(State::ClosingResume);
            hot = true;
        }
        if ((pressed & BtnQ) && state_ != State::ClosingTitle) {
            w.playSound(kSeSelect);
            interruptAll(2, 0, 9);
            setState(State::ClosingTitle);
            hot = true;
        }
        if ((pressed & BtnR) && state_ != State::ClosingTitle && !hot) {
            w.playSound(kSeSelect);
            interruptAll(2, 0, 9);
            setState(State::ClosingRestart);
        }
        switch (state_) {
            case State::Opening:
                startSnapshot(lib, rng);
                for (int i = 0; i < 10; ++i) {
                    AnmVm& vm = vms_[static_cast<std::size_t>(i)];
                    vm = AnmVm{};
                    vm.startFresh(lib, rng, kSlotAscii, kPauseScripts + i);
                    visible_[static_cast<std::size_t>(i)] = true;
                }
                for (int i = 0; i < 4; ++i) vms_[static_cast<std::size_t>(i)].interrupt(1);
                // (A spell-practice Last Word card shows sprite 288: the card number is not
                // tracked yet.)
                vms_[7].selectSprite(283 + std::clamp(g.difficulty, 0, 4));
                visible_[8] = g.practice;
                visible_[9] = false;  // (the slow mode of the configuration copy: not ported)
                setState(State::OnResume);
                [[fallthrough]];
            case State::OnResume:
            case State::OnReturn:
            case State::OnRestart: {
                const int lit = state_ == State::OnResume ? 1 : state_ == State::OnReturn ? 2 : 3;
                for (int i = 1; i <= 3; ++i) vms_[static_cast<std::size_t>(i)].color = i == lit ? kLit : kDim;
                selected_ = lit;
                if (timer_ < 4) break;
                if (up || down) {
                    w.playSound(kSeMove);
                    if (state_ == State::OnResume) setState(up ? State::OnRestart : State::OnReturn);
                    else if (state_ == State::OnReturn) setState(up ? State::OnResume : State::OnRestart);
                    else setState(up ? State::OnReturn : State::OnResume);
                } else if (select) {
                    w.playSound(kSeSelect);
                    interruptAll(2, 0, 3);
                    if (state_ == State::OnResume) {
                        setState(State::ClosingResume);
                    } else {
                        for (int i = 4; i <= 6; ++i) vms_[static_cast<std::size_t>(i)].interrupt(1);
                        confirmYes_ = false;
                        setState(state_ == State::OnReturn ? State::ConfirmReturn : State::ConfirmRestart);
                    }
                }
                break;
            }
            case State::ConfirmReturn:
            case State::ConfirmRestart:
                vms_[5].color = confirmYes_ ? kYesNoLit : kDim;
                vms_[6].color = confirmYes_ ? kDim : kYesNoLit;
                selected_ = confirmYes_ ? 5 : 6;
                if (timer_ < 4) break;
                if (up || down) {
                    confirmYes_ = !confirmYes_;
                    w.playSound(kSeMove);
                } else if (select) {
                    w.playSound(kSeSelect);
                    if (confirmYes_) {
                        interruptAll(2, 4, 6);
                        setState(state_ == State::ConfirmReturn ? State::ClosingTitle : State::ClosingRestart);
                    } else {
                        interruptAll(1, 0, 3);
                        interruptAll(2, 4, 6);
                        setState(state_ == State::ConfirmReturn ? State::OnReturn : State::OnRestart);
                    }
                }
                break;
            case State::ClosingResume:
            case State::ClosingTitle:
            case State::ClosingRestart:
                selected_ = -1;
                if (timer_ >= 20) {
                    done = state_ == State::ClosingResume ? Request::Resume
                           : state_ == State::ClosingTitle ? Request::Title
                           : mainGame                      ? Request::Restart
                                                           : Request::Retry;
                    mode_ = Mode::None;
                    visible_.fill(false);
                }
                break;
            default: break;
        }
    } else if (mode_ == Mode::Continue) {
        // Practice: straight to the results. Main game at dawn or in 6B: the bad ending.
        if (g.practice && !g.spellPractice) {
            mode_ = Mode::None;
            return Request::Results;
        }
        switch (state_) {
            case State::Opening:
                if (mainGame && (g.nightSteps >= 11 || g.stage == 7)) {
                    mode_ = Mode::None;
                    return Request::BadEnding;
                }
                startSnapshot(lib, rng);
                for (int i = 0; i < 3; ++i) {
                    AnmVm& vm = vms_[static_cast<std::size_t>(10 + i)];
                    vm = AnmVm{};
                    vm.startFresh(lib, rng, kSlotAscii, kContinueScripts + i);
                    vm.interrupt(1);
                    visible_[static_cast<std::size_t>(10 + i)] = true;
                }
                vms_[13] = AnmVm{};
                if (mainGame && lib.loaded(kSlotTimes)) {  // (the clock only in the main game)
                    vms_[13].startFresh(lib, rng, kSlotTimes, 1);
                    vms_[13].selectSprite(g.nightSteps);
                    visible_[13] = true;
                }
                setState(State::Waiting);
                break;
            case State::Waiting:
                if (timer_ < 8) break;
                if (g.spellPractice) {  // No after a capture (Spellcard bit 9), else Yes
                    setState(w.spellcard && w.spellcard->has(Spellcard::LastCaptured) ? State::OnNo : State::OnYes);
                } else {
                    setState(mainGame || g.difficulty >= 4 ? State::OnNo : State::OnYes);
                }
                [[fallthrough]];
            case State::OnYes:
            case State::OnNo: {
                const bool yes = state_ == State::OnYes;
                vms_[11].color = yes ? kYesNoLit : kDim;
                vms_[12].color = yes ? kDim : kYesNoLit;
                selected_ = yes ? 11 : 12;
                if (timer_ < (yes ? 4 : 30)) break;
                if (up || down) {
                    w.playSound(kSeMove);
                    state_ = yes ? State::OnNo : State::OnYes;
                } else if (select) {
                    w.playSound(kSeSelect);
                    if (!yes) {
                        interruptAll(2, 10, 13);
                        setState(State::GivingUp);
                    } else if (mainGame) {
                        if (visible_[13]) vms_[13].interrupt(1);
                        setState(State::Continuing);
                    } else {
                        mode_ = Mode::None;
                        visible_.fill(false);
                        done = Request::Retry;
                    }
                }
                break;
            }
            case State::GivingUp:
                selected_ = -1;
                if (timer_ >= 20) {
                    mode_ = Mode::None;
                    visible_.fill(false);
                    done = Request::Results;
                }
                break;
            case State::Continuing:
                selected_ = -1;
                if (timer_ == 15) {
                    ++g.nightSteps;  // a continue costs 30 minutes of the night
                    if (visible_[13]) vms_[13].selectSprite(g.nightSteps);
                } else if (timer_ == 60) {
                    if (visible_[13]) vms_[13].interrupt(1);
                    interruptAll(3, 10, 12);
                } else if (timer_ >= 90) {
                    mode_ = Mode::None;
                    visible_.fill(false);
                    done = Request::Continue;
                }
                break;
            default: break;
        }
    }
    for (std::size_t i = 0; i < vms_.size(); ++i) {
        if (visible_[i]) vms_[i].tick(rng);
    }
    if (snapshot_.sprite) snapshot_.tick(rng);
    ++timer_;
    return done;
}

void InGameMenu::draw(SpriteSink& sink) {
    if (!active()) return;
    AnmDrawState none;
    auto drawItem = [&](int i) {
        if (!visible_[static_cast<std::size_t>(i)]) return;
        AnmVm& vm = vms_[static_cast<std::size_t>(i)];
        const Vec3f at = vm.position;
        if (i == selected_) vm.position = vm.position - Vec3f{4.0f, 4.0f, 0.0f};
        drawVm(sink, vm, none);
        vm.position = at;
    };
    if (captureReady_ && snapshot_.sprite) drawVm(sink, snapshot_, none);
    if (mode_ == Mode::Pause) {
        for (int i = 0; i <= 9; ++i) drawItem(i);
    } else {
        for (int i = 10; i <= 13; ++i) drawItem(i);
    }
}

}  // namespace touhou::th08
