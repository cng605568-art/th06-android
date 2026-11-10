#pragma once

// A running game of Imperishable Night (spec_flow_menus §6): the game manager's stage
// set-up and the stage subsystems in the original's calc and draw order. Ported so far: the
// stage background, the player (movement, focus, familiars, shots), the effect manager's
// allocation and lists, the HUD panel and the stage music; bullets, items, enemies, dialogue
// and the in-game menus come next.

#include <cstdint>
#include <memory>
#include <string>

#include "th08/Anm.hpp"
#include "th08/Background.hpp"
#include "th08/Effects.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"
#include "th08/Player.hpp"
#include "th08/World.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

class Gameplay {
  public:
    static constexpr int kSlotEffects = 9;  // effNN.anm (the spell-card backgrounds)
    static constexpr int kSlotEtama = 6;    // etama.anm (bullets and effects)

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
    const Player& player() const { return *player_; }
    const Effects& effects() const { return *effects_; }
    // The game input word (spec_player 2.1): the raw buttons the game reads.
    std::uint16_t gameInput() const { return gameInput_; }
    std::uint32_t stageFrame() const { return stageFrame_; }

  private:
    bool setUpStage(bool fresh, std::string* error);
    void releaseStageFiles();

    FlowContext& ctx_;
    Setup setup_;
    GameState state_;
    World world_;
    Background background_;
    std::unique_ptr<Player> player_ = std::make_unique<Player>();
    std::unique_ptr<Effects> effects_ = std::make_unique<Effects>();
    Gui gui_;
    std::uint16_t gameInput_ = 0;
    AsciiText ascii_;
    AnmDrawState drawState_;
    bool running_ = false;
    bool effectsLoaded_ = false, etamaLoaded_ = false;
    int musicStart_ = 0;  // GM+0x3DE28: 1 = play slot 0 when play begins
    std::uint32_t stageFrame_ = 0;
    Outcome outcome_ = Outcome::None;
};

}  // namespace touhou::th08
