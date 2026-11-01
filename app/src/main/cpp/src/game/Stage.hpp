#pragma once

// One running stage of the 2002 game, loaded from the player's archives:
//   player00.anm (slot 5, 0x400)  etama3.anm (6, 0x200)  etama4.anm (7, 0x29a)
//   stgNenm.anm / stgNenm2.anm (8 / 9, 0x100)            ecldataN.ecl
// tick() advances exactly one 1/60 s game frame in the original subsystem order
// (player, enemies + timeline, bullets); render() draws the interpolated state.

#include <cstdint>
#include <memory>
#include <string>

#include "core/ZunMath.hpp"
#include "game/BulletManager.hpp"
#include "game/Ecl.hpp"
#include "game/EnemyManager.hpp"
#include "game/ItemManager.hpp"
#include "game/Input.hpp"
#include "game/Player.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextureLibrary.hpp"

namespace th06 {

class SoundPlayer;
class Vfs;

struct HudInfo {
    float fps = 0;
    std::uint32_t drawCalls = 0;
};

class Stage {
  public:
    Stage(TextureLibrary& textures, const UiArt& ui, const Vfs* vfs, SoundPlayer* sound);

    // Loads stage 1..7 at the given difficulty. On failure the stage shows why instead.
    bool load(int stageNumber, int difficulty);
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }

    void tick(const InputState& input);
    void render(SpriteSink& sink, float alpha, const HudInfo& hud);

    bool paused() const { return paused_; }
    void setPaused(bool p) { paused_ = p; }

    // Diagnostics (tests, HUD, logs)
    const GameState& state() const { return state_; }
    const UnportedFeatures& unported() const { return unported_; }
    EnemyManager& enemies() { return *enemies_; }
    BulletManager& bullets() { return *bullets_; }
    ItemManager& items() { return *items_; }
    Player& player() { return *player_; }
    AnmManager& anm() { return anm_; }
    int stageNumber() const { return stageNumber_; }

  private:
    void renderHud(SpriteSink& sink, const HudInfo& hud);
    void renderMessage(SpriteSink& sink);

    SoundPlayer* sound_;
    const Vfs* vfs_;
    UiArt ui_;
    ZunRng rng_;
    GameState state_;
    UnportedFeatures unported_;
    AnmManager anm_;
    std::unique_ptr<BulletManager> bullets_;
    std::unique_ptr<EnemyManager> enemies_;
    std::unique_ptr<ItemManager> items_;
    std::unique_ptr<Player> player_;
    EclFile ecl_;
    World world_;
    bool ready_ = false;
    bool paused_ = false;
    bool previousPause_ = false;
    std::string error_;
    int stageNumber_ = 1;
    int clearTimer_ = 0;
};

}  // namespace th06
