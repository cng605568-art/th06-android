#include "th07/InGameMenu.hpp"

#include "gfx/SpriteSink.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {
constexpr int kScripts = 0x0FE;
constexpr int kBackground = 0x724;  // capture.anm
constexpr int kSelect = 10, kMove = 12, kShooting = 0, kPaused = 37;
constexpr std::uint32_t kLit = 0xFFFFFFFFu, kDim = 0x80303030u, kYesNoLit = 0xFFFF8080u, kYesNoDim = 0x80808080u;
}  // namespace

void InGameMenu::interruptAll(int which, int from, int to) {
    for (int i = from; i <= to; ++i) {
        if (visible_[static_cast<std::size_t>(i)]) vms_[static_cast<std::size_t>(i)].interrupt(which);
    }
}

void InGameMenu::startBackground(const World& w) {
    background_ = AnmVm{};
    backgroundLive_ = w.anm->script(kBackground) != nullptr;
    captureReady_ = false;
    if (!backgroundLive_) return;
    background_.startFresh(*w.anm, *w.rng, kBackground);
    background_.position = Vec3f{static_cast<float>(kPlayfieldX), static_cast<float>(kPlayfieldY), 0.0f};
    captureRequest_ = true;
}

void InGameMenu::captureRect(int& x, int& y, int& w, int& h) const {
    x = y = w = h = 0;
    if (!backgroundLive_ || !background_.sprite) return;
    const AnmSprite& sp = *background_.sprite;
    x = static_cast<int>(sp.startX);
    y = static_cast<int>(sp.startY);
    w = static_cast<int>(sp.endY - sp.startY);  // [QUIRK] height as the width
    h = static_cast<int>(sp.endX - sp.startX);
}

void InGameMenu::openPause(const World& w) {
    world_ = &w;
    mode_ = Mode::Pause;
    setState(State::Opening);
    w.playSound(kPaused);
}

void InGameMenu::openContinue(const World& w, int maxContinues) {
    world_ = &w;
    mode_ = Mode::Continue;
    maxContinues_ = maxContinues;
    setState(State::Opening);
}

InGameMenu::Request InGameMenu::update(const World& w, std::uint16_t pressed) {
    world_ = &w;
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    const GameState& g = *w.state;
    Request done = Request::None;
    const bool select = (pressed & (BtnShoot | BtnEnter)) != 0;
    const bool up = (pressed & BtnUp) != 0, down = (pressed & BtnDown) != 0;
    if (mode_ == Mode::Pause) {
        const bool closingResume = state_ == State::ClosingResume, closingTitle = state_ == State::ClosingTitle;
        bool hot = false;
        if ((pressed & BtnMenu) && !closingResume) {
            w.playSound(kSelect);
            interruptAll(2, 0, 9);
            background_.interrupt(1);
            setState(State::ClosingResume);
            hot = true;
        }
        if ((pressed & BtnQ) && !closingTitle) {
            w.playSound(kSelect);
            interruptAll(2, 0, 9);
            setState(State::ClosingTitle);
            hot = true;
        }
        if ((pressed & BtnR) && !g.replay && state_ != State::ClosingTitle && !hot) {
            w.playSound(kSelect);
            interruptAll(2, 0, 9);
            setState(State::ClosingRestart);
        }
        switch (state_) {
            case State::Opening:
                for (int i = 0; i < 10; ++i) {
                    AnmVm& vm = vms_[static_cast<std::size_t>(i)];
                    vm = AnmVm{};
                    vm.startFresh(lib, rng, kScripts + i);
                    visible_[static_cast<std::size_t>(i)] = true;
                }
                for (int i = 0; i < 4; ++i) vms_[static_cast<std::size_t>(i)].interrupt(1);
                vms_[7].selectSprite(lib, kScripts + 15 + g.difficulty);
                visible_[8] = g.practice;
                visible_[9] = g.slowMode;
                if (g.replay) vms_[3].current = nullptr;
                startBackground(w);
                setState(State::OnResume);
                [[fallthrough]];
            case State::OnResume:
            case State::OnReturn:
            case State::OnRestart: {
                const int lit = state_ == State::OnResume ? 1 : state_ == State::OnReturn ? 2 : 3;
                for (int i = 1; i <= 3; ++i) vms_[static_cast<std::size_t>(i)].color = i == lit ? kLit : kDim;
                if (timer_ < 4) break;
                if (up || down) {
                    w.playSound(kShooting);
                    if (state_ == State::OnResume) setState(up ? (g.replay ? State::OnReturn : State::OnRestart) : State::OnReturn);
                    else if (state_ == State::OnReturn) setState(up ? State::OnResume : (g.replay ? State::OnResume : State::OnRestart));
                    else setState(up ? State::OnReturn : State::OnResume);
                } else if (select) {
                    w.playSound(kSelect);
                    interruptAll(2, 0, 3);
                    if (state_ == State::OnResume) {
                        background_.interrupt(1);
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
                vms_[5].color = confirmYes_ ? kYesNoLit : kYesNoDim;
                vms_[6].color = confirmYes_ ? kYesNoDim : kYesNoLit;
                if (timer_ < 4) break;
                if (up || down) {
                    confirmYes_ = !confirmYes_;
                    w.playSound(kShooting);
                } else if (select) {
                    w.playSound(kSelect);
                    if (confirmYes_) {
                        interruptAll(2, 4, 6);
                        setState(state_ == State::ConfirmReturn ? State::ClosingTitle : State::ClosingRestart);
                    } else {
                        interruptAll(1, 0, 3);
                        interruptAll(2, 4, 6);
                        const State back = state_ == State::ConfirmReturn ? State::OnReturn : State::OnRestart;
                        setState(back);
                    }
                }
                break;
            case State::ClosingResume:
            case State::ClosingTitle:
            case State::ClosingRestart:
                if (timer_ >= 20) {
                    done = state_ == State::ClosingResume ? Request::Resume : state_ == State::ClosingTitle ? Request::Title : Request::Restart;
                    mode_ = Mode::None;
                    visible_.fill(false);
                }
                break;
            default: break;
        }
    } else if (mode_ == Mode::Continue) {
        if (g.practice || g.replay || g.difficulty >= Extra || (g.continues >= maxContinues_ && !g.cheat(MischiefContinues))) {
            mode_ = Mode::None;
            return Request::Results;
        }
        switch (state_) {
            case State::Opening:
                for (int i = 0; i < 5; ++i) {
                    AnmVm& vm = vms_[static_cast<std::size_t>(10 + i)];
                    vm = AnmVm{};
                    vm.startFresh(lib, rng, kScripts + 10 + i);
                    vm.interrupt(1);
                    visible_[static_cast<std::size_t>(10 + i)] = true;
                }
                vms_[14].selectSprite(lib, 0x092 + 116 + (g.cheat(MischiefContinues) ? maxContinues_ : std::max(0, maxContinues_ - g.continues)));
                startBackground(w);
                // [QUIRK] The state moves on to "cursor on No", but this pass still runs the
                // "cursor on Yes" colours (input is locked at timer 0 anyway).
                setState(State::OnNo);
                vms_[12].color = kYesNoLit;
                vms_[13].color = kYesNoDim;
                break;
            case State::OnYes:
            case State::OnNo:
                vms_[12].color = state_ == State::OnYes ? kYesNoLit : kYesNoDim;
                vms_[13].color = state_ == State::OnYes ? kYesNoDim : kYesNoLit;
                if ((up || down) && timer_ >= (state_ == State::OnNo ? 30 : 4)) {
                    w.playSound(kMove);
                    state_ = state_ == State::OnNo ? State::OnYes : State::OnNo;
                } else if (select && timer_ >= (state_ == State::OnNo ? 30 : 4)) {
                    w.playSound(kSelect);
                    interruptAll(2, 10, 14);
                    if (state_ == State::OnYes) background_.interrupt(1);
                    setState(state_ == State::OnYes ? State::Continuing : State::GivingUp);
                }
                break;
            case State::GivingUp:
                if (timer_ >= 20) {
                    mode_ = Mode::None;
                    visible_.fill(false);
                    done = Request::Results;
                }
                break;
            case State::Continuing:
                if (timer_ >= 30) {
                    mode_ = Mode::None;
                    visible_.fill(false);
                    done = Request::Continue;
                }
                break;
            default: break;
        }
    }
    for (std::size_t i = 0; i < vms_.size(); ++i) {
        if (visible_[i]) vms_[i].tick(lib, rng);
    }
    if (backgroundLive_) background_.tick(lib, rng);
    ++timer_;
    return done;
}

void InGameMenu::draw(SpriteSink& sink) {
    if (!active() || !world_) return;
    AnmDrawState none;
    sink.setClip(kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH);
    // Colours were set by the cursor states. [QUIRK] The (-4, -4) offsets the original
    // stores on the highlighted rows are never drawn.
    auto drawItem = [&](int i) {
        if (visible_[static_cast<std::size_t>(i)]) drawVm(sink, vms_[static_cast<std::size_t>(i)], none);
    };
    const bool snapshot = captureReady_ && backgroundLive_;
    if (mode_ == Mode::Pause) {
        if (snapshot && state_ != State::Opening) drawVm(sink, background_, none);
        for (int i = 0; i <= 9; ++i) drawItem(i);
    } else {
        if (snapshot && (state_ != State::Opening || timer_ > 2)) drawVm(sink, background_, none);
        // [QUIRK] The credits sprite is drawn once more while a cursor is shown (its blend
        // doubles).
        if (state_ == State::OnYes || state_ == State::OnNo) drawItem(14);
        for (int i = 10; i <= 14; ++i) drawItem(i);
    }
    sink.clearClip();
}

}  // namespace touhou::th07
