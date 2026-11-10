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
    std::uint32_t score = 0, guiScore = 0, hiScore = 100000;
    int continues = 0, hiScoreContinues = 0;
    float lives = 2, bombs = 3, power = 0;
    std::int32_t graze = 0, stageGraze = 0;
    std::int32_t pointItems = 0, nextExtend = 50, stagePointItems = 0;
    std::int32_t timePoints = 0, timeQuota = 0;
    int nightSteps = 0;  // 23:00 + 30 minutes per step
    std::int32_t pointValue = 100000;  // globals+0x24: the maximum point-item value
    std::int32_t rank = 10;             // GM+0x3DE2C
    float misses = 0, bombsUsed = 0;
    int livesI() const { return static_cast<int>(lives); }
    int bombsI() const { return static_cast<int>(bombs); }
    int powerI() const { return static_cast<int>(power); }
};

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

  private:
    void drawPanel(SpriteSink& sink, const GameState& g, AsciiText& ascii);

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
    std::array<int, 6> refresh_{};  // lives, bombs, power, graze, point items, time points
};

}  // namespace touhou::th08
