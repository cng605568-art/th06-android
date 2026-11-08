#pragma once

// The result screen (spec 6): the viewer opened from the title (score pages per difficulty
// and shot, the spell card list, the Player Data statistics and the hidden unlock code), and
// the screen after a game (name entry into the high-score list, the final statistics, the
// replay prompt) or after a practice run (the practice best). Removing it writes score.dat.
// result00.anm's 41 instances are driven by interrupts; names and statistics are rendered
// into text.anm sprites, numbers drawn with the ASCII font. Logic priority 14, draw order 13.

#include <array>
#include <cstdint>
#include <string>

#include "th07/Anm.hpp"
#include "th07/Flow.hpp"
#include "th07/ScoreFile.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

class ResultScreen {
  public:
    enum class Mode : std::uint8_t { Viewer, AfterGame, PracticeEnd };
    enum class State : std::uint8_t {
        Menu, ScorePage, SpellList, PlayerData, PlayerDataOut, Exit,
        NameEntry, Statistics, StatisticsOut, Prompt, CannotSave, Leave,
    };
    static constexpr int kInstances = 41;
    static constexpr int kTexts = 15;
    // The name-entry grid: 6 rows of 16 (93 a blank, 94 "SP", 95 "END").
    static constexpr int kKeys = 96, kKeySpace = 93, kKeySp = 94, kKeyEnd = 95;
    static const char kKeyTable[kKeys + 1];

    ResultScreen(FlowContext& ctx, Mode mode) : ctx_(ctx), mode_(mode) {}
    ~ResultScreen();

    bool create(std::string* error);  // spec 6.3
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    // The screen removed itself (it requested the title). finish() then writes score.dat and
    // releases its files.
    bool done() const { return done_; }
    void finish();

    Mode mode() const { return mode_; }
    State state() const { return state_; }
    int menuCursor() const { return menuCursor_; }
    int keyCursor() const { return keyCursor_; }
    int nameCursor() const { return nameCursor_; }
    const std::array<char, 12>& lastName() const { return lastName_; }
    const ScoreList& list(int difficulty, int shot) const { return lists_[ScoreFile::listIndex(difficulty, shot)]; }
    int rank() const { return rank_; }  // this run's rank (0-based), -1 none

  private:
    void setState(State s);
    void sendAll(int interrupt);
    void menu(const MenuInput& in);
    void scorePage(const MenuInput& in);
    void spellList(const MenuInput& in);
    void playerData(const MenuInput& in);
    void nameEntry(const MenuInput& in);
    void statistics(const MenuInput& in);
    void prompt(const MenuInput& in);
    void cheat(const MenuInput& in);
    void finishName();
    void renderText(int index, const std::string& sjis, std::uint32_t colorRef = 0xFFFFFFu);
    void renderSpellPage();
    void renderPlayerData();
    void drawScorePage(SpriteSink& sink, int difficulty, int shot);
    void drawSpellList(SpriteSink& sink);
    void drawStatistics();
    void drawGrid();
    float completion() const;

    FlowContext& ctx_;
    Mode mode_;
    State state_ = State::Menu;
    int timer_ = 0;
    bool entered_ = false;
    bool done_ = false, finished_ = false, loaded_ = false;

    Surface background_;
    std::array<AnmVm, kInstances> vm_{};
    std::array<bool, kInstances> has_{};
    std::array<AnmVm, kTexts> text_{};
    std::array<bool, kTexts> textShown_{};
    AnmVm bar_;
    AsciiText ascii_;

    std::array<ScoreList, kDifficulties * kShots> lists_;
    std::array<std::uint8_t, 0x1C> header_{};
    bool haveHeader_ = false;
    std::array<char, 12> lastName_{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0, 0, 0, 0};
    bool lastNameFound_ = false;
    std::array<int, 7> capturedCount_{};

    // Viewer.
    int menuCursor_ = 1;
    int pageDifficulty_ = 0, pageShot_ = 0;
    int spellPage_ = 0, spellColumn_ = 6, rowAnim_ = 40;
    bool spellDirty_ = true;
    std::uint32_t lastSeconds_ = 0xFFFFFFFFu;
    int cheatStep_ = 0;
    bool cheatFire_ = false;

    // After a game.
    int rank_ = -1;
    int nameCursor_ = 0, keyCursor_ = 0;
    std::array<char, 9> name_{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0};
    int statsPass_ = 0;
    bool promptNote_ = false;
};

}  // namespace touhou::th07
