#include "input/InputMapper.hpp"

#include <algorithm>

#include "core/Log.hpp"

namespace th06 {
namespace {
constexpr Sint16 kStickDeadZone = 12000;  // ~37% of full deflection
constexpr Sint16 kTriggerThreshold = 16000;
}  // namespace

InputMapper::~InputMapper() { closeAll(); }

bool InputMapper::handleEvent(const SDL_Event& e, SDL_JoystickID touchJoystick) {
    switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) {
                controllers_.push_back(c);
                const char* name = SDL_GameControllerName(c);
                log::info("Controller connected: {}", name ? name : "(unnamed)");
            }
            return false;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            const auto it = std::find_if(controllers_.begin(), controllers_.end(), [&](SDL_GameController* c) {
                return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == e.cdevice.which;
            });
            if (it != controllers_.end()) {
                SDL_GameControllerClose(*it);
                controllers_.erase(it);
                log::info("Controller disconnected");
            }
            return false;
        }
        case SDL_CONTROLLERBUTTONDOWN:
            return e.cbutton.which != touchJoystick;
        default:
            return false;
    }
}

InputState InputMapper::sample() const {
    InputState s;
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    s.up = keys[SDL_SCANCODE_UP];
    s.down = keys[SDL_SCANCODE_DOWN];
    s.left = keys[SDL_SCANCODE_LEFT];
    s.right = keys[SDL_SCANCODE_RIGHT];
    s.shoot = keys[SDL_SCANCODE_Z];
    s.bomb = keys[SDL_SCANCODE_X];
    s.focus = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    s.pause = keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_P];
    s.skip = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];

    for (SDL_GameController* c : controllers_) {
        auto button = [c](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
        const Sint16 lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        s.up = s.up || button(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -kStickDeadZone;
        s.down = s.down || button(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > kStickDeadZone;
        s.left = s.left || button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -kStickDeadZone;
        s.right = s.right || button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > kStickDeadZone;
        s.shoot = s.shoot || button(SDL_CONTROLLER_BUTTON_A) || button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        s.bomb = s.bomb || button(SDL_CONTROLLER_BUTTON_B);
        s.focus = s.focus || button(SDL_CONTROLLER_BUTTON_X) || button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
                  SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold;
        s.pause = s.pause || button(SDL_CONTROLLER_BUTTON_START);
        s.skip = s.skip || button(SDL_CONTROLLER_BUTTON_Y);
    }
    return s;
}

void InputMapper::closeAll() {
    for (SDL_GameController* c : controllers_) SDL_GameControllerClose(c);
    controllers_.clear();
}

}  // namespace th06
