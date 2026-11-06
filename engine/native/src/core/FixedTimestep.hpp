#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ratio>
#include <type_traits>

namespace touhou {

// Fixed-timestep clock for the simulation.
//
// One game tick is exactly 1/60 s (std::ratio<1, 60>, not rounded to nanoseconds). Real
// time from std::chrono::steady_clock is accumulated in the common type of the clock's
// nanoseconds and the tick (units of 1/3,000,000,000 s), so "has a whole tick elapsed?"
// is exact rational arithmetic and never drifts.
//
// The simulation runs ahead: whenever the accumulator is positive another tick is run, so
// afterwards it lies in (-1 tick, 0]. The latest tick therefore describes a moment at or
// just after "now", the tick before it a moment before "now", and the renderer draws
//     prev + (current - prev) * alpha,   alpha = 1 + accumulator / tick   in (0, 1]
// which is the state at exactly the present instant. On a 60 Hz display alpha is 1 every
// frame (no added latency); at 120 Hz frames alternate 0.5 / 1.0; at 90 Hz 0.67 / 0.33 /
// 1.0. Simulation results never depend on the display rate: every tick is the same
// 1/60 s step of the original game logic.
//
// Frame times within 0.5 ms of a whole number of display refreshes are snapped to it
// (vsync jitter), so a 60 Hz display yields exactly one tick per frame.
class FixedTimestep {
  public:
    using Clock = std::chrono::steady_clock;
    using Tick = std::chrono::duration<std::int64_t, std::ratio<1, 60>>;
    using Fine = std::common_type_t<Clock::duration, Tick>;

    static constexpr int kMaxTicksPerFrame = 4;  // beyond this the game slows down, like the original
    static constexpr Fine kVsyncSnap = std::chrono::microseconds(500);
    static constexpr Fine kMaxFrameTime = std::chrono::milliseconds(100);

    void reset(Clock::time_point now) {
        last_ = now;
        accumulator_ = Fine::zero();
    }
    // Practice slow motion: game time runs at percent / 100 of real time.
    void setSpeed(int percent) { speedPercent_ = std::clamp(percent, 10, 100); }
    void setDisplay(int refreshHz, bool vsync) {
        refreshHz_ = refreshHz;
        vsync_ = vsync;
    }

    // Measures the frame time up to `now` and returns how many ticks to run (0..4).
    int advance(Clock::time_point now);
    // Advances by an exact amount (tests and headless runs).
    int advanceBy(Fine elapsed);
    // Interpolation factor between the previous and the latest tick, in (0, 1].
    float alpha() const;
    // Real time until another tick becomes due (for sleeping when vsync is unavailable).
    Fine untilNextTick() const { return -accumulator_; }

  private:
    Clock::time_point last_{};
    Fine accumulator_ = Fine::zero();
    int refreshHz_ = 60;
    bool vsync_ = false;
    int speedPercent_ = 100;
};


inline int FixedTimestep::advance(Clock::time_point now) {
    Fine dt = now - last_;
    last_ = now;
    return advanceBy(dt);
}

inline int FixedTimestep::advanceBy(Fine dt) {
    if (dt < Fine::zero()) dt = Fine::zero();
    if (vsync_ && refreshHz_ > 0) {
        // One display period in Fine units (exact for 50/60/100/120/144... Hz up to rounding
        // of one 1/3e9 s unit, which the run-ahead formulation tolerates).
        const double period = static_cast<double>(Fine::period::den) / (static_cast<double>(Fine::period::num) * refreshHz_);
        const double k = std::round(static_cast<double>(dt.count()) / period);
        if (k >= 1.0) {
            const Fine snapped(static_cast<Fine::rep>(std::llround(k * period)));
            const Fine diff = dt > snapped ? dt - snapped : snapped - dt;
            if (diff < kVsyncSnap) dt = snapped;
        }
    }
    if (speedPercent_ != 100) dt = Fine(dt.count() * speedPercent_ / 100);
    accumulator_ += std::min(dt, kMaxFrameTime);
    int ticks = 0;
    while (accumulator_ > Fine::zero()) {
        if (ticks == kMaxTicksPerFrame) {
            accumulator_ = Fine::zero();  // drop the backlog: the game slows down instead
            break;
        }
        accumulator_ -= Tick(1);
        ++ticks;
    }
    return ticks;
}

inline float FixedTimestep::alpha() const {
    const double tick = static_cast<double>(Fine(Tick(1)).count());
    const double a = 1.0 + static_cast<double>(accumulator_.count()) / tick;
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

}  // namespace touhou
