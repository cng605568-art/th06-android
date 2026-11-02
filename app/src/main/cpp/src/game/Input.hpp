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
    bool skip = false;  // dialogue fast-forward (Ctrl on a keyboard)
    bool quit = false;  // Q: quits from the pause menu without asking (keyboard only)
    // Drag-to-move (touch): the finger's offset from where it touched down, in playfield
    // pixels (sensitivity applied). A new dragId means a new touch: the ship re-anchors.
    bool saveState = false, loadState = false;  // practice save states (F5 / F9, pad L3 / R3, touch)
    bool drag = false;
    unsigned dragId = 0;
    float dragX = 0.0f, dragY = 0.0f;
};

}  // namespace th06
