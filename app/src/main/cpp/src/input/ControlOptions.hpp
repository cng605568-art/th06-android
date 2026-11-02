#pragma once

// The launcher's touch-control settings (Android) or their desktop defaults.

namespace th06 {

struct ControlOptions {
    bool drag = true;           // drag anywhere to move (false: the 8-way D-pad)
    float sensitivity = 1.5f;   // drag: playfield pixels per picture pixel of finger travel
    float buttonScale = 1.0f;   // on-screen button size
    float opacity = 1.0f;       // on-screen control opacity (0.4 .. 1)
    bool leftHanded = false;    // buttons on the left, movement on the right
    bool focusToggle = false;   // the Focus button latches instead of being held
    bool autoFire = false;      // Shot is held during play
    bool vibrate = true;        // a short vibration on a miss and on a bomb
    // Practice tools (port extras).
    int speedPercent = 100;     // slow motion (scores not saved below 100)
    bool showHitbox = false;    // a marker on the ship's hitbox
    bool saveStates = false;    // SAVE / LOAD (loading means the game's scores are not saved)
};

}  // namespace th06
