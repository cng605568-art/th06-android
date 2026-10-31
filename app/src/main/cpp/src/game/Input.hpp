#pragma once

namespace th06 {

// Logical controls for one tick, merged from every source (touch overlay via its virtual
// SDL game controller, physical gamepads, keyboard).
struct InputState {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool shoot = false;
    bool focus = false;
    bool bomb = false;
    bool pause = false;
};

}  // namespace th06
