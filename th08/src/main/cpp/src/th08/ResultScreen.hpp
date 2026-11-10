#pragma once

// Imperishable Night's result screen (claude/th08-specs/spec_result_ending.md §1-§3, §5):
// Player Data from the title (score tables, the spell card list per category and character,
// the play statistics), and after a game the high-score entry (the qualifying rank among the
// ten placeholders, the name grid), the final statistics, then the leave that writes
// score.dat; after a practice run the practice best. The replay prompt is not ported (no
// replays yet): the screen leaves at once. result00.anm's 72 VMs are driven by interrupts;
// 30 text VMs carry the Shift-JIS lines. Logic priority 16, draw 18.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"
#include "th08/ScoreFile.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

class ResultScreen {
  public:
    enum class Mode : std::uint8_t { Viewer, AfterGame, PracticeEnd, SpellPracticeEnd };
    enum class State : std::uint8_t {
        Menu, Leave, Difficulty, Character, Table, SpellCategory, SpellColumn, SpellList,  // the viewer (1-8)
        NameEntry, Statistics, StatisticsOut,                                             // after a game (9, 15, 16)
        DataIn, DataShown, DataOut,                                                       // play statistics (19-21)
        ReplaySave, ReplayCannot, ReplaySlots, ReplayName, ReplayOverwrite,               // the replay prompt (10-14)
    };
    static constexpr int kTextVms = 30;
    static constexpr int kVms = 72;
    static constexpr int kSlotResult = 21;
    // The name grid: 6 rows of 16 (93 a blank, 94 "SP", 95 "END").
    static constexpr int kKeys = 96, kKeySpace = 93, kKeySp = 94, kKeyEnd = 95;
    static const char kKeyTable[kKeys + 1];

    // `game`: the run as it ended; `records`: the game's records (written when leaving).
    ResultScreen(FlowContext& ctx, Mode mode, const GameState& game, ScoreData& records)
        : ctx_(ctx), mode_(mode), game_(game), records_(records) {}
    ~ResultScreen();

    bool create(std::string* error);
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    bool done() const { return done_; }

    State state() const { return state_; }
    int cursor() const { return cursor_; }
    int menuCursor() const { return menuCursor_; }
    int shownCharacter() const { return shownCharacter_; }
    int column() const { return column_; }
    int category() const { return category_; }
    // The spell list's captured-card counts per category and column (spec 1.3 step 9).
    int capturedCount(int category, int column) const { return counts_[static_cast<std::size_t>(category)][static_cast<std::size_t>(column)]; }
    const std::string& textLine(int i) const { return textLines_[static_cast<std::size_t>(i)]; }
    int rank() const { return rank_; }  // this run's rank (0-based), -1 none
    int keyCursor() const { return keyCursor_; }
    int nameCursor() const { return nameCursor_; }
    const std::array<char, 9>& name() const { return name_; }

  private:
    // The viewer's pages.
    void menu(const MenuInput& in);
    void rowsPage(const MenuInput& in);
    void table(const MenuInput& in);
    void spellList(const MenuInput& in);
    void playData(const MenuInput& in);
    void enterRows(int first, int count, int cursor);
    void highlightRows(int first, int count, int cursor);
    void chooseRow(int first, int count, int chosen);
    bool moveRows(const MenuInput& in, int count, int& cursor);
    void cheatCode(const MenuInput& in);
    void renderText(int i, const std::string& sjis);
    void renderSpellPage();
    void renderPlayData();
    void drawSpellList(SpriteSink& sink);
    void setState(State s);
    void sendAll(int interrupt);
    void nameEntry(const MenuInput& in);
    void finishName();
    void drawTable(int difficulty, int character);
    void drawGrid();
    void drawStatistics();
    // The replay prompt (spec 4).
    void replayPrompt(const MenuInput& in);
    void enterSlots();
    void drawSlots();
    // The name grid's input on `name` (spec 3.4): 0 nothing, 1 END chosen, 2 Menu pressed.
    int gridInput(const MenuInput& in, std::array<char, 9>& name, int& cursor);
    HighScoreRecord* runRecord();

    FlowContext& ctx_;
    Mode mode_;
    GameState game_;
    ScoreData& records_;
    State state_ = State::NameEntry;
    int pass_ = 0;
    bool done_ = false, loaded_ = false;

    Surface background_;
    std::array<AnmVm, kVms> vm_{};
    AsciiText ascii_;

    std::array<AnmVm, kTextVms> text_{};
    std::array<std::string, kTextVms> textLines_{};
    AnmVm bar_;
    int sub_ = 0, subPass_ = 0;
    int cursor_ = 0, menuCursor_ = 0;
    int shownCharacter_ = 0;  // +0x3C (-1: nothing yet)
    int category_ = 5, column_ = 12, shownColumn_ = 12, shownPage_ = -1;
    bool animating_ = false, leaving_ = false;
    int cheatStep_ = 0;
    std::array<std::array<int, 13>, 6> counts_{};
    int yesNo_ = 0, slot_ = 0, replayNameCursor_ = 0;
    std::array<char, 9> replayName_{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0};
    std::array<std::optional<std::array<std::uint8_t, 0xCC>>, 15> slots_{};
    std::string replayDate_;
    bool lastNameFound_ = false;
    int difficulty_ = 1, character_ = 0;
    int rank_ = -1;
    int nameCursor_ = 0, keyCursor_ = 0;
    std::array<char, 9> name_{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0};
};

}  // namespace touhou::th08
