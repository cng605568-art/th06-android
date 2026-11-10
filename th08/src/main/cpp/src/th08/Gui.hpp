#pragma once

// Imperishable Night's in-stage HUD (claude/th08-specs/spec_stage_gui.md §3): the side panel
// of front.anm (frame, labels, lives and bombs, score rows, power bar), the difficulty label,
// the stage-title texts of stgNtxt.anm and the night clock of times.anm. Ported so far: the
// stage start, the per-frame VM ticks and clock fade, and the panel; the boss HUD, the
// timed messages, the tally and dialogue come with the systems that drive them.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th08 {

// What a game keeps across its stages (GameManager / globals, spec_flow_menus §6.2).
struct GameState {
    int stage = 0;       // stage index 0-8
    int character = 0;   // 0-3 teams, 4-11 solo characters
    int difficulty = 1;  // 0 Easy .. 3 Lunatic, 4 Extra
    bool practice = false, spellPractice = false;
    // Scores in the internal unit (points / 10); the HUD prints them followed by the continue digit.
    std::uint32_t score = 0, guiScore = 0, hiScore = 100000, rollSpeed = 0;
    int continues = 0, hiScoreContinues = 0;
    float lives = 2, bombs = 3, power = 0;
    std::int32_t graze = 0, stageGraze = 0;
    std::int32_t pointItems = 0, nextExtend = 50, stagePointItems = 0;
    std::int32_t timePoints = 0, timeQuota = 0;
    int nightSteps = 0;  // 23:00 + 30 minutes per step
    std::int32_t pointValue = 100000;  // globals+0x24: the maximum point-item value
    std::int32_t timeTotal = 0;         // globals+0x44: this stage's running total of time points
    std::int32_t extends = 0;           // globals+0x34: extends granted
    std::int32_t rank = 10, minRank = 8, maxRank = 16, subrank = 0;  // GM+0x3DE2C (spec 14)
    float misses = 0, bombsUsed = 0;
    std::int32_t lastSpellsUsed = 0, bombsUsedGame = 0;

    // The human/youkai gauge (spec 11): negative human, positive youkai; the limits and the
    // 80 % / 20 % thresholds are set per character at every stage set-up.
    std::int16_t gauge = 0, gaugeShown = 0;
    std::int16_t gaugeMin = -10000, gaugeMax = 10000;
    std::int16_t human80 = -8000, youkai80 = 8000, human20 = -2000, youkai20 = 2000;
    // Frame counters for the stage result's "over 80 %" lines (spec 1.2 step 11).
    std::int32_t stageFrames = 0, gameFrames = 0, stageHuman80 = 0, gameHuman80 = 0, stageYoukai80 = 0, gameYoukai80 = 0;

    bool h80() const { return gauge <= human80; }
    bool y80() const { return gauge >= youkai80; }
    bool h20() const { return gauge <= human20; }
    bool y20() const { return gauge >= youkai20; }
    // AddHumanYoukaiGauge (0x43C0BB): ignored during a bomb unless forced; clamped; the
    // display copy follows.
    // The score roll (spec_flow_menus 6.6 step 9, as TH07): the shown score climbs towards the
    // score by (score - shown) >> 5 a frame, clamped to 1 .. 578910, never slowing down; the
    // hi-score follows the shown score.
    void rollScore() {
        if (score > 999999999u) score = 999999999u;
        if (guiScore == score) return;
        if (score < guiScore) score = guiScore;
        std::uint32_t step = (score - guiScore) >> 5;
        step = step < 1u ? 1u : step > 578910u ? 578910u : step;
        if (rollSpeed < step) rollSpeed = step;
        if (guiScore + rollSpeed > score) rollSpeed = score - guiScore;
        guiScore += rollSpeed;
        if (guiScore >= score) {
            rollSpeed = 0;
            guiScore = score;
        }
        if (guiScore > hiScore) {
            hiScore = guiScore;
            hiScoreContinues = continues;
        }
    }
    void addGauge(int delta, bool force, bool bombActive) {
        if (bombActive && !force) return;
        gauge = static_cast<std::int16_t>(std::clamp(gauge + delta, static_cast<int>(gaugeMin), static_cast<int>(gaugeMax)));
        gaugeShown = gauge;
    }
    void increaseSubrank(std::int32_t amount) {
        subrank += amount;
        while (subrank >= 100) {
            ++rank;
            subrank -= 100;
        }
        if (rank > maxRank) rank = maxRank;
    }
    void decreaseSubrank(std::int32_t amount) {
        subrank -= amount;
        while (subrank < 0) {
            --rank;
            subrank += 100;
        }
        if (rank < minRank) rank = minRank;
    }
    int livesI() const { return static_cast<int>(lives); }
    int bombsI() const { return static_cast<int>(bombs); }
    int powerI() const { return static_cast<int>(power); }
};

struct World;

class Gui {
  public:
    static constexpr int kSlotFront = 10, kSlotLoading = 12, kSlotStageText = 13, kSlotTimes = 14;
    static constexpr int kFrontVms = 16;

    // The Gui's added callback (spec 3.2). `firstStage`: the first stage of a game (the files
    // are loaded and the front VMs started).
    bool stageStart(const FlowContext& ctx, const GameState& g, bool firstStage, std::string* error);
    void release(AnmLibrary& anm);
    // The per-frame update (spec 3.3); `playerPos` in field coordinates.
    void update(ZunRng& rng, const GameState& g, const Vec3f& playerPos, bool frozen = false);
    // Gui draw (priority 17): the stage texts and clocks (3rd), then the panel (4th); ASCII
    // strings are queued for the AsciiManager's pass (priority 20).
    void draw(SpriteSink& sink, const GameState& g, AsciiText& ascii);
    // Full redraw requests (16 at stage start, 60 after loading, 8 while Skip is held, ...).
    void requestRedraw(int frames) { fullRedraw_ = std::max(fullRedraw_, frames); }
    int frame() const { return frame_; }
    const AnmVm& frontVm(int i) const { return front_[static_cast<std::size_t>(i)]; }
    const AnmVm& stageClock() const { return stageClock_; }

    // ---- the night clock (ECL ops 179-181) and the music caption (MSG op 7) ----
    void showClock(ZunRng& rng, int steps);  // times.anm script 2, sprite = steps
    void hideClock();
    void clockStep(int steps);               // interrupt 1, or 2 at dawn (12)
    void musicCaption(ZunRng& rng, int track);

    // ---- results (spec_msg_stage_end §10-§11) ----
    // MSG op 9: the snapshots, the night advance, the stage-clear picture and caption, the
    // finals' extend lock, the Ghost team's bomb.
    void stageResults(const World& w, GameState& g);
    // The HUD update's parts after op 9, in order: the bonus (the next frame), the stage end of
    // stages 1-5 (returned true once the clock has reached its target and the stage end is
    // idle), the clock count-up (x4 while Shoot or Skip is held, game input).
    bool resultsStep(GameState& g, std::uint16_t gameInput, int configuredLives, bool stageEndIdle);
    int resultsState() const { return resultsState_; }
    std::int32_t resultsTotal() const { return total_; }
    std::int32_t clockDisplayed() const { return displayed_; }
    std::int32_t clockTarget() const { return target_; }
    // The stage-clear picture has stopped: the stage layers are not drawn (spec 3.11).
    bool stagePictureFinished() const;

  private:
    void drawPanel(SpriteSink& sink, const GameState& g, AsciiText& ascii);
    void drawResults(const GameState& g, AsciiText& ascii);

    AnmLibrary* anm_ = nullptr;
    const UiArt* ui_ = nullptr;
    bool loaded_ = false;
    bool spellPractice_ = false;
    std::array<AnmVm, kFrontVms> front_{};
    std::array<AnmVm, 4> stageText_{};
    std::array<bool, 4> hasStageText_{};
    AnmVm titleClock_, stageClock_, nightCaption_, lastSpellMarker_, difficultyLabel_;
    int frame_ = 0;
    int fullRedraw_ = 0;
    bool clockShown_ = false;
    // Results (GI +0x22D84 ...): state 0 none, 1 just shown (the bonus is due), 2 counted.
    int resultsState_ = 0, stage_ = 0, configuredLives_ = 2;
    bool practice_ = false;
    std::int32_t base_ = 0, points_ = 0, graze_ = 0, time_ = 0, advance_ = 0, before_ = 0, target_ = 0, displayed_ = 0,
                 delay_ = 0, total_ = 0;
    AnmVm picture_;
    bool hasPicture_ = false;
    std::array<int, 6> refresh_{};  // lives, bombs, power, graze, point items, time points
};

}  // namespace touhou::th08
