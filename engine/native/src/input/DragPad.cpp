#include "input/DragPad.hpp"

#include <cmath>

namespace touhou {

void DragPad::down(float x, float y) {
    active_ = true;
    ++id_;
    if (id_ == 0) id_ = 1;  // 0 means "no drag yet"
    startX_ = x_ = x;
    startY_ = y_ = y;
}

void DragPad::move(float x, float y) {
    if (!active_) return;
    x_ = x;
    y_ = y;
}

void DragPad::directions(bool out[4]) const {
    out[0] = out[1] = out[2] = out[3] = false;
    if (!active_) return;
    const float dx = x_ - startX_, dy = y_ - startY_;
    const float len = std::hypot(dx, dy);
    if (len < deadZone_) return;
    constexpr float kAxis = 0.38f;  // 8 equal 45-degree sectors
    out[0] = dy / len < -kAxis;
    out[1] = dy / len > kAxis;
    out[2] = dx / len < -kAxis;
    out[3] = dx / len > kAxis;
}

}  // namespace touhou
