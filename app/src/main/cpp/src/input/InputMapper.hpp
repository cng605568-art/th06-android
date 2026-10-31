#pragma once

#include <vector>

#include <SDL.h>

#include "game/Input.hpp"

namespace th06 {

// Merges every input source into one InputState per tick. All controllers, including
// the on-screen touch overlay (an SDL virtual game controller), are read through the
// SDL GameController API, so the game sees touch and real pads identically.
//   D-pad / left stick  move        A / right shoulder  shoot
//   B                   bomb        X / left shoulder / left trigger  focus
//   Start               pause
// Keyboard: arrows, Z shoot, X bomb, Shift focus, Esc/P pause.
class InputMapper {
  public:
    ~InputMapper();

    // Opens/closes controllers as they come and go. Returns true for a button press on
    // a physical (non-virtual) controller, which the app uses to hide the touch overlay.
    bool handleEvent(const SDL_Event& event, SDL_JoystickID touchJoystick);
    InputState sample() const;
    void closeAll();

  private:
    std::vector<SDL_GameController*> controllers_;
};

}  // namespace th06
