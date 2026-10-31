#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <SDL.h>

#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {

// On-screen controls: an 8-way D-pad plus Shoot, Bomb, Focus and Pause.
//
// The overlay registers itself with SDL as a *virtual game controller*
// (SDL_JoystickAttachVirtualEx, type GAMECONTROLLER) and translates multi-touch into
// SDL_JoystickSetVirtualButton calls. SDL then emits ordinary SDL_CONTROLLERBUTTONDOWN/UP
// events and controller state for it, exactly as for a physical gamepad:
//   D-pad -> DPAD_UP/DOWN/LEFT/RIGHT, Shoot -> A, Bomb -> B, Focus -> X, Pause -> START.
//
// Layout: on wide screens the 4:3 picture leaves side bars, and the controls live there
// (D-pad left, buttons right) so they never cover bullets; on near-4:3 screens they are
// drawn translucent over the picture instead.
class VirtualController {
  public:
    ~VirtualController();

    bool attach(std::string* error);
    void detach();
    SDL_JoystickID joystickId() const;

    // gameX..gameH: where the 640x480 picture sits on the screen, in pixels.
    void layout(int screenWidth, int screenHeight, float gameX, float gameY, float gameW, float gameH);
    // Consumes finger events; returns true when a touch landed on the controls.
    bool handleEvent(const SDL_Event& event, int screenWidth, int screenHeight);
    void releaseAll();

    void draw(SpriteSink& sink, const UiArt& ui) const;
    // Screen position of a control (D-pad directions: a point on that side of the pad).
    bool locate(SDL_GameControllerButton button, float& x, float& y) const;
    bool visible() const { return visible_; }
    void setVisible(bool visible);

  private:
    enum Control : int { kNone = -1, kDpad = 0, kShoot, kBomb, kFocus, kPause, kControlCount };

    struct Button {
        SDL_GameControllerButton button = SDL_CONTROLLER_BUTTON_A;
        float x = 0, y = 0, r = 0;
        const char* label = "";
        std::uint32_t color = 0;
        bool down = false;
    };
    struct Finger {
        SDL_FingerID id = 0;
        Control control = kNone;
        float x = 0, y = 0;
        bool active = false;
    };

    Control hitTest(float x, float y, bool buttonsOnly) const;
    void recompute();
    void push(SDL_GameControllerButton button, bool down);

    SDL_Joystick* joystick_ = nullptr;
    std::array<bool, SDL_CONTROLLER_BUTTON_MAX> sent_{};
    std::array<Button, kControlCount> controls_{};  // kDpad entry holds the D-pad circle
    std::array<Finger, 10> fingers_{};
    bool dirs_[4] = {};  // up, down, left, right
    float knobX_ = 0, knobY_ = 0;
    bool knobActive_ = false;
    bool overlay_ = false;
    bool visible_ = true;
};

}  // namespace th06
