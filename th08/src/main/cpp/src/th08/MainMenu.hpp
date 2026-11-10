#pragma once

// Imperishable Night's main menu (claude/th08-specs/spec_flow_menus.md §3-4): title00.png
// or select00.png behind the 142 VMs of title01.anm (VM i runs script i). Ported so far: the
// title (nine rows with their descriptions, the white fade-in and the title song), the Option
// screen, and the difficulty and character selects of Game Start, Practice Start and Extra
// Start. A row or choice whose screen is not ported yet says so.

#include <array>
#include <cstdint>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"

namespace touhou {
class SpriteSink;
}

namespace touhou::th08 {

class MainMenu {
  public:
    static constexpr int kInstances = 142;
    static constexpr int kDescriptions = 14;
    static constexpr int kTitleRows = 9;
    static constexpr int kOptionRows = 10;
    enum Row { GameStart, ExtraStart, SpellPractice, PracticeStart, Replay, PlayerData, MusicRoom, Option, Quit };
    enum OptionRow { Lives, Colour, BgmMode, BgmVolume, SeVolume, Window, Slow, Reset, KeyConfig, OptionExit };
    // The menu states (spec 3.1, MM+0x14428).
    enum class State : int {
        Title = 0, Options = 2, KeyConfigScreen = 3, Difficulty = 4, Character = 5, Replays = 7,
        PracticeDifficulty = 8, PracticeCharacter = 9, PracticeStage = 11, ExtraDifficulty = 12, ExtraCharacter = 13,
    };

    explicit MainMenu(FlowContext& ctx) : ctx_(ctx) {}

    // Loads title01.anm and the background and sets up the description VMs (spec 3.2).
    bool create(std::string* error);
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    bool removed() const { return removed_; }
    State state() const { return state_; }
    int cursor() const { return cursor_; }
    int subState() const { return sub_; }

  private:
    void setState(State s);
    void enterTitle();
    void title(const MenuInput& in);
    void options(const MenuInput& in);
    void showOptionValues();
    void leaveOptions();
    void difficulty(const MenuInput& in);
    void highlightDifficulty();
    void character(const MenuInput& in);
    void showCards(bool afterMove);
    void highlightRows(int first, int count, int selected);
    void showDescription(int row);
    void hideDescription();
    void sendAll(int interrupt);
    bool rowLocked(int row) const;
    void notPorted();
    void loadBackground(const char* path);
    void restartTitleSong();

    FlowContext& ctx_;
    std::array<AnmVm, kInstances> vm_{};
    std::array<std::int16_t, kInstances> base_{};
    std::array<bool, kInstances> has_{};
    bool instancesMade_ = false;
    std::array<AnmVm, kDescriptions> desc_{};
    int currentDesc_ = 0;
    bool descShown_ = false;
    Surface background_;
    AsciiText ascii_;

    State state_ = State::Title;
    State prevState_ = State::Title;  // MM+0x68
    int sub_ = 0;          // MM+0xC
    int stateTimer_ = 0;   // MM+0x10
    int inputTimer_ = 0;   // MM+0x1442C
    int idle_ = 0;         // MM+0x14430
    int cursor_ = 0;
    int remembered_ = -1;
    int titleCursor_ = 0;  // where the title cursor goes back to
    bool firstEntry_ = true;
    std::uint8_t savedColour_ = 0, savedWindow_ = 0;  // the Option menu's comparison copy
    int fade_ = 0;          // the white fade-in: frames left of 70
    int notice_ = 0;        // "not ported yet": frames left
    bool removed_ = false;
};

}  // namespace touhou::th08
