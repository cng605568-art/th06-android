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
#include "th07/Gui.hpp"
#include "th07/InGameMenu.hpp"
#include "th07/Player.hpp"
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

// Screen shake jobs (the screen-effect pass, priority 15): each job draws two u32 values in
// range 3 per frame; the last job's offset moves everything drawn before the GUI.
class ScreenShake final : public ShakeSink {
  public:
    void shake(int duration, float from, float to) override { jobs_.push_back({duration, 0, from, to}); }
    void update(ZunRng& rng, std::uint32_t stageFrame, float& offsetX, float& offsetY);
    void clear() { jobs_.clear(); }

  private:
    struct Job {
        int duration, timer;
        float from, to;
    };
    std::vector<Job> jobs_;
};

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
    enum class Outcome : std::uint8_t { None, Title, Restart, Results, Ending };
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
    };

    Gameplay();
    ~Gameplay();
    // A fresh game from the setup's stage (the game manager's added hook).
    bool start(const Resources& res, const Setup& setup, std::string* error);
    // The next stage, keeping the game's state.
    bool nextStage(std::string* error);
    void end();

    void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    void tick(std::uint16_t input);
    // The GUI's "go to the next stage" request (stages 1-5), acted on by the supervisor.
    bool nextStagePending() const { return running_ && gui_->nextStageRequested(); }
    // After a tick: performs a pending next-stage request at once (no supervisor).
    bool stageAdvance(std::string* error);
    Outcome outcome() const { return outcome_; }
    bool running() const { return running_; }
    bool finished() const { return outcome_ != Outcome::None; }
    bool paused() const { return menu_.active(); }
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
    ScreenShake shake_;
    std::unique_ptr<Gui> gui_ = std::make_unique<Gui>();
    std::uint16_t prevInput_ = 0;
    InGameMenu menu_;
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
