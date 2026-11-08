#pragma once

// The in-game pause and continue menus (run by the text/menu manager at logic priority 1;
// while one is up the game manager stops the rest of the logic list). Their sprites are
// ascii.anm's third entry, scripts 0x0FE + 0..14.

#include <array>
#include <cstdint>

#include "th07/Anm.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

class InGameMenu {
  public:
    enum class Request : std::uint8_t { None, Resume, Title, Restart, Results, Continue };

    void openPause(const World& w);
    void openContinue(const World& w, int maxContinues);
    bool active() const { return mode_ != Mode::None; }
    bool pauseMenu() const { return mode_ == Mode::Pause; }
    // One pass; `pressed` are the buttons newly down this pass. Returns what to do once the
    // menu has finished closing (None while it is still up).
    Request update(const World& w, std::uint16_t pressed);
    void draw(SpriteSink& sink);

    // The playfield snapshot behind both menus (capture.anm script 0x724): asked for on a
    // menu's opening pass, copied by the host from the frame just shown into `captureRect`
    // of capture.anm's texture.
    bool takeCaptureRequest() {
        const bool r = captureRequest_;
        captureRequest_ = false;
        return r;
    }
    void setCaptureReady(bool ready) { captureReady_ = ready; }
    // x, y = the backdrop sprite's start pixel, w = its pixel height and h = its width (the
    // original swaps them; capture.anm's sprite is square). Zero size before a menu opened.
    void captureRect(int& x, int& y, int& w, int& h) const;

  private:
    enum class Mode : std::uint8_t { None, Pause, Continue };
    enum class State : std::uint8_t {
        Opening, OnResume, OnReturn, OnRestart, ClosingResume, ConfirmReturn, ConfirmRestart, ClosingTitle, ClosingRestart,
        OnYes, OnNo, GivingUp, Continuing,
    };
    void setState(State s) {
        state_ = s;
        timer_ = 0;
    }
    void interruptAll(int which, int from, int to);
    void startBackground(const World& w);

    const World* world_ = nullptr;
    Mode mode_ = Mode::None;
    State state_ = State::Opening;
    int timer_ = 0;
    bool confirmYes_ = false;
    int maxContinues_ = 3;
    std::array<AnmVm, 15> vms_{};
    std::array<bool, 15> visible_{};
    AnmVm background_;
    bool backgroundLive_ = false;
    bool captureRequest_ = false;
    bool captureReady_ = false;
};

}  // namespace touhou::th07
