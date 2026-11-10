#pragma once

// Imperishable Night's in-game menus (claude/th08-specs/spec_flow_menus.md §7): the pause menu
// and the continue menu, run by the AsciiManager while the GameManager stops the simulation.
// Their sprites are ascii.anm scripts 12-21 (pause) and 22-24 (continue), plus the night
// clock (times.anm script 1) on the continue menu: a continue costs 30 minutes of the night.

#include <array>
#include <cstdint>

#include "th08/Anm.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

struct GameState;
struct World;

class InGameMenu {
  public:
    // What the game does once the menu has closed.
    enum class Request : std::uint8_t {
        None,
        Resume,      // back to play
        Title,       // quit to the title
        Restart,     // main game: from Stage 1 with a fresh set-up (scene 10)
        Retry,       // practice, spell practice, Extra: the same stage again (scene 11)
        Results,     // the run is over (scene 6; the result screen is not ported yet)
        BadEnding,   // dawn: no continue offered (scene 9; the ending is not ported yet)
        Continue,    // a continue was taken
    };

    void openPause(const World& w);
    void openContinue(const World& w);
    bool active() const { return mode_ != Mode::None; }
    bool pauseMenu() const { return mode_ == Mode::Pause; }
    // One pass; `pressed` are the buttons newly down this pass. Returns what to do once the
    // menu has finished closing (None while it is up).
    Request update(const World& w, std::uint16_t pressed);
    void draw(SpriteSink& sink);

  private:
    enum class Mode : std::uint8_t { None, Pause, Continue };
    enum class State : std::uint8_t {
        Opening, OnResume, OnReturn, OnRestart, ClosingResume, ConfirmReturn, ConfirmRestart, ClosingTitle, ClosingRestart,
        Waiting, OnYes, OnNo, GivingUp, Continuing,
    };
    void setState(State s) {
        state_ = s;
        timer_ = 0;
    }
    void interruptAll(int which, int from, int to);

    Mode mode_ = Mode::None;
    State state_ = State::Opening;
    int timer_ = 0;
    bool confirmYes_ = false;
    AnmLibrary* lib_ = nullptr;
    static constexpr int kVms = 14;  // 0-9 pause, 10-12 continue, 13 the night clock
    std::array<AnmVm, kVms> vms_{};
    std::array<bool, kVms> visible_{};
    int selected_ = -1;  // the highlighted row (drawn offset by (-4, -4))
};

}  // namespace touhou::th08
