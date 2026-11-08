#pragma once

// A running game of Perfect Cherry Blossom: the game manager's state and stage set-up, and
// the stage subsystems in the original's logic and draw order. Systems not ported yet are
// simply absent (their World pointers stay null).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "th07/Anm.hpp"
#include "th07/Background.hpp"
#include "th07/Bullets.hpp"
#include "th07/Effects.hpp"
#include "th07/Enemies.hpp"
#include "th07/Player.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
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
    };
    struct Resources {
        const Vfs* vfs = nullptr;
        TextureLibrary* textures = nullptr;
        AnmLibrary* anm = nullptr;
        ZunRng* rng = nullptr;
        const ExeTables* tables = nullptr;
        SoundPlayer* sound = nullptr;
        BgmPlayer* music = nullptr;
    };

    Gameplay();
    ~Gameplay();
    // A fresh game from the setup's stage (the game manager's added hook).
    bool start(const Resources& res, const Setup& setup, std::string* error);
    // The next stage, keeping the game's state.
    bool nextStage(std::string* error);
    void end();

    void tick(std::uint16_t input);
    void draw(SpriteSink& sink);

    const GameState& state() const { return state_; }
    GameState& state() { return state_; }
    const Player& player() const { return *player_; }
    const EnemyManager& enemies() const { return *enemies_; }
    std::uint16_t stageSeed() const { return stageSeed_; }
    // Developer aid: test patterns and items every second (on top of the stage's enemies).
    void setTestPatterns(bool on) { testPatterns_ = on; }
    // The GUI and dialogue (owned by the host; may stay null).
    void setGui(StageGui* gui) { gui_ = world_.gui = gui; }

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
    AnmDrawState drawState_;
    std::uint16_t stageSeed_ = 0;
    bool running_ = false;
    bool etamaLoaded_ = false;
    bool testPatterns_ = false;
    StageGui* gui_ = nullptr;
};

}  // namespace touhou::th07
