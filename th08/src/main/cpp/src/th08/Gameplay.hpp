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
#include "th08/ExeTables.hpp"
#include "th08/Replay.hpp"
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
        bool restart = false;  // scene 10: the main game restarted from the pause menu
        int card = -1;         // spell practice: the card (0-based)
        // Playback (spec_replays 3): the file (kept alive by the caller); character,
        // difficulty, card and the first stage come from it.
        const ReplayFile* replay = nullptr;
        int playbackMode = 0;  // 0 normal, 1 the recorded slowdown, 2 x5 without a boss
        int demoIndex = 0;     // with `demo`: which demo (its fade and end frames)
        bool demo = false;     // the attract demo: no pause menu
    };
    // What the game hands over: the title, the ending (scene 9) or the result screen (6).
    enum class Outcome : std::uint8_t { None, Title, Ending, Results, ReplayEnd };

    explicit Gameplay(FlowContext& ctx) : ctx_(ctx) {}
    ~Gameplay();

    // The fresh set-up (spec 6.2 steps 1-14) and the stage registration.
    bool start(const Setup& setup, std::string* error);
    void end();
    // One pass with the menus' input (raw buttons).
    void tick(const MenuInput& in);
    void draw(SpriteSink& sink);

    Outcome outcome() const { return outcome_; }
    bool menuOpen() const { return menu_.active(); }
    bool replaying() const { return replay_ != nullptr; }
    // The one-pass input delay of live play (spec_replays 2.2) is the original's; unit tests
    // that drive the player frame by frame turn it off.
    void setInputDelay(bool on) { inputDelay_ = on; }
    std::uint16_t stageSeed() const { return stageSeed_; }
    std::size_t replayFrame() const { return replayFrame_; }
    // The replayed FPS sample (SUP+0x198) and its flag (bit 7, SUP+0x33C).
    int replayFps() const { return replayFps_; }
    bool replayFpsFlagged() const { return replayFpsFlag_; }
    // Playback speed (spec_replays 3.4): run another whole pass this frame (skippable dialogue
    // x3 in every mode, mode 2 x5 while no boss is present).
    bool runAgain() const;
    // The run being recorded (live play; spec_replays 2): valid from the first stage's set-up.
    bool recording() const { return recordValid_; }
    const ReplayFile& recorded() const { return record_; }
    // The GameManager's removal (spec 2.3): a 0 entry closes the stage being played.
    void closeRecording();
    void requestTitle() { outcome_ = Outcome::Title; }
    // The stage end (spec 6.3), raised by the stage-clear sequence (stages 1-5) or the
    // dialogue (6A, 6B, Extra).
    void stageCleared() {
        if (stageEnd_ == 0) stageEnd_ = 2;
    }
    int stageEndState() const { return stageEnd_; }
    // Android Back: what Menu does (opens the pause menu, or closes it).
    void requestPause() { pauseRequested_ = true; }
    // The app going to the background: opens the pause menu unless a menu is already up.
    void pauseNow() {
        if (!menu_.active()) pauseRequested_ = true;
    }
    const GameState& state() const { return state_; }
    const Background& background() const { return background_; }
    const Gui& gui() const { return gui_; }
    Gui& gui() { return gui_; }
    // The host's playfield capture, one request at a time: the menus' snapshot or the Gui's
    // (results snapshot, transition tiles).
    bool takeCaptureRequest();
    void captureRect(int& x, int& y, int& w, int& h) const;
    void setCaptureReady(bool ready);
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
    std::uint32_t pressShotFrames_ = 0;
    enum class CaptureOwner : std::uint8_t { None, Menu, Gui } captureOwner_ = CaptureOwner::None;
    enum class Pending : std::uint8_t { None, Results, BadEnding, Ending } pending_ = Pending::None;
    Gui gui_;
    std::uint16_t gameInput_ = 0;
    AsciiText ascii_;
    AnmDrawState drawState_;
    bool running_ = false;
    bool effectsLoaded_ = false, etamaLoaded_ = false;
    int musicStart_ = 0;  // GM+0x3DE28: 1 = play slot 0 when play begins
    bool keepMusic_ = false;  // a spell-practice retry whose boss music keeps playing
    // Replays (spec_replays): the file played back, its entries read this stage; live play's
    // raw input of the previous pass (the one-pass input delay, spec 2.2); the stage seed.
    const ReplayFile* replay_ = nullptr;
    std::size_t replayFrame_ = 0;
    std::uint16_t rawPending_ = 0;
    // The FPS block read along (one sample per 30 entries), mode 1's pass counter (GM+0x3DE08),
    // the replayed-FPS text (refreshed about twice a second) and the demo marker (ascii.anm
    // script 11, spec_replay_menu 4).
    std::size_t fpsIndex_ = 0;
    int replayFps_ = 60;
    bool replayFpsFlag_ = false;
    std::uint32_t slowCounter_ = 0;
    std::string fpsText_;
    int fpsTextTimer_ = 0;
    AnmVm demoVm_;
    bool demoVmOn_ = false;
    int demoCounter_ = 0;  // GM+0x3DBB8
    // Slow mode (spec_flow 6.6 step 10, TH07's rule): the lag counter, and the skip flag that
    // stays raised into the next pass (no pause check then).
    std::uint32_t slowLag_ = 0;
    bool slowSkip_ = false;
    bool inputDelay_ = true;
    ReplayFile record_;
    bool recordValid_ = false, retrying_ = false;
    std::size_t recordFrames_ = 0;
    int resultsEntries_ = 0;
    void recordStageStart();
    void recordPass(std::uint16_t raw);
    std::uint16_t stageSeed_ = 0;
    void restoreReplayStage();
    const ExeTables::BossMusic* bossMusic() const;  // spell practice: the card's record
    std::uint32_t stageFrame_ = 0;
    Outcome outcome_ = Outcome::None;
};

}  // namespace touhou::th08
