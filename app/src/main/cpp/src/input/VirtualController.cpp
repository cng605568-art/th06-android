#include "input/VirtualController.hpp"

#include <algorithm>
#include <cmath>

#include "core/Log.hpp"
#include "core/Math.hpp"

namespace th06 {
namespace {
constexpr float kDpadDeadZone = 0.22f;   // fraction of the D-pad radius
constexpr float kAxisThreshold = 0.38f;  // ~sin(22.5 deg): 8 equal 45-degree sectors
constexpr int kVirtualButtons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
}  // namespace

VirtualController::~VirtualController() { detach(); }

bool VirtualController::attach(std::string* error) {
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = 0;
    desc.nbuttons = kVirtualButtons;
    // All of A..DPAD_RIGHT present: SDL then maps virtual button i to
    // SDL_GameControllerButton i, so pushes below can use the enum directly.
    desc.button_mask = (1u << kVirtualButtons) - 1;
    desc.name = "TH06 Touch Controls";

    const int deviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (deviceIndex < 0) {
        if (error) *error = std::string("SDL_JoystickAttachVirtualEx: ") + SDL_GetError();
        return false;
    }
    joystick_ = SDL_JoystickOpen(deviceIndex);
    if (!joystick_) {
        if (error) *error = std::string("SDL_JoystickOpen(virtual): ") + SDL_GetError();
        SDL_JoystickDetachVirtual(deviceIndex);
        return false;
    }
    log::info("Touch controls attached as virtual game controller #{}", SDL_JoystickInstanceID(joystick_));
    return true;
}

void VirtualController::detach() {
    if (!joystick_) return;
    const SDL_JoystickID id = SDL_JoystickInstanceID(joystick_);
    SDL_JoystickClose(joystick_);
    joystick_ = nullptr;
    for (int i = 0, n = SDL_NumJoysticks(); i < n; ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            SDL_JoystickDetachVirtual(i);
            break;
        }
    }
}

SDL_JoystickID VirtualController::joystickId() const {
    return joystick_ ? SDL_JoystickInstanceID(joystick_) : -1;
}

void VirtualController::layout(int screenWidth, int screenHeight, float gameX, float gameY,
                               float gameW, float gameH) {
    (void)gameY;
    (void)gameH;
    const float w = static_cast<float>(screenWidth);
    const float h = static_cast<float>(screenHeight);
    const float unit = std::min(w, h);
    const float leftBar = gameX;
    const float rightBar = w - (gameX + gameW);
    overlay_ = std::min(leftBar, rightBar) < unit * 0.30f;

    Button& dpad = controls_[kDpad];
    Button& shoot = controls_[kShoot];
    Button& bomb = controls_[kBomb];
    Button& focus = controls_[kFocus];
    Button& pause = controls_[kPause];

    if (!overlay_) {
        dpad.r = std::min(leftBar * 0.42f, unit * 0.24f);
        dpad.x = leftBar * 0.5f;
        dpad.y = h * 0.64f;
        const float cx = w - rightBar * 0.5f;
        const float d = std::min(rightBar * 0.22f, unit * 0.13f);
        shoot.r = d * 0.95f;
        shoot.x = cx + d * 0.55f;
        shoot.y = h * 0.64f;
        focus.r = d * 0.80f;
        focus.x = cx - d * 1.05f;
        focus.y = h * 0.64f + d * 0.95f;
        bomb.r = d * 0.80f;
        bomb.x = cx - d * 0.55f;
        bomb.y = h * 0.64f - d * 1.55f;
        pause.r = unit * 0.05f;
        pause.x = cx;
        pause.y = unit * 0.09f;
    } else {
        dpad.r = unit * 0.17f;
        dpad.x = dpad.r * 1.25f;
        dpad.y = h - dpad.r * 1.25f;
        const float d = unit * 0.085f;
        shoot.r = d;
        shoot.x = w - d * 1.4f;
        shoot.y = h - d * 1.6f;
        focus.r = d * 0.85f;
        focus.x = shoot.x - d * 2.3f;
        focus.y = shoot.y + d * 0.3f;
        bomb.r = d * 0.85f;
        bomb.x = shoot.x - d * 0.6f;
        bomb.y = shoot.y - d * 2.2f;
        pause.r = unit * 0.045f;
        pause.x = w - pause.r * 1.6f;
        pause.y = pause.r * 1.6f;
    }

    shoot.button = SDL_CONTROLLER_BUTTON_A;
    shoot.label = "SHOT";
    shoot.color = rgba(235, 70, 95);
    bomb.button = SDL_CONTROLLER_BUTTON_B;
    bomb.label = "BOMB";
    bomb.color = rgba(80, 150, 255);
    focus.button = SDL_CONTROLLER_BUTTON_X;
    focus.label = "FOCUS";
    focus.color = rgba(245, 205, 70);
    pause.button = SDL_CONTROLLER_BUTTON_START;
    pause.label = "II";
    pause.color = rgba(180, 180, 190);
    dpad.color = rgba(220, 220, 235);
}

VirtualController::Control VirtualController::hitTest(float x, float y, bool buttonsOnly) const {
    Control best = kNone;
    float bestScore = 1e30f;
    for (int i = buttonsOnly ? kShoot : kDpad; i < kControlCount; ++i) {
        const Button& b = controls_[i];
        const float slack = i == kDpad ? 1.3f : 1.25f;  // generous targets for thumbs
        const float d = std::hypot(x - b.x, y - b.y);
        if (d <= b.r * slack && d / b.r < bestScore) {
            bestScore = d / b.r;
            best = static_cast<Control>(i);
        }
    }
    return best;
}

bool VirtualController::handleEvent(const SDL_Event& e, int screenWidth, int screenHeight) {
    if (e.type != SDL_FINGERDOWN && e.type != SDL_FINGERMOTION && e.type != SDL_FINGERUP) return false;
    const float x = e.tfinger.x * static_cast<float>(screenWidth);
    const float y = e.tfinger.y * static_cast<float>(screenHeight);

    auto find = [&](SDL_FingerID id) -> Finger* {
        for (Finger& f : fingers_) {
            if (f.active && f.id == id) return &f;
        }
        return nullptr;
    };

    bool consumed = false;
    if (e.type == SDL_FINGERDOWN) {
        visible_ = true;  // touching the screen brings the overlay back
        const Control c = hitTest(x, y, false);
        if (c != kNone) {
            for (Finger& f : fingers_) {
                if (!f.active) {
                    f = Finger{e.tfinger.fingerId, c, x, y, true};
                    break;
                }
            }
            consumed = true;
        }
    } else if (Finger* f = find(e.tfinger.fingerId)) {
        if (e.type == SDL_FINGERUP) {
            f->active = false;
        } else {
            f->x = x;
            f->y = y;
            // A D-pad finger keeps steering wherever it goes; a button finger can slide
            // from one button onto another (or off, releasing it).
            if (f->control != kDpad) f->control = hitTest(x, y, true);
        }
        consumed = true;
    }
    if (consumed) recompute();
    return consumed;
}

void VirtualController::recompute() {
    bool held[kControlCount] = {};
    bool dirs[4] = {};
    knobActive_ = false;
    const Button& dpad = controls_[kDpad];

    for (const Finger& f : fingers_) {
        if (!f.active || f.control == kNone) continue;
        held[f.control] = true;
        if (f.control != kDpad) continue;
        const float dx = f.x - dpad.x, dy = f.y - dpad.y;
        const float len = std::hypot(dx, dy);
        knobActive_ = true;
        const float clamp = std::min(len, dpad.r * 0.75f);
        knobX_ = len > 0 ? dpad.x + dx / len * clamp : dpad.x;
        knobY_ = len > 0 ? dpad.y + dy / len * clamp : dpad.y;
        if (len < dpad.r * kDpadDeadZone) continue;
        dirs[0] = dirs[0] || dy / len < -kAxisThreshold;
        dirs[1] = dirs[1] || dy / len > kAxisThreshold;
        dirs[2] = dirs[2] || dx / len < -kAxisThreshold;
        dirs[3] = dirs[3] || dx / len > kAxisThreshold;
    }

    for (int i = kShoot; i < kControlCount; ++i) {
        controls_[i].down = held[i];
        push(controls_[i].button, held[i]);
    }
    std::copy(std::begin(dirs), std::end(dirs), dirs_);
    push(SDL_CONTROLLER_BUTTON_DPAD_UP, dirs[0]);
    push(SDL_CONTROLLER_BUTTON_DPAD_DOWN, dirs[1]);
    push(SDL_CONTROLLER_BUTTON_DPAD_LEFT, dirs[2]);
    push(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, dirs[3]);
}

void VirtualController::push(SDL_GameControllerButton button, bool down) {
    if (!joystick_ || sent_[button] == down) return;
    sent_[button] = down;
    SDL_JoystickSetVirtualButton(joystick_, button, down ? SDL_PRESSED : SDL_RELEASED);
}

void VirtualController::releaseAll() {
    for (Finger& f : fingers_) f.active = false;
    recompute();
}

void VirtualController::setVisible(bool visible) {
    if (!visible) releaseAll();
    visible_ = visible;
}

bool VirtualController::locate(SDL_GameControllerButton button, float& x, float& y) const {
    const Button& d = controls_[kDpad];
    const float off = d.r * 0.7f;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: x = d.x; y = d.y - off; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: x = d.x; y = d.y + off; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: x = d.x - off; y = d.y; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: x = d.x + off; y = d.y; return true;
        default: break;
    }
    for (int i = kShoot; i < kControlCount; ++i) {
        if (controls_[i].button == button) {
            x = controls_[i].x;
            y = controls_[i].y;
            return true;
        }
    }
    return false;
}

void VirtualController::draw(SpriteSink& sink, const UiArt& ui) const {
    if (!visible_) return;
    const std::uint8_t base = overlay_ ? 70 : 110;
    const std::uint8_t lit = overlay_ ? 150 : 200;

    // D-pad: ring, four direction chevrons, and a knob following the thumb.
    const Button& d = controls_[kDpad];
    sink.drawRegion(ui.disc, d.x, d.y, d.r * 2 / ui.disc.width, rgba(30, 30, 45, base));
    sink.drawRegion(ui.ring, d.x, d.y, d.r * 2 / ui.ring.width, rgba(220, 220, 235, lit));
    const float arrowScale = d.r * 0.42f / ui.chevron.width;
    const float off = d.r * 0.66f;
    const float angles[4] = {0.0f, kPi, -kPi * 0.5f, kPi * 0.5f};  // up, down, left, right
    const float ox[4] = {0, 0, -off, off};
    const float oy[4] = {-off, off, 0, 0};
    for (int i = 0; i < 4; ++i) {
        sink.drawRegion(ui.chevron, d.x + ox[i], d.y + oy[i], arrowScale,
                        dirs_[i] ? rgba(255, 255, 255, 240) : rgba(220, 220, 235, base + 30),
                        angles[i]);
    }
    if (knobActive_) {
        sink.drawRegion(ui.disc, knobX_, knobY_, d.r * 0.55f / ui.disc.width, rgba(255, 255, 255, lit));
    }

    for (int i = kShoot; i < kControlCount; ++i) {
        const Button& b = controls_[i];
        const std::uint32_t c = b.color;
        const auto r = static_cast<std::uint8_t>(c), g = static_cast<std::uint8_t>(c >> 8),
                   bl = static_cast<std::uint8_t>(c >> 16);
        sink.drawRegion(ui.disc, b.x, b.y, b.r * 2 / ui.disc.width, rgba(r, g, bl, b.down ? lit : base));
        sink.drawRegion(ui.ring, b.x, b.y, b.r * 2 / ui.ring.width, rgba(r, g, bl, lit));
        const float scale = std::max(1.0f, std::floor(b.r / 16.0f));
        const float tw = textWidth(b.label, scale);
        drawText(sink, ui, b.x - tw * 0.5f, b.y - 3.5f * scale, b.label, scale,
                 rgba(255, 255, 255, b.down ? 255 : 220));
    }
}

}  // namespace th06
