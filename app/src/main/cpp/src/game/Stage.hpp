#pragma once

// One running stage of the 2002 game, loaded from the player's archives:
//   player00.anm (slot 5, 0x400)  etama3.anm (6, 0x200)  etama4.anm (7, 0x29a)
//   stgNenm.anm / stgNenm2.anm (8 / 9, 0x100)            ecldataN.ecl
// tick() advances exactly one 1/60 s game frame in the original subsystem order
// (player + bomb, enemies + timeline, effects, bullets + items, GUI, screen shake); render()
// draws the interpolated state.

#include <cstdint>
#include <memory>
#include <string>

#include "core/ZunMath.hpp"
#include "game/BulletManager.hpp"
#include "game/Ecl.hpp"
#include "game/EffectManager.hpp"
#include "game/Gui.hpp"
#include "game/EnemyManager.hpp"
#include "game/ItemManager.hpp"
#include "game/Input.hpp"
#include "game/Player.hpp"
#include "game/ScreenShake.hpp"
#include "game/StageBackground.hpp"
#include "game/ShotData.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextRenderer.hpp"
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
    // `text` renders the game's Shift-JIS strings (platform font); null uses the built-in
    // ASCII fallback.
    Stage(TextureLibrary& textures, const UiArt& ui, const Vfs* vfs, SoundPlayer* sound, TextRenderer* text = nullptr);

    // Loads stage 1..7 at the given difficulty. On failure the stage shows why instead.
    bool load(int stageNumber, int difficulty);
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }

    void tick(const InputState& input);
    void render(SpriteSink& sink, float alpha, const HudInfo& hud);

    // 0 Reimu A, 1 Reimu B, 2 Marisa A, 3 Marisa B. Takes effect on the next load().
    // Background music (null: silent). Set before load().
    void setMusic(MusicPlayer* music) {
        music_ = music;
        world_.music = music;
    }

    void setShotType(int shotType) { state_.shotType = shotType < 0 || shotType > 3 ? 0 : shotType; }

    bool paused() const { return paused_; }
    void setPaused(bool p) { paused_ = p; }

    // Diagnostics (tests, HUD, logs)
    const GameState& state() const { return state_; }
    const UnportedFeatures& unported() const { return unported_; }
    EnemyManager& enemies() { return *enemies_; }
    BulletManager& bullets() { return *bullets_; }
    ItemManager& items() { return *items_; }
    ScreenShake& shake() { return shake_; }
    EffectManager& effects() { return effects_; }
    Gui& gui() { return gui_; }
    World& world() { return world_; }
    StageBackground& background() { return background_; }
    Player& player() { return *player_; }
    AnmManager& anm() { return anm_; }
    int stageNumber() const { return stageNumber_; }

  private:
    void renderHud(SpriteSink& sink, const HudInfo& hud);
    void renderMessage(SpriteSink& sink);
    void renderBombName(SpriteSink& sink);
    void renderDiagnostics(SpriteSink& sink);

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
    ScreenShake shake_;
    EffectManager effects_;
    FallbackTextRenderer fallbackText_;
    Gui gui_;
    StageBackground background_;
    MusicPlayer* music_ = nullptr;
    bool guiLoadedOnce_ = false;
    int loadedGuiCharacter_ = -1;
    EclFile ecl_;
    ShotData shots_;
    bool shotsSearched_ = false;
    int loadedCharacter_ = -1;
    World world_;
    bool ready_ = false;
    bool paused_ = false;
    bool previousPause_ = false;
    std::string error_;
    int stageNumber_ = 1;
    int clearTimer_ = 0;
};

}  // namespace th06
