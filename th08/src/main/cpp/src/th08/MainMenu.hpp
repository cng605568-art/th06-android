#pragma once

// Imperishable Night's main menu (claude/th08-specs/spec_flow_menus.md §3-4): title00.png
// or select00.png behind the 142 VMs of title01.anm (VM i runs script i). Ported so far: the
// title (nine rows with their descriptions, the white fade-in and the title song), the Option
// screen, the difficulty and character selects of Game Start, Practice Start and Extra Start,
// Spell Card Practice and the Replay menu (claude/th08-specs/spec_replay_menu.md §1). A row or
// choice whose screen is not ported yet says so.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"
#include "th08/Replay.hpp"

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
        SpellCharacter = 15, SpellStage = 16, SpellCards = 17,
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
    int page() const { return page_; }
    const std::string& listLine(int i) const { return listLines_[static_cast<std::size_t>(i)]; }
    const std::string& panelLine(int i) const { return panelLines_[static_cast<std::size_t>(i)]; }
    // A card's difficulty for Spell Card Practice (0-4, 5 = Last Word; 0x4144D0).
    static int cardDifficulty(const ExeTables* tables, int card);
    // A Last Word's stage index (FLOW 4.6.3).
    static int lastWordStage(int card);

    // The Replay menu (state 7): the listed files (numbered slots, then user files), the chosen
    // file and stage slot, and the strings its draw queues this frame.
    struct ReplayEntry {
        std::string path, label;  // label "No.NN" (the slot number) or "User "
        ReplaySummary summary;
        std::array<bool, kReplayStages> present{};
        std::array<std::uint32_t, kReplayStages> carriedScore{};
    };
    struct TextLine {
        float x = 0, y = 0;
        std::string text;
        std::uint32_t color = 0xFFFFFFFFu;
        bool selected = false;
    };
    const std::vector<ReplayEntry>& replayEntries() const { return replays_; }
    int chosenReplay() const { return chosenFile_; }
    int chosenStage() const { return chosenStage_; }
    std::vector<TextLine> replayMenuText() const;

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
    void showCards(bool afterMove, int character = -1, bool withName = true);
    // Spell Card Practice (spec_flow_menus 4.6, claude/th08-specs/spec_spell_practice.md §2).
    void spellCharacter(const MenuInput& in);
    void spellStage(const MenuInput& in);
    void spellCards(const MenuInput& in);
    bool spellCharacterAllowed(int character) const;
    int spellStep(int character, int step) const;
    const std::vector<std::int32_t>* spellList() const;
    void renderText(AnmVm& vm, std::string& line, const std::string& sjis);
    std::string cardName(int card, const std::string& sjis) const;
    void renderSpellRows();
    void spellCursorMoved();
    void detailPanel(bool entry);
    void drawSpellStage(SpriteSink& sink);
    void drawSpellCards(SpriteSink& sink);
    void drawPie(SpriteSink& sink, float cx, float cy, std::uint32_t argb, float fraction, float size);
    void drawPercent(SpriteSink& sink, float x, float y, float value, std::uint32_t argb);
    // The Replay menu (spec_replay_menu §1.3-1.8).
    void enterReplays(bool sound);
    bool startDemo();
    void replays(const MenuInput& in);
    void scanReplays();
    int verticalMove(const MenuInput& in, int n);
    void highlightModes();
    void drawReplays(SpriteSink& sink);
    void practiceStage(const MenuInput& in);
    bool stageSelectable(int stage) const;
    void drawStageList();
    void highlightRows(int first, int count, int selected);
    void showDescription(int row);
    void hideDescription();
    void sendAll(int interrupt);
    bool rowLocked(int row) const;
    void notPorted();
    void showNotice(std::string text, float y);  // a port message at the bottom, about 5 s
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
    int autoReturn_ = 0;  // MM+0x6C: back from a practice (1) or spell practice (2) game
    // Spell Card Practice: 16 list-text VMs (resulttext.anm bands 2-17), the rate panel's
    // decoration (title01 sprite 144), the 7 detail lines (text.anm sprites 21-27), the eased
    // capture rates, the page and the detail counter.
    std::array<AnmVm, 16> listText_{};
    std::array<std::string, 16> listLines_{};
    AnmVm rateDeco_;
    std::array<AnmVm, 7> panel_{};
    std::array<std::string, 7> panelLines_{};
    std::array<float, 4> rates_{};
    int page_ = 0, detail_ = 0;
    bool resultTextLoaded_ = false;
    std::vector<ReplayEntry> replays_;
    std::optional<ReplayFile> chosen_;  // MM+0xC280: the chosen replay, read again on Select
    int chosenFile_ = 0, chosenStage_ = 0;  // MM+0xC28C / MM+0xC290
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
    int notice_ = 0;        // a port message ("not ported yet", the stage-list hint): frames left
    std::string noticeText_;
    float noticeY_ = 448.0f;
    bool removed_ = false;
};

}  // namespace touhou::th08
