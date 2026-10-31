#pragma once

#include <cmath>
#include <cstdint>

namespace th06 {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTau = 2.0f * kPi;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2& operator+=(Vec2 o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    constexpr float lengthSquared() const { return x * x + y * y; }
    float length() const { return std::sqrt(lengthSquared()); }
    float angle() const { return std::atan2(y, x); }

    static Vec2 fromAngle(float radians, float length = 1.0f) {
        return {std::cos(radians) * length, std::sin(radians) * length};
    }
};

// Deterministic xorshift32 so demo patterns replay identically on every device.
class Rng {
  public:
    explicit constexpr Rng(std::uint32_t seed = 0x6a09e667u) : state_(seed ? seed : 1u) {}

    constexpr std::uint32_t next() {
        std::uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    // Uniform in [0, 1).
    constexpr float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    constexpr float range(float lo, float hi) { return lo + (hi - lo) * unit(); }

  private:
    std::uint32_t state_;
};

}  // namespace th06
