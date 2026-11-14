#pragma once

// Imperishable Night's screen effects (claude/th08-specs/spec_bombs_familiars.md §1.4): jobs
// registered by bombs, deaths and ECL extension routines, run at calc priority 3 (before the
// player, so a job registered in frame N first runs in frame N+1). The shakes matter to the
// simulation: every live shake draws two u32 from the gameplay RNG per frame. The flash is
// visual.

#include <cstdint>
#include <vector>

#include "core/ZunMath.hpp"
#include "th08/Anm.hpp"

namespace touhou {
class SpriteSink;
struct SpriteRegion;
}  // namespace touhou

namespace touhou::th08 {

struct World;

class ScreenEffects {
  public:
    void reset();
    // Type 1: a shake for `frames` frames whose offset goes from `from` to `to`.
    void shake(std::int32_t frames, float from, float to);
    // Type 7: a shake of `amplitude`, ramping up over `up` frames, holding for `hold`, ramping
    // down over `down`.
    void rampedShake(float amplitude, std::int32_t up, std::int32_t hold, std::int32_t down);
    // Type 3: a playfield flash fading over `frames` frames, `repeats` times.
    void flash(std::int32_t frames, std::int32_t repeats, std::uint32_t color);
    // Type 4: a full-screen fade to `color` over `frames` frames (MSG op 14), held at the end.
    void fadeTo(std::int32_t frames, std::uint32_t color);
    // Type 2: the same over the playfield (the attract demo's ending fade).
    void playfieldFadeTo(std::int32_t frames, std::uint32_t color);

    void update(const World& w);  // calc priority 3
    void draw(SpriteSink& sink, const SpriteRegion* white) const;  // draw priority 21
    float offsetX() const { return offset_.x; }
    float offsetY() const { return offset_.y; }
    int jobs() const { return static_cast<int>(jobs_.size()); }

  private:
    struct Job {
        int type = 1;
        Timer timer;
        std::int32_t frames = 0, up = 0, hold = 0, down = 0, repeats = 0;
        float from = 0, to = 0, amplitude = 0;
        std::uint32_t color = 0;
    };
    std::vector<Job> jobs_;
    Vec2f offset_;
};

}  // namespace touhou::th08
