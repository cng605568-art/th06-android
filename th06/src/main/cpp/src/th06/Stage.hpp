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
#include "th06/BulletManager.hpp"
#include "th06/Ecl.hpp"
#include "th06/EffectManager.hpp"
#include "th06/Gui.hpp"
#include "th06/EnemyManager.hpp"
#include "th06/ItemManager.hpp"
#include "input/InputState.hpp"
#include "th06/Player.hpp"
#include "th06/ScreenShake.hpp"
#include "th06/StageBackground.hpp"
#include "th06/StageMenu.hpp"
#include "th06/ResultScreen.hpp"
#include "th06/Ending.hpp"
#include "th06/GameConfig.hpp"
#include "th06/MusicRoom.hpp"
#include "th06/ScoreFile.hpp"
#include "th06/ShotData.hpp"
#include "th06/Replay.hpp"
#include "th06/Title.hpp"
#include "th06/Translation.hpp"
#include "th06/World.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/Image.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextRenderer.hpp"
#include "gfx/TextureLibrary.hpp"

namespace touhou {
class Vfs;
}  // namespace touhou

namespace touhou::th06 {

class SoundPlayer;

struct HudInfo {
    float fps = 0;
    std::uint32_t drawCalls = 0;
};

class Stage {
  public:
    // `text` renders the game's Shift-JIS strings (platform font); null uses the built-in
    // ASCII fallback.
    Stage(TextureLibrary& textures, const UiArt& ui, const Vfs* vfs, SoundPlayer* sound, TextRenderer* text = nullptr);

    // Opens the game's title screen (the menus before a game); every game then returns to
    // it. False if the title files are missing (then start a game with quickStart()).
    bool openTitle(bool firstVisit = true, int cursor = -1, Title::Entry entry = Title::Entry::Normal);
    bool inTitle() const { return inTitle_; }
    const Title& title() const { return title_; }
    // The title's own screens: the Music Room and the score viewer (Result).
    bool inMusicRoom() const { return inMusicRoom_; }
    const MusicRoom& musicRoom() const { return musicRoom_; }
    // The title or one of its screens is up (Android Back is Cancel there).
    bool inMenus() const { return inTitle_ || inMusicRoom_ || (inResults_ && results_.titleMode()); }
    // A new game straight away, without the title: stage 1..7 at the given difficulty, with
    // the shot type set by setShotType(). `stageSelect` (the launcher's stage select): it
    // plays on from that stage with the stage's practice power and saves nothing (no high
    // scores, clear progress or spell history); otherwise it is an ordinary game.
    bool quickStart(int stageNumber, int difficulty, bool stageSelect = false);
    bool stageSelectRun() const { return stageSelect_; }
    // Loads stage 1..7 at the given difficulty (also the next stage of a game). On failure
    // the stage shows why instead.
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
    // Touch option: Shot is held for the ship during play (the dialogue still takes taps).
    void setAutoFire(bool on) { autoFire_ = on; }
    // Practice tools (port extras): slow motion (percent of normal speed; below 100 the
    // game's scores are not saved), a hitbox marker, save states.
    void setPracticeTools(int speedPercent, bool showHitbox, bool saveStates) {
        speedPercent_ = speedPercent;
        showHitbox_ = showHitbox;
        saveStates_ = saveStates;
    }
    // A save state of the stage in play (false: nothing to save now). Loading one makes the
    // game unrecordable; it is refused once the stage or its loaded files have changed.
    bool saveState();
    bool loadState();
    bool hasSaveState() const {
        return save_ && save_->valid && save_->loadSerial == loadSerial_ && save_->generation == anm_.generation();
    }
    // Replays: where replay/th6_NN.rpy live (empty: no saving, no replay list).
    void setReplayDir(std::string dir) {
        replayDir_ = std::move(dir);
        title_.setReplayDir(replayDir_);
    }
    // Plays a replay file from stage index 0..6 (the demo: from the archives, without
    // stage music or pause). False if the file is invalid or lacks that stage.
    bool startReplay(std::span<const std::uint8_t> file, int stageIndex, bool demo = false);
    bool replaying() const { return playback_ != nullptr; }
    bool demoPlaying() const { return demo_; }
    // The recording of the game in play (live games only).
    const Replay* recording() const { return recording_.get(); }
    // The game's text in another language: translation patch folders, lowest priority first
    // (empty: the original Japanese). Takes effect for text drawn from now on.
    void setTranslation(std::vector<std::string> folders) {
        translation_.setStack(std::move(folders));
        world_.translation = translation_.active() ? &translation_ : nullptr;
    }
    const Translation& translation() const { return translation_; }
    // The game RNG's seed (the original takes the clock at start-up).
    void seedRandom(std::uint16_t seed) { rng_.seed(seed); }
    // The launcher's mischief options (Mischief bits). Set before the first load().
    void setMischief(std::uint32_t bits) { state_.mischief = bits & 31u; }

    // A menu (pause or continue) is up and the game is frozen.
    bool paused() const { return menus_.frozen(); }
    // The Menu button from outside the input state (Android Back): counts as a press on the
    // next frame.
    void pressMenu() { menuPress_ = true; }
    // The app went to the background: open the pause menu (unless a menu is already up).
    // The original simply stopped; pausing is kinder to a player coming back.
    void pauseNow() {
        if (ready_ && !inTitle_ && !inResults_ && !inEnding_ && !menus_.frozen()) menus_.openPause();
    }
    // The player chose Quit on the title (or, without the title, left the game): back to
    // the launcher.
    bool quitRequested() const { return quitRequested_; }
    // The playfield snapshot behind the menus: the app copies the last rendered playfield
    // into `captureTexture()` when asked and reports success.
    bool takeCaptureRequest() { return menus_.takeCaptureRequest(); }
    void setCaptureReady(bool ready) { captureReady_ = ready; }
    // -1 when capture.anm is not loaded.
    int captureTexture() const { return captureTexture_; }
    const StageMenus& menus() const { return menus_; }
    // Where score.dat is kept (the app's own storage). Read at the start of a game (the
    // player's PC score.dat in the game folder is used if the port has none yet) and
    // written when the result screen closes. Empty: scores are not saved.
    void setScorePath(std::string path) { scorePath_ = std::move(path); }
    // Where th06.cfg (the Option menu's settings) is kept: read now, written when the
    // settings change. Empty: defaults, not saved.
    void setConfigPath(std::string path);
    const GameConfig& config() const { return config_; }
    GameConfig& config() { return config_; }
    TitleSession& session() { return session_; }
    const ScoreFile& scores() const { return scores_; }
    bool inResults() const { return inResults_; }
    const ResultScreen& results() const { return results_; }
    ResultScreen& results() { return results_; }
    // Practice Start (or a quick start at stage 2..6): one stage, its best score saved.
    bool practiceRun() const { return practiceRun_; }
    bool recordable() const { return recordable_; }
    // Developer hooks: the game ends now, as after giving up (desktop --demo-gameover), or
    // as when the last stage's boss dialogue ends (the ending, then the results).
    void endGameForTesting() {
        if (ready_ && !inTitle_ && !inResults_ && !inEnding_) gameOver(false);
    }
    void clearGameForTesting() {
        if (ready_ && !inTitle_ && !inResults_ && !inEnding_) gameCleared();
    }
    bool inEnding() const { return inEnding_; }
    const Ending& ending() const { return ending_; }

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
    void runMenus(const MenuInput& edges);
    void applyContinue();
    void startRun();
    void gameOver(bool cleared);
    void gameCleared();
    void startEnding();
    void saveScores();
    void writeScores();
    void loadScores();
    bool tickTitleScreens(const InputState& in, bool menuPress);
    void saveConfig();
    void applyConfig();
    void tickTitle(const InputState& in, bool menuPress);
    void startFromTitle();
    void newRun(int stageNumber, int lives, int bombs, bool practice);
    void practiceOver();
    void leaveGame();

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
    StageMenus menus_;
    InputState previousInput_;
    bool clearInput_ = false;  // the pass after a stage transition sees no input
    bool menuPress_ = false;
    bool quitRequested_ = false;
    bool captureReady_ = false;
    int captureTexture_ = -1;
    ScoreFile scores_;
    std::string scorePath_;
    int runStartStage_ = 0;  // 0 until the first stage of the game is loaded
    bool practiceRun_ = false;
    bool stageSelect_ = false;  // started from the launcher's stage select: nothing is saved
    bool autoFire_ = false;
    int speedPercent_ = 100;
    bool saveEdge_ = false, loadEdge_ = false;
    bool showHitbox_ = false, saveStates_ = false, toolsUsed_ = false;
    // A practice save state: copies of everything that runs during a stage.
    struct SaveSlot {
        bool valid = false;
        std::uint64_t generation = 0, loadSerial = 0;
        GameState state;
        ZunRng rng{0};
        BulletManager bullets;
        EnemyManager enemies;
        ItemManager items;
        Player player;
        ScreenShake shake;
        EffectManager effects;
        Gui gui;
        StageBackground background;
        std::vector<std::uint8_t> ecl;
        Image text;  // text.anm's rendered lines
        int clearTimer = 0;
        std::string song;
    };
    std::unique_ptr<SaveSlot> save_;
    std::uint64_t loadSerial_ = 0;
    std::string toast_;  // "STATE SAVED" ... shown for a second
    int toastTimer_ = 0;
    void renderToast(SpriteSink& sink);
    bool recordable_ = true;  // a full game without mischief: its scores are saved
    Translation translation_;
    // Replays: the recording of a live game, or the replay being watched.
    std::string replayDir_;
    std::unique_ptr<Replay> recording_;
    ReplayRecorder recorder_;
    std::unique_ptr<Replay> playback_;
    ReplayCursor cursor_;
    bool demo_ = false;
    int demoTimer_ = 0;
    InputState demoPrev_;
    void tickPass(const InputState& raw);
    void nextStage();
    void endPlayback(bool toReplayList);
    void stopRecordingStage();
    ResultScreen results_;
    bool inResults_ = false;
    Ending ending_;
    bool inEnding_ = false;
    bool endingMusic_ = false;  // the result screen keeps what the ending left playing
    Title title_;
    bool inTitle_ = false;
    MusicRoom musicRoom_;
    bool inMusicRoom_ = false;
    TitleSession session_;
    GameConfig config_;
    std::string configPath_;
    TextureLibrary& textures_;
    std::string error_;
    int stageNumber_ = 1;
    int clearTimer_ = 0;
};

}  // namespace touhou::th06
