#pragma once

// The result screen after a game (game over, giving up, or a clear), as in the 2002 game:
// result.jpg behind 38 animations from result00..03.anm, then
//   1. high-score name entry when the score makes the top 10 of its difficulty and
//      character: the score table (both shot types) and a 6 x 16 key grid with END and SP,
//      auto-repeating cursor, 8-character names;
//   2. the statistics page: score, difficulty, completion, continues, misses, bombs, spell
//      cards captured, slowdown;
//   3. the save-replay prompt: "save replay?", the 15 slots (replay/th6_NN.rpy), overwrite?,
//      the replay's name on the same key grid (or "cannot save" after a continue);
//   4. a 60-frame exit to the title, writing score.dat.
// From the title's "Result" item the same screen is a score viewer: a menu (Easy ... Extra,
// Spell Card, Exit), the score tables per difficulty for Reimu or Marisa (Left/Right), and
// the spell-card history in 7 pages of 10 (Left/Right); Exit writes score.dat.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "core/ZunMath.hpp"
#include "game/Input.hpp"
#include "game/Replay.hpp"
#include "game/ScoreFile.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {

class Gui;
class TextureLibrary;
class Vfs;
struct World;

class ResultScreen {
  public:
    static constexpr int kVms = 38, kTexts = 16;
    enum class State {
        NameEntry, Statistics, StatisticsFade, Exiting, Done, MenuIntro, Menu, ScorePage, SpellPage,
        ReplayQuestion, ReplayCannot, ReplaySlots, ReplayName, ReplayOverwrite,
    };

    struct Run {
        int difficulty = 1, shotType = 0;
        int stage = 1;  // where the run ended (7 = the Extra stage)
        bool cleared = false;
        std::uint32_t score = 0;
        int continues = 0;
        std::uint32_t deaths = 0, bombs = 0, spellsCaptured = 0;
        std::int64_t completion = 0;
        bool recordable = true;  // false (mischief): no name entry, nothing saved
    };

    // Loads result.jpg and result00..03.anm (the game's art must be gone: they reuse the
    // enemy slots) and starts the first page. False if the files are missing.
    bool start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui, ScoreFile& scores, const Run& run);
    // The game's recording for the save-replay prompt (null: no prompt), the folder of
    // th6_NN.rpy, and the game RNG (the file's key comes from it). Set before start().
    void setReplay(const Replay* recording, std::string dir, ZunRng* rng) {
        replay_ = recording;
        replayDir_ = std::move(dir);
        rng_ = rng;
    }
    // "MM/DD/YY" written into a saved replay (default: today).
    void setDate(std::string date) { date_ = std::move(date); }
    // The score viewer (title mode). False if the files are missing.
    bool startTitleMode(World& world, const Vfs& vfs, TextureLibrary& textures, ScoreFile& scores);
    bool titleMode() const { return titleMode_; }
    // One pass. True once the screen has finished (score.dat should be written now).
    bool update(World& world, Gui& gui, const InputState& in);
    void draw(World& world, Gui& gui, SpriteSink& sink);

    State state() const { return state_; }
    int timer() const { return t_; }
    int rank() const { return rank_; }  // of the new entry (>= 10: not in the table)
    int keyCursor() const { return key_; }
    int nameCursor() const { return cursor_; }
    int menuCursor() const { return menuCursor_; }  // also the character / spell page on a page
    int pageItem() const { return pageItem_; }
    int shownCharacter() const { return renderedCharacter_; }
    int shownSpellPage() const { return renderedPage_; }
    std::string name() const;  // the entry's name as typed (8 bytes)
    int replayChoice() const { return choice_; }  // 0 Yes, 1 No
    int replaySlot() const { return slot_; }
    const std::string& replayName() const { return replayName_; }
    // The last replay file written (empty: none).
    const std::string& savedReplay() const { return savedPath_; }
    const AnmVm& vm(int i) const { return vms_[static_cast<std::size_t>(i)]; }

    // Statistics lines as drawn (tests).
    static std::string completionText(const Run& run);

  private:
    struct Edges {
        bool up = false, down = false, left = false, right = false;  // pressed or repeating
        bool confirm = false, confirmRepeat = false, cancelRepeat = false, esc = false;
        bool cancel = false;  // Bomb or Esc, pressed
    };
    Edges readInput(const InputState& in);
    bool loadFiles(World& world, const Vfs& vfs, TextureLibrary& textures);
    bool titleUpdate(World& world, Gui& gui, const Edges& e);
    void menuPass(World& world, const Edges& e);
    void renderShotNames(World& world, Gui& gui, int character);
    void drawSpellList(Gui& gui, SpriteSink& sink, World& world);
    void interruptAll(std::int16_t k);
    void nameEntry(World& world, Gui& gui, const Edges& e);
    void finishNameEntry(World& world);
    void moveKey(int step, bool vertical);
    void drawTable(Gui& gui, SpriteSink& sink, World& world);
    void drawKeys(Gui& gui);
    void drawStatistics(Gui& gui);
    // The save-replay prompt.
    struct SlotInfo {
        std::string name, date;
        int shotType = 0;
        std::uint32_t score = 0;
    };
    void replayPrompt(World& world, const Edges& e);
    void enterSlots(World& world);
    void replayNameEntry(World& world, const Edges& e);
    void colourChoices();
    bool saveReplay();
    std::string today() const;
    void drawReplaySlots(Gui& gui, SpriteSink& sink, World& world);

    ScoreFile* scores_ = nullptr;
    Run run_;
    State state_ = State::Done;
    int t_ = 0;
    std::array<AnmVm, kVms> vms_{};
    std::array<AnmVm, kTexts> texts_{};
    int rank_ = 10;
    int key_ = 0, cursor_ = 0;
    int shownDifficulty_ = 0, shownCharacter_ = 0;
    std::uint16_t background_ = 0;
    bool haveBackground_ = false;
    float bgWidth_ = 640.0f, bgHeight_ = 480.0f;
    // Input: previous buttons and the auto-repeat counter (whole input unchanged).
    std::uint32_t prevButtons_ = 0;
    int repeatCounter_ = 0;
    // The score viewer.
    bool titleMode_ = false;
    int menuCursor_ = 1;
    int pageItem_ = 0;             // 0-4 the difficulty shown, 5 the spell cards
    int rememberedCharacter_ = 0;  // shared by the five score pages
    int rememberedPage_ = 0;
    int renderedCharacter_ = 0, renderedPage_ = 0;  // -1 until a page has rendered its names
    bool spellsShown_ = false;     // the last page opened was the spell cards
    // The save-replay prompt.
    const Replay* replay_ = nullptr;
    std::string replayDir_;
    ZunRng* rng_ = nullptr;
    std::string date_;
    int choice_ = 0, slot_ = 0;
    std::string replayName_ = std::string(8, ' ');
    std::array<std::optional<SlotInfo>, kReplaySlots> slots_{};
    std::string savedPath_;
};

}  // namespace th06
