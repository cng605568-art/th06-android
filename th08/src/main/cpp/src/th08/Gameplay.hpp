#pragma once

// A running game of Imperishable Night (spec_flow_menus §6): the game manager's stage
// set-up, the stage subsystems in the original's calc and draw order, the in-game menus, and
// the stage end with the next stage's set-up (the dialogue and the stage-clear picture that
// raise the stage end come with the MSG port).

#include <cstdint>
#include <memory>
#include <string>

#include "th08/Anm.hpp"
#include "th08/Background.hpp"
#include "th08/Bullets.hpp"
#include "th08/Effects.hpp"
#include "th08/Enemies.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"
#include "th08/InGameMenu.hpp"
#include "th08/Msg.hpp"
#include "th08/ScreenEffects.hpp"
#include "th08/Spellcard.hpp"
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
    // The stage end (spec 6.3), raised by the stage-clear sequence (stages 1-5) or the
    // dialogue (6A, 6B, Extra).
    void stageCleared() {
        if (stageEnd_ == 0) stageEnd_ = 2;
    }
    int stageEndState() const { return stageEnd_; }
    // Android Back: what Menu does (opens the pause menu, or closes it).
    void requestPause() { pauseRequested_ = true; }
    const GameState& state() const { return state_; }
    const Background& background() const { return background_; }
    const Gui& gui() const { return gui_; }
    const Player& player() const { return *player_; }
    const Effects& effects() const { return *effects_; }
    const ScreenEffects& screenEffects() const { return screen_; }
    const InGameMenu& menu() const { return menu_; }
    const Dialogue& dialogue() const { return dialogue_; }
    BulletManager& bullets() { return *bullets_; }
    EnemyManager& enemies() { return *enemies_; }
    const World& world() const { return world_; }
    // The game input word (spec_player 2.1): the raw buttons the game reads.
    std::uint16_t gameInput() const { return gameInput_; }
    std::uint32_t stageFrame() const { return stageFrame_; }

  private:
    bool setUpStage(bool fresh, std::string* error);
    void menuDone(InGameMenu::Request request);
    bool stageEndStep(const MenuInput& in);  // true: the pass stops here
    bool nextStage(std::string* error);
    bool loadFaces(bool fresh, std::string* error);
    void releaseStageFiles();

    FlowContext& ctx_;
    Setup setup_;
    GameState state_;
    World world_;
    Background background_;
    std::unique_ptr<Player> player_ = std::make_unique<Player>();
    std::unique_ptr<Effects> effects_ = std::make_unique<Effects>();
    std::unique_ptr<BulletManager> bullets_ = std::make_unique<BulletManager>();
    std::unique_ptr<EnemyManager> enemies_ = std::make_unique<EnemyManager>();
    ScreenEffects screen_;
    InGameMenu menu_;
    Dialogue dialogue_;
    Spellcard spellcard_;
    // The game's spell card records (CATK; copied from score.dat at the game start).
    std::unique_ptr<std::array<CardRecord, kCardCount>> cards_ = std::make_unique<std::array<CardRecord, kCardCount>>();
    bool facesLoaded_ = false, bossFacesLoaded_ = false;
    std::uint16_t prevGameInput_ = 0;
    Effect* moon_ = nullptr;  // the 6B moon (special effect slot 12)
    bool pauseRequested_ = false;
    // Stage end (spec 6.3): 0 playing, 2 just ended, 3 waiting for Shoot, 1 the next stage
    // loaded and waiting for Shoot.
    int stageEnd_ = 0;
    enum class Pending : std::uint8_t { None, Results, BadEnding, Ending } pending_ = Pending::None;
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
