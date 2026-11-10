#pragma once

// Imperishable Night's endings and staff roll (claude/th08-specs/spec_result_ending.md §7-§8;
// the interpreter as TH07's, with TH08's deltas): end0Ta.end (bad), end0Tb.end (Final A, then
// staff00b.end), end0Tc.end (Final B, then staff00.end), T = the team. The ending writes the
// clear records first (Final A / Final B bits, clear counts, the ending themes' Music Room
// flags); when its script ends, the result screen follows. Logic priority 5, draw 4.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

struct ScoreData;

class Ending {
  public:
    static constexpr int kVms = 16;  // 15 executed and drawn, a 16th record never used
    static constexpr int kSlotStaff = 24;

    explicit Ending(FlowContext& ctx) : ctx_(ctx) {}
    ~Ending();

    struct Run {
        int character = 0;   // 0-11 (solos use their team's file)
        int difficulty = 1;
        int stage = 6;       // the stage index the game ended on (6 Final A, 7 Final B)
        bool finished = false;  // a good ending (else the bad one)
        bool continued = false;
    };
    // The ending's start (0x429980): the records written, staff01.anm loaded, the script read.
    // False: no script (the result screen follows at once).
    bool create(const Run& run, ScoreData* records, std::string* error);
    // Runs a script given as bytes (tests; `path` names it in logs).
    bool startScript(std::vector<std::uint8_t> bytes, const std::string& path);
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    bool done() const { return done_; }
    void release();

    bool seen() const { return seen_; }
    const std::string& script() const { return path_; }
    std::uint32_t steps() const { return steps_; }
    int lineSlot() const { return lineSlot_; }
    float backgroundTop() const { return top_; }
    const std::string& lastLine() const { return lastLine_; }

  private:
    bool step(const MenuInput& in);
    bool command(bool& endStep);
    std::int32_t readNumber();
    std::string readString();
    void skipLine();
    bool loadScript(const std::string& path);
    void finish();

    FlowContext& ctx_;
    std::vector<std::uint8_t> data_;
    std::size_t pos_ = 0;
    std::string path_;
    bool done_ = false, staffLoaded_ = false;
    bool seen_ = false, unlockGranted_ = false;
    std::array<AnmVm, kVms> vm_{};
    Surface background_;
    float top_ = 0.0f, scroll_ = 0.0f;
    int normalDelay_ = 8, heldDelay_ = 0;
    std::int32_t wait_ = 0, waitMin_ = 0, clear_ = 0, clearMin_ = 0;
    std::uint32_t color_ = 0;  // GDI colour (black until the script sets one)
    int lineSlot_ = 0;
    std::uint32_t steps_ = 0;
    int fadeType_ = -1, fadeT_ = 0, fadeN_ = 0;
    std::uint32_t overlay_ = 0;  // 0xAARRGGBB
    std::string lastLine_;
};

}  // namespace touhou::th08
