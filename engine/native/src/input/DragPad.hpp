#pragma once

// Drag-to-move: one finger anywhere off the buttons steers like a touchpad. The ship follows
// the finger's travel since it touched down (scaled to playfield pixels); the same travel
// also gives 8-way directions around the touch-down point (a floating joystick), which the
// menus use. Plain logic, no SDL.

#include <cstdint>

namespace touhou {

class DragPad {
  public:
    // gamePerScreen: playfield pixels per screen pixel of finger travel (sensitivity
    // included). deadZone: screen pixels before a direction counts (menus only).
    void configure(float gamePerScreen, float deadZone) {
        scale_ = gamePerScreen;
        deadZone_ = deadZone;
    }
    void down(float x, float y);
    void move(float x, float y);
    void up() { active_ = false; }

    bool active() const { return active_; }
    unsigned id() const { return id_; }
    float offsetX() const { return active_ ? (x_ - startX_) * scale_ : 0.0f; }
    float offsetY() const { return active_ ? (y_ - startY_) * scale_ : 0.0f; }
    // up, down, left, right
    void directions(bool out[4]) const;
    float startX() const { return startX_; }
    float startY() const { return startY_; }
    float x() const { return x_; }
    float y() const { return y_; }

  private:
    bool active_ = false;
    unsigned id_ = 0;
    float startX_ = 0, startY_ = 0, x_ = 0, y_ = 0;
    float scale_ = 1.0f, deadZone_ = 24.0f;
};

}  // namespace touhou
