#pragma once

// The title and select screens (spec 3): the main menu, the Option menu and Key Config, the
// difficulty, character, shot and practice-stage selections (main game, practice, Extra and
// Phantasm), the Phantasm unlock splash, the replay menu (files, stages, playback modes) and
// the attract demo after 15 idle seconds. title01.anm's 164 instances are driven by
// interrupts; descriptions are rendered into text.anm sprites. Logic priority 3, draw order 0.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "th07/Anm.hpp"
#include "th07/Flow.hpp"
#include "th07/GameConfig.hpp"
#include "th07/Replay.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

class MainMenu {
  public:
    static constexpr int kInstances = 164;
    static constexpr int kDescriptions = 14;
    // Replay is the replay menu's entry and file list, ReplayStages its stage list and
    // ReplayMode the playback-mode list (spec 3.10, sub-states 0-3; ReplayLeave is 4).
    enum class State : std::uint8_t {
        Splash, Main, OptionWait, Option, KeyConfig, Difficulty, DifficultyLeave, Character, Shot, PracticeStage, Replay,
        ReplayLeave, Quit, ReplayStages, ReplayMode,
    };
    // One file of the replay list: its label ("No.NN" or "User "), path and summary.
    struct ReplayItem {
        std::string label, path;
        ReplayFile file;  // header and summary only
    };
    enum class Variant : std::uint8_t { Main, Practice, Extra };

    explicit MainMenu(FlowContext& ctx) : ctx_(ctx) {}
    ~MainMenu();

    // The menu's creation (spec 3.3), each time the title is entered.
    bool create(std::string* error);
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    // The menu removed itself (it requested another state). Its files are released then.
    bool done() const { return done_; }
    void release();

    State state() const { return state_; }
    Variant variant() const { return variant_; }
    int cursor() const { return cursor_; }
    // Android Back leaves the app from here (the title's cursor on Quit).
    bool atQuit() const { return state_ == State::Main && cursor_ == 7; }
    const std::vector<ReplayItem>& replays() const { return replays_; }
    int replayIndex() const { return replayIndex_; }

    // The replay list of spec 3.10.1: replay/th7_01.rpy .. th7_15.rpy that validate, then up to
    // 45 valid replay/th7_ud????.rpy (sorted by name), at most 60.
    static std::vector<ReplayItem> scanReplays(const std::string& dir, std::span<const std::uint8_t> verFile);

  private:
    void setState(State s);
    bool handle(const MenuInput& in);  // true: "run again"
    bool mainMenu(const MenuInput& in);
    bool option(const MenuInput& in);
    bool keyConfig(const MenuInput& in);
    bool difficulty(const MenuInput& in);
    bool character(const MenuInput& in);
    bool shot(const MenuInput& in);
    bool practiceStage(const MenuInput& in);
    bool replay(const MenuInput& in);
    bool replayStages(const MenuInput& in);
    bool replayMode(const MenuInput& in);
    bool tryDemo();
    void splash(const MenuInput& in);
    void drawReplayMenu();

    int verticalMove(const MenuInput& in, int rows);
    int horizontalMove(const MenuInput& in, int columns);
    void sendAll(int interrupt);
    void highlight(int instance, bool on);
    void highlightRows(int first, int count, int selected);
    void renderDescriptions(int count, const char* const* texts);
    void showDescription(int row);
    void hideDescription();
    void loadBackground(const char* path);
    bool characterLocked(int character) const;
    bool shotLocked(int shot) const;
    int difficultyCard() const;
    void setCharacterCards(bool allActive);
    void onlyCharacterCards(int character);
    void startGame();
    void makeInstances();

    FlowContext& ctx_;
    State state_ = State::Main, prevState_ = State::Main;
    Variant variant_ = Variant::Main;
    int timer_ = 0, idle_ = 0;
    bool stateChanged_ = false, entered_ = false;
    int cursor_ = 0;
    bool firstEntry_ = true, instancesMade_ = false;
    bool autoPractice_ = false;
    bool done_ = false;
    bool loaded_ = false;
    int fade_ = -1;  // the white fade-in (70 passes), -1 off
    int splashT_ = 0;
    int optionCursor_ = 0, keyCursor_ = 0;
    GameConfig configCopy_;
    std::array<std::int16_t, 9> keyWork_{}, keyBoot_{};
    bool keyBootSaved_ = false;

    Surface background_, splash_;
    std::array<AnmVm, kInstances> vm_{};
    std::array<bool, kInstances> has_{}, active_{};
    std::array<std::int16_t, kInstances> base_{};
    std::array<AnmVm, kDescriptions> desc_{};
    int currentDesc_ = 0;
    bool descShown_ = false;
    AsciiText text_;

    // The replay menu.
    std::vector<ReplayItem> replays_;
    int replayIndex_ = 0;  // the file chosen in the list
    std::shared_ptr<ReplayFile> chosen_;  // its whole contents
    bool afterReplay_ = false;  // a replay just ended: straight to the replay menu
};

}  // namespace touhou::th07
