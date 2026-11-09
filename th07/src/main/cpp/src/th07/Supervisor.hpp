#pragma once

// Perfect Cherry Blossom's supervisor (spec 1.4): the state machine that runs the title and
// select screens, a game, the result screen, the ending and the Music Room. Any screen may
// request a state; at the start of each pass the supervisor samples the input, performs the
// transition of the (active, requested) pair, zeroes the input for the rest of that pass,
// and then runs whatever is active. It also owns what lives for the whole session: the
// configuration (th07.cfg), the play statistics and records kept in memory (score.dat), and
// the play-time clocks.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "th07/Flow.hpp"
#include "th07/GameConfig.hpp"
#include "th07/Gameplay.hpp"
#include "th07/Replay.hpp"
#include "th07/ScoreFile.hpp"
#include "th07/Version.hpp"

namespace touhou {
class SpriteSink;
class TextRenderer;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th07 {

class BgmPlayer;
class Ending;
class English;
class MainMenu;
class MusicRoom;
class ResultScreen;
class SoundPlayer;
struct ExeTables;

class Supervisor {
  public:
    struct Boot {
        const Vfs* vfs = nullptr;
        TextureLibrary* textures = nullptr;
        TextRenderer* text = nullptr;
        const UiArt* ui = nullptr;
        SoundPlayer* sound = nullptr;
        BgmPlayer* music = nullptr;
        const ExeTables* tables = nullptr;
        AnmLibrary* anm = nullptr;
        ZunRng* rng = nullptr;
        const English* english = nullptr;  // the English text mode (null: the original text)
        // The app's storage, ending in '/' (score.dat, th07.cfg); empty: nothing is saved.
        std::string storageDir;
        std::vector<std::uint8_t> verFile;  // th07_0100b.ver (the VRSM check and stamp)
        ExeIdentity exe;                    // what VRSM records carry
        bool thbgm = false;                 // thbgm.dat present (th07.cfg's default music mode)
        bool canPlay = true;                // th07.exe tables and a 1.00b archive: games can start
        // Wall clock in milliseconds (play-time statistics); default: the steady clock.
        std::function<std::uint64_t()> clockMs;
    };

    Supervisor();
    ~Supervisor();

    // The boot sequence (spec 1.4.6): text.anm, ascii.anm and capture.anm, th07.cfg, the play
    // statistics from score.dat. The first pass then opens the title.
    bool start(const Boot& boot);
    // A game straight from the launcher or the command line (no title first); the flow goes
    // on normally afterwards (results, then the title).
    bool startGame(const Gameplay::Setup& setup);
    // Developer aid: the ending of `shot`, as after a clear (`continued`: the bad ending)
    // with `score` (internal units) for the result screen that follows.
    void startEnding(int shot, int difficulty, bool continued, std::uint32_t score = 0);

    // Developer aid: every clear record at 99 (the result screen's hidden code), saved; the
    // next title shows the Phantasm splash.
    void unlockAll();
    // A replay's playback from stage index `stageIndex` (0-6; -1 its first stage) in
    // `mode` (0-2), or as attract demo `demoIndex` (mode 0), straight from the command line;
    // afterwards the flow goes on as after a replay chosen in the menu.
    bool startReplay(std::shared_ptr<const ReplayFile> file, int stageIndex, int mode, bool demo = false, int demoIndex = 0);

    // One logic pass with the raw 16-bit button word (a replay's fast-forward runs several
    // passes in it, spec 7.8.6).
    void pass(std::uint16_t raw);
    void draw(SpriteSink& sink);
    // Program exit (spec 1.4.5): score.dat rewritten with the current statistics, th07.cfg
    // written. Safe to call twice.
    void shutdown();

    void request(FlowState s) { requested_ = s; }
    FlowState active() const { return active_; }
    FlowState requested() const { return requested_; }
    FlowState previous() const { return previous_; }
    bool quit() const { return quit_; }

    // Host queries.
    bool inGame() const { return active_ == FlowState::Game && game_ && game_->running(); }
    bool paused() const { return inGame() && game_->paused(); }
    void pauseNow();
    // Buttons added to the game's input while it is not paused (the launcher's "Shot is held
    // during play", the desktop's --hold-focus).
    void setGameExtras(std::uint16_t buttons) { gameExtras_ = buttons; }
    // Android Back: the pause menu in a game, Return in menus; false to leave the app (the
    // title's cursor already on Quit).
    bool back();

    Gameplay* game() { return game_.get(); }
    MainMenu* mainMenu() { return menu_.get(); }
    ResultScreen* resultScreen() { return result_.get(); }
    Ending* ending() { return ending_.get(); }
    MusicRoom* musicRoom() { return musicRoom_.get(); }
    FlowContext& context() { return ctx_; }
    ScoreData& scores() { return scores_; }
    GameConfig& config() { return config_; }
    std::uint32_t passes() const { return passes_; }

    // score.dat in the storage directory (or the game folder's, until the port writes one).
    std::optional<ScoreImage> readScoreFile();
    std::uint32_t writes() const { return writes_; }
    // The parse-only result screen (spec 5.1 use 6): every record re-read over memory and
    // the file rewritten at once.
    void rewriteScoreFile();

    // The recording of the live game (kept until the result screen saves it or it is dropped).
    const ReplayRecorder& recorder() const { return recorder_; }
    ReplayRecorder& recorder() { return recorder_; }
    // Spec 7.6: saves the recording into `path` under `name` (false: the write was skipped,
    // spec 7.6.2, or failed); the recording is freed in every case.
    bool saveReplay(const std::string& path, const std::string& name);
    // Logic passes run in the last pass() call (more than one while fast-forwarding).
    int passesLastCall() const { return passesLastCall_; }
    // Desync diagnostics for every game played back (see Gameplay::onPlaybackFrame).
    void setPlaybackObserver(std::function<void(int, std::int32_t, const ReplayEntry&, std::uint32_t)> f) {
        playbackObserver_ = std::move(f);
    }

  private:
    bool passOnce(std::uint16_t raw, bool first);
    void dropReplay();
    void transition(FlowState from);
    void openMainMenu();
    bool openGame(bool fresh, bool restart);
    void cutGame();
    void openResult(int mode);
    void openEnding();
    void closeScreens();
    void gameHooks(Gameplay& g);
    void writeScores(const std::array<ScoreList, kDifficulties * kShots>& lists, const std::array<char, 12>& lastName,
                     const std::array<std::uint8_t, 0x1C>* header);
    void refreshRunningTime();
    void playTimeAdd();
    void playTimeRebase();
    std::uint64_t now() const;
    void saveConfig();

    Boot boot_;
    FlowContext ctx_;
    GameConfig config_;
    ScoreData scores_;
    MenuInput input_;
    std::uint16_t injected_ = 0, gameExtras_ = 0;

    FlowState active_ = FlowState::Init, requested_ = FlowState::Init, previous_ = FlowState::Init;
    std::uint32_t passes_ = 0;
    bool started_ = false, quit_ = false, shutDown_ = false;
    bool statsReady_ = false;

    std::unique_ptr<Gameplay> game_;
    Gameplay::Setup setup_;  // the running game's set-up (restarts)
    std::unique_ptr<MainMenu> menu_;
    std::unique_ptr<ResultScreen> result_;
    std::unique_ptr<Ending> ending_;
    std::unique_ptr<MusicRoom> musicRoom_;

    std::array<std::uint8_t, 0x1C> lastHeader_{};
    bool haveHeader_ = false;
    std::uint32_t writes_ = 0;
    ReplayRecorder recorder_;
    int passesLastCall_ = 0;
    bool zeroNextPass_ = false;
    std::function<void(int, std::int32_t, const ReplayEntry&, std::uint32_t)> playbackObserver_;
    std::uint64_t runningBase_ = 0, playBase_ = 0;
};

}  // namespace touhou::th07
