#pragma once

// A running game of Perfect Cherry Blossom: the game manager's state and stage set-up, and
// the stage subsystems in the original's logic and draw order. Systems not ported yet are
// simply absent (their World pointers stay null).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "th07/Anm.hpp"
#include "th07/Background.hpp"
#include "th07/Bullets.hpp"
#include "th07/Effects.hpp"
#include "th07/Enemies.hpp"
#include "th07/GameConfig.hpp"
#include "th07/Gui.hpp"
#include "th07/InGameMenu.hpp"
#include "th07/Player.hpp"
#include "th07/Replay.hpp"
#include "th07/ScreenEffects.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
class TextRenderer;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th07 {

class BgmPlayer;
class SoundPlayer;
struct ExeTables;

class Gameplay {
  public:
    struct Setup {
        int stage = 1;  // 1-6, 7 Extra, 8 Phantasm
        int shotIndex = 0;
        int difficulty = Normal;
        bool practice = false;
        int lifeCount = 2;
        bool slowMode = false;
        int maxContinues = 3;  // credits by total play time (3, 4 or 5)
        bool restart = false;  // a pause-menu restart (counts a retry)
        // The live configuration: the game's configuration copy is made from it (with the
        // life count and slow mode above), and a recording stores that copy.
        GameConfig config;
        // Live play: the recording (owned by the supervisor; it outlives the game). Null: none.
        ReplayRecorder* recorder = nullptr;
        ExeIdentity exe;  // what a recording's summary carries
        // Playback: the replay (owned by the supervisor), its playback mode (0 normal, 1 the
        // recorded slowdown, 2 fast-forward outside boss fights) and the attract demo.
        const ReplayFile* playback = nullptr;
        int playbackMode = 0;
        bool demo = false;
        int demoIndex = 0;
    };
    // What the flow around the game provides (all optional): score.dat and the play
    // statistics, at the points of the stage set-up and play where the original touches them.
    struct Hooks {
        // Fresh set-up, step 7: score.dat is rewritten (two RNG draws) and read (hi-score,
        // spell history, clear records, practice bests).
        std::function<void(GameState&)> readScores;
        // Fresh set-up, step 8 (live play, slow mode off): a play (and a retry, a practice).
        std::function<void(const GameState&, bool restart)> countPlay;
        // Step 9 (live play): stage `state.stage` entered.
        std::function<void(const GameState&)> stageEntered;
        // Spell history in live play (practice included).
        std::function<void(int card, const std::string& name)> spellDeclared;
        std::function<void(int card, std::uint32_t bonus)> spellCaptured;
        std::function<void(const GameState&)> continued;  // a continue was used
        // Play time: counted up to now (pause, continue menu) and re-based (menus closed,
        // after a continue, at every stage set-up).
        std::function<void()> playTimeAdd;
        std::function<void()> playTimeRebase;
    };
    // What the game asks the supervisor for (once set, it stays until the next start).
    enum class Outcome : std::uint8_t { None, Title, Restart, Results, Ending, ReplayEnd };
    struct Resources {
        const Vfs* vfs = nullptr;
        TextureLibrary* textures = nullptr;
        AnmLibrary* anm = nullptr;
        ZunRng* rng = nullptr;
        const ExeTables* tables = nullptr;
        SoundPlayer* sound = nullptr;
        BgmPlayer* music = nullptr;
        TextRenderer* text = nullptr;
        const UiArt* ui = nullptr;
        const English* english = nullptr;  // the English text mode (null: the original text)
    };

    Gameplay();
    ~Gameplay();
    // A fresh game from the setup's stage (the game manager's added hook).
    bool start(const Resources& res, const Setup& setup, std::string* error);
    // The next stage, keeping the game's state.
    bool nextStage(std::string* error);
    void end();

    void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    // One pass with the raw button word. True: the replay's fast-forward asks for the whole
    // logic list to run again in this displayed frame (spec 7.8.6).
    bool tick(std::uint16_t raw);
    // The GUI's "go to the next stage" request (stages 1-5), acted on by the supervisor.
    bool nextStagePending() const { return running_ && gui_->nextStageRequested(); }
    // After a tick: performs a pending next-stage request at once (no supervisor).
    bool stageAdvance(std::string* error);
    Outcome outcome() const { return outcome_; }
    bool running() const { return running_; }
    bool finished() const { return outcome_ != Outcome::None; }
    bool paused() const { return menu_.active(); }
    // The pass was stopped by slow mode (the supervisor merges the next pass's input).
    bool slowSkipped() const { return slowSkip_; }
    bool replay() const { return playback_ != nullptr; }
    bool demo() const { return demo_; }
    std::int32_t demoFrame() const { return demoFrame_; }
    // The game input the player, bombs and dialogue read (spec 1.5.2).
    std::uint16_t gameInput() const { return gameInput_; }
    const ReplayFeed& feed() const { return feed_; }
    // Desync diagnostics: after each played-back frame (1-based within the stage), the entry
    // recorded in that frame and this frame's own event flags (the flags match while the
    // simulation follows the recording, apart from 0x0100 "paused").
    std::function<void(int stage, std::int32_t frame, const ReplayEntry& recorded, std::uint32_t flags)> onPlaybackFrame;
    // The pause and continue menus.
    InGameMenu& inGameMenu() { return menu_; }
    // The host's playfield capture, one request at a time: the pause/continue menus' backdrop
    // or the GUI's stage-transition snapshot (different rectangles of capture.anm's texture).
    bool takeCaptureRequest();
    void captureRect(int& x, int& y, int& w, int& h) const;
    void setCaptureReady(bool ready);
    // Opens the pause menu (Android back button), as Menu pressed would.
    void requestPause() { pauseRequested_ = true; }
    void draw(SpriteSink& sink);

    const GameState& state() const { return state_; }
    GameState& state() { return state_; }
    const Player& player() const { return *player_; }
    const EnemyManager& enemies() const { return *enemies_; }
    std::uint16_t stageSeed() const { return stageSeed_; }
    // Developer aid: test patterns and items every second (on top of the stage's enemies).
    void setTestPatterns(bool on) { testPatterns_ = on; }

  private:
    bool setUpStage(bool fresh, std::string* error);
    void restoreReplayStage();
    void startRecordingBlock();
    bool fastForward() const;
    bool loadEffectFiles(std::string* error);
    void releaseStageFiles();

    Resources res_;
    Setup setup_;
    GameState state_;
    World world_;
    std::unique_ptr<Player> player_;
    Effects effects_;
    std::unique_ptr<BulletManager> bullets_ = std::make_unique<BulletManager>();
    std::unique_ptr<EnemyManager> enemies_ = std::make_unique<EnemyManager>();
    Background background_;
    ScreenEffects screenFx_;
    std::unique_ptr<Gui> gui_ = std::make_unique<Gui>();
    std::uint16_t prevRaw_ = 0;
    // The game input (latched at the end of a live pass, fed at its start in playback) and
    // the previous pass's.
    std::uint16_t gameInput_ = 0, prevGameInput_ = 0;
    GameConfig config_;  // the configuration copy
    ReplayRecorder* recorder_ = nullptr;
    const ReplayFile* playback_ = nullptr;
    ReplayFeed feed_;
    int playbackMode_ = 0;
    bool demo_ = false;
    int demoIndex_ = 0;
    std::int32_t demoFrame_ = 0;
    bool wasPaused_ = false;  // the "was paused" flag of the recorder
    bool slowSkip_ = false;
    int slowLag_ = 0, replayLag_ = 0;
    InGameMenu menu_;
    enum class CaptureOwner : std::uint8_t { None, Menu, Gui };
    CaptureOwner captureOwner_ = CaptureOwner::None;
    bool pauseRequested_ = false;
    Outcome outcome_ = Outcome::None;
    Hooks hooks_;
    void applyContinue();
    AnmDrawState drawState_;
    std::uint16_t stageSeed_ = 0;
    bool running_ = false;
    bool etamaLoaded_ = false;
    bool testPatterns_ = false;
};

}  // namespace touhou::th07
