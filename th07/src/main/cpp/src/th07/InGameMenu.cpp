#include "th07/InGameMenu.hpp"

#include "gfx/SpriteSink.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {
constexpr int kScripts = 0x0FE;
constexpr int kSelect = 10, kMove = 12, kShooting = 0, kPaused = 37;
}  // namespace

void InGameMenu::interruptAll(int which, int from, int to) {
    for (int i = from; i <= to; ++i) {
        if (visible_[static_cast<std::size_t>(i)]) vms_[static_cast<std::size_t>(i)].interrupt(which);
    }
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
                setState(State::OnResume);
                break;
            case State::OnResume:
            case State::OnReturn:
            case State::OnRestart:
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
                        setState(State::ClosingResume);
                    } else {
                        for (int i = 4; i <= 6; ++i) vms_[static_cast<std::size_t>(i)].interrupt(1);
                        confirmYes_ = false;
                        setState(state_ == State::OnReturn ? State::ConfirmReturn : State::ConfirmRestart);
                    }
                }
                break;
            case State::ConfirmReturn:
            case State::ConfirmRestart:
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
        if (g.practice || g.replay || g.difficulty >= Extra || g.continues >= maxContinues_) {
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
                vms_[14].selectSprite(lib, 0x092 + 116 + (maxContinues_ - g.continues));
                setState(State::OnNo);
                break;
            case State::OnYes:
            case State::OnNo:
                if ((up || down) && timer_ >= (state_ == State::OnNo ? 30 : 4)) {
                    w.playSound(kMove);
                    state_ = state_ == State::OnNo ? State::OnYes : State::OnNo;
                } else if (select && timer_ >= (state_ == State::OnNo ? 30 : 4)) {
                    w.playSound(kSelect);
                    interruptAll(2, 10, 14);
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
    ++timer_;
    return done;
}

void InGameMenu::draw(SpriteSink& sink) {
    if (!active() || !world_) return;
    AnmDrawState none;
    sink.setClip(kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH);
    auto drawItem = [&](int i, bool selected, std::uint32_t dim, std::uint32_t lit) {
        if (!visible_[static_cast<std::size_t>(i)]) return;
        AnmVm vm = vms_[static_cast<std::size_t>(i)];
        if (selected) {
            vm.position.x -= 4.0f;
            vm.position.y -= 4.0f;
            vm.color = lit;
        } else if (dim != 0) {
            vm.color = dim;
        }
        drawVm(sink, vm, none);
    };
    if (mode_ == Mode::Pause) {
        const bool rows = state_ == State::OnResume || state_ == State::OnReturn || state_ == State::OnRestart;
        drawItem(0, false, 0, 0);
        drawItem(1, state_ == State::OnResume, rows ? 0x80303030u : 0, 0xFFFFFFFFu);
        drawItem(2, state_ == State::OnReturn, rows ? 0x80303030u : 0, 0xFFFFFFFFu);
        drawItem(3, state_ == State::OnRestart, rows ? 0x80303030u : 0, 0xFFFFFFFFu);
        const bool confirm = state_ == State::ConfirmReturn || state_ == State::ConfirmRestart;
        drawItem(4, false, 0, 0);
        drawItem(5, confirm && confirmYes_, confirm ? 0x80808080u : 0, 0xFFFF8080u);
        drawItem(6, confirm && !confirmYes_, confirm ? 0x80808080u : 0, 0xFFFF8080u);
        for (int i = 7; i <= 9; ++i) drawItem(i, false, 0, 0);
    } else {
        const bool rows = state_ == State::OnYes || state_ == State::OnNo;
        drawItem(10, false, 0, 0);
        drawItem(11, false, 0, 0);
        drawItem(12, rows && state_ == State::OnYes, rows ? 0x80808080u : 0, 0xFFFF8080u);
        drawItem(13, rows && state_ == State::OnNo, rows ? 0x80808080u : 0, 0xFFFF8080u);
        drawItem(14, false, 0, 0);
    }
    sink.clearClip();
}

}  // namespace touhou::th07
