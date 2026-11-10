#pragma once

// A running game of Imperishable Night (spec_flow_menus §6): the game manager's stage
// set-up and the stage subsystems in the original's calc and draw order. Ported so far: the
// stage background, the effect file of the stage, the HUD panel and the stage music; the
// player, bullets, items, enemies, dialogue and the in-game menus come next.

#include <cstdint>
#include <string>

#include "th08/Anm.hpp"
#include "th08/Background.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

class Gameplay {
  public:
    static constexpr int kSlotEffects = 9;  // effNN.anm (the spell-card backgrounds)

    struct Setup {
        int stage = 0;  // stage index 0-8
        int character = 0;
        int difficulty = 1;
        bool practice = false, spellPractice = false;
        int lives = 2;  // the configured lives (the set-up applies the Extra and practice rules)
    };
    enum class Outcome : std::uint8_t { None, Title };

    explicit Gameplay(FlowContext& ctx) : ctx_(ctx) {}
    ~Gameplay();

    // The fresh set-up (spec 6.2 steps 1-14) and the stage registration.
    bool start(const Setup& setup, std::string* error);
    void end();
    // One pass with the menus' input (raw buttons).
    void tick(const MenuInput& in);
    void draw(SpriteSink& sink);

    Outcome outcome() const { return outcome_; }
    void requestTitle() { outcome_ = Outcome::Title; }
    const GameState& state() const { return state_; }
    const Background& background() const { return background_; }
    const Gui& gui() const { return gui_; }
    std::uint32_t stageFrame() const { return stageFrame_; }

  private:
    bool setUpStage(bool fresh, std::string* error);
    void releaseStageFiles();

    FlowContext& ctx_;
    Setup setup_;
    GameState state_;
    Background background_;
    Gui gui_;
    AsciiText ascii_;
    AnmDrawState drawState_;
    bool running_ = false;
    bool effectsLoaded_ = false;
    int musicStart_ = 0;  // GM+0x3DE28: 1 = play slot 0 when play begins
    std::uint32_t stageFrame_ = 0;
    Outcome outcome_ = Outcome::None;
};

}  // namespace touhou::th08
