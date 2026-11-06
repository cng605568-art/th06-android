#pragma once

// Numeric primitives of the 2002 engine, reproduced exactly where it matters for game
// logic: single-precision pi constants, the angle normaliser, and ZUN's 16-bit RNG.
// (Sources: the th06 decompilation, CC0 - Global.cpp, ZunMath.hpp.)

#include <cmath>
#include <cstdint>

namespace touhou {

inline constexpr float kZunPi = 3.14159265358979323846f;
inline constexpr float kZunTwoPi = kZunPi * 2.0f;
inline constexpr float kZunHalfPi = kZunPi / 2.0f;

struct Vec3f {
    float x = 0, y = 0, z = 0;

    constexpr Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3f operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3f& operator+=(const Vec3f& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
};

inline Vec3f lerp(const Vec3f& a, const Vec3f& b, float t) { return a + (b - a) * t; }

// utils::AddNormalizeAngle: a + b wrapped into [-pi, pi], at most 17 wrap steps.
inline float addNormalizeAngle(float a, float b) {
    int i = 0;
    a += b;
    while (a > kZunPi) {
        a -= kZunTwoPi;
        if (i++ > 16) break;
    }
    while (a < -kZunPi) {
        a += kZunTwoPi;
        if (i++ > 16) break;
    }
    return a;
}

// sincosmul: x = cos(angle) * m, y = sin(angle) * m, z untouched.
inline void sincosmul(Vec3f& out, float angle, float m) {
    out.x = std::cos(angle) * m;
    out.y = std::sin(angle) * m;
}

// The game's RNG (one global instance in the original). Bit-exact.
class ZunRng {
  public:
    explicit ZunRng(std::uint16_t seed = 0) : seed_(seed) {}

    void seed(std::uint16_t s) {
        seed_ = s;
        generations_ = 0;
    }
    std::uint16_t currentSeed() const { return seed_; }
    std::uint32_t generations() const { return generations_; }

    std::uint16_t u16() {
        const auto a = static_cast<std::uint16_t>((seed_ ^ 0x9630u) - 0x6553u);
        seed_ = static_cast<std::uint16_t>((((a & 0xc000u) >> 14) + a * 4u) & 0xffffu);
        ++generations_;
        return seed_;
    }
    std::uint32_t u32() {
        const std::uint32_t hi = u16();
        return (hi << 16) | u16();
    }
    float f32ZeroToOne() { return static_cast<float>(u32()) / static_cast<float>(0xffffffffu); }

    std::uint16_t u16InRange(std::uint16_t range) { return range != 0 ? u16() % range : 0; }
    std::uint32_t u32InRange(std::uint32_t range) { return range != 0 ? u32() % range : 0; }
    float f32InRange(float range) { return f32ZeroToOne() * range; }

  private:
    std::uint16_t seed_;
    std::uint32_t generations_ = 0;
};

// D3DCOLOR helpers (0xAARRGGBB).
inline constexpr std::uint32_t colorCombineAlpha(std::uint32_t rgbFrom, std::uint32_t alphaFrom) {
    return (rgbFrom & 0x00ffffffu) | (alphaFrom & 0xff000000u);
}
inline constexpr std::uint32_t argbToRgbaBytes(std::uint32_t argb) {
    // SpriteInstance colour bytes are R, G, B, A in memory (little-endian u32).
    return ((argb >> 16) & 0xffu) | (argb & 0xff00u) | ((argb & 0xffu) << 16) | (argb & 0xff000000u);
}

}  // namespace touhou
