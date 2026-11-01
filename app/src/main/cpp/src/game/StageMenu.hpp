#pragma once

// The two in-game menus of the 2002 game, both drawn from ascii.anm scripts over a
// snapshot of the frozen playfield (capture.anm script 0):
//
//   Pause (Menu button): "Resume" / "Quit", Quit asks "Yes" / "No" (cursor on No). Input is
//   taken 4 passes after opening or switching lists; Menu resumes from anywhere, quitting
//   (Yes or the hidden Q key) and resuming both take 20 passes to close.
//   Continue (after the last life): "Yes" / "No" with the cursor on No and input locked for
//   30 passes, the credits left drawn as an ASCII digit beside the label. Yes closes in 30
//   passes, No in 20.
//
// While either menu is open the game is frozen: the stage runs the menu instead of the
// score popups, then stops the frame. Neither menu plays a sound or draws random numbers.

#include <array>
#include <cstdint>

#include "gfx/AnmManager.hpp"

namespace th06 {

class SpriteSink;

// Rising edges of this pass.
struct MenuInput {
    bool up = false, down = false, shoot = false, menu = false, quit = false;
};

// capture.anm (in the IN archive): a blank "@" texture the playfield snapshot is stretched
// into, drawn by its script 0.
inline constexpr int kAnmFileCapture = 3;
inline constexpr int kAnmOffsetCapture = 0x718;

class StageMenus {
  public:
    enum class Result { None, Resume, Quit, Continue, GiveUp };

    // Stage load: both menus closed, every sprite blank.
    void reset();

    bool pauseActive() const { return pauseActive_; }
    bool continueActive() const { return continueActive_; }
    bool frozen() const { return pauseActive_ || continueActive_; }

    // The game manager's pause check (Menu pressed, no menu up).
    void openPause() { pauseActive_ = true; }
    // The player ran out of lives: the continue menu's first pass is the next one.
    void openContinue() { continueActive_ = true; }

    // The menu manager's turn of one pass (only while a menu is up). `creditsLeft` is shown
    // by the continue menu. A Resume/Continue result means the world runs again this pass.
    Result update(AnmManager& anm, const MenuInput& in);

    // After the stage's own layers and HUD, clipped to the playfield. `captureReady`: the
    // snapshot texture holds the frozen playfield.
    void draw(AnmManager& anm, SpriteSink& sink, int creditsLeft, bool captureReady);

    // Set on a menu's opening pass: copy the playfield of the next rendered frame into the
    // capture texture.
    bool takeCaptureRequest() {
        const bool r = captureRequest_;
        captureRequest_ = false;
        return r;
    }

    // Inspection (tests).
    int pauseState() const { return pause_.state; }
    int continueState() const { return cont_.state; }
    int pauseTimer() const { return pause_.timer; }
    int continueTimer() const { return cont_.timer; }
    const AnmVm& pauseSlot(int i) const { return pause_.slots[static_cast<std::size_t>(i)]; }
    const AnmVm& continueSlot(int i) const { return cont_.slots[static_cast<std::size_t>(i)]; }
    const AnmVm& pauseBackground() const { return pause_.background; }

    // Pause states. A opening, B/C cursor on Resume/Quit, D closing to resume, E/F cursor
    // on Yes/No, G closing to quit.
    enum : int { PauseOpening, PauseResume, PauseQuit, PauseClosing, PauseYes, PauseNo, PauseQuitting };
    // Continue states: A opening, B cursor on Yes, C on No, D closing to continue, E to give up.
    enum : int { ContOpening, ContYes, ContNo, ContClosing, ContGivingUp };

  private:
    struct Record {
        int state = 0;
        int timer = 0;
        std::array<AnmVm, 6> slots{};
        AnmVm background;
    };

    Result updatePause(AnmManager& anm, const MenuInput& in);
    Result updateContinue(AnmManager& anm, const MenuInput& in);
    static void blank(Record& r);
    static void highlight(Record& r, int on, int off);
    void tick(AnmManager& anm, Record& r, int slots);

    Record pause_, cont_;
    bool pauseActive_ = false, continueActive_ = false;
    bool captureRequest_ = false;
};

}  // namespace th06
