#pragma once

// The result screen after a game (game over, giving up, or a clear), as in the 2002 game:
// result.jpg behind 38 animations from result00..03.anm, then
//   1. high-score name entry when the score makes the top 10 of its difficulty and
//      character: the score table (both shot types) and a 6 x 16 key grid with END and SP,
//      auto-repeating cursor, 8-character names;
//   2. the statistics page: score, difficulty, completion, continues, misses, bombs, spell
//      cards captured, slowdown;
//   3. a 60-frame exit to the title, writing score.dat.
// The port records no replays, so the save-replay prompt after the statistics is skipped.

#include <array>
#include <cstdint>
#include <string>

#include "game/Input.hpp"
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
    enum class State { NameEntry, Statistics, StatisticsFade, Exiting, Done };

    struct Run {
        int difficulty = 1, shotType = 0;
        int stage = 1;  // where the run ended (7 = the Extra stage)
        bool cleared = false;
        std::uint32_t score = 0;
        int continues = 0;
        std::uint32_t deaths = 0, bombs = 0, spellsCaptured = 0;
        std::int64_t completion = 0;
    };

    // Loads result.jpg and result00..03.anm (the game's art must be gone: they reuse the
    // enemy slots) and starts the first page. False if the files are missing.
    bool start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui, ScoreFile& scores, const Run& run);
    // One pass. True once the screen has finished (score.dat should be written now).
    bool update(World& world, Gui& gui, const InputState& in);
    void draw(World& world, Gui& gui, SpriteSink& sink);

    State state() const { return state_; }
    int timer() const { return t_; }
    int rank() const { return rank_; }  // of the new entry (>= 10: not in the table)
    int keyCursor() const { return key_; }
    int nameCursor() const { return cursor_; }
    std::string name() const;  // the entry's name as typed (8 bytes)
    const AnmVm& vm(int i) const { return vms_[static_cast<std::size_t>(i)]; }

    // Statistics lines as drawn (tests).
    static std::string completionText(const Run& run);

  private:
    struct Edges {
        bool up = false, down = false, left = false, right = false;  // pressed or repeating
        bool confirm = false, confirmRepeat = false, cancelRepeat = false, esc = false;
    };
    Edges readInput(const InputState& in);
    void interruptAll(std::int16_t k);
    void nameEntry(World& world, Gui& gui, const Edges& e);
    void finishNameEntry(World& world);
    void moveKey(int step, bool vertical);
    void drawTable(Gui& gui, SpriteSink& sink, World& world);
    void drawKeys(Gui& gui);
    void drawStatistics(Gui& gui);

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
};

}  // namespace th06
