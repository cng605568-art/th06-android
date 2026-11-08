#pragma once

// The ending and staff roll (spec stage/GUI 5): an interpreter for the .end scripts
// (data/endNN.end, endN0b.end, chained to the staff roll with @F) that shows pictures,
// staff01.anm sprites and text lines rendered into text.anm sprites, with waits, fades,
// background scrolling and music. When it ends, the result screen follows. Logic priority 4,
// draw order 1.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "th07/Anm.hpp"
#include "th07/Flow.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

class Ending {
  public:
    static constexpr int kVms = 16;  // 15 executed and drawn, a 16th record never used

    explicit Ending(FlowContext& ctx) : ctx_(ctx) {}
    ~Ending();

    // The ending of `shot` on `difficulty`: the clear records (seen before?) and the script
    // (the bad ending of the character after a continue). False: nothing to show (the
    // result screen follows at once).
    bool create(int shot, int difficulty, bool continued, std::string* error);
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
    std::uint32_t colorRef() const { return color_; }
    // The last text line rendered (tests).
    const std::string& lastLine() const { return lastLine_; }

  private:
    // One step (spec 5.4); returns false once the ending is over.
    bool step(const MenuInput& in);
    bool command(const MenuInput& in, bool& endStep);
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
    bool seen_ = false;
    std::array<AnmVm, kVms> vm_{};
    Surface background_;
    float top_ = 0.0f, scroll_ = 0.0f;
    int normalDelay_ = 8, heldDelay_ = 0;
    std::int32_t wait_ = 0, waitMin_ = 0, clear_ = 0, clearMin_ = 0;
    std::uint32_t color_ = 0xFFFFFFu;  // GDI colour
    int lineSlot_ = 0;
    std::uint32_t steps_ = 0;
    int fadeType_ = -1, fadeT_ = 0, fadeN_ = 0;
    std::uint32_t overlay_ = 0;  // 0xAARRGGBB
    std::string lastLine_;
};

}  // namespace touhou::th07
