#pragma once

// Particle effects: a 512-slot ring pool of short animations (etama4.anm scripts 3..19,
// plus the stage's eff0N.anm for one type). Effects never touch gameplay, but several types
// draw random numbers on their first update, so spawning exactly the original's effects
// keeps the shared RNG (and so replays and bullet patterns) in step.
//
// Motion per type: none, splash (random burst that decelerates over 19/20 frames), drift,
// converge (jumps 256 px out, then falls back through its spawn point) and orbit (spell
// circles driven by their enemy). Effects end when their script ends, when an orbit fade
// completes, or at stage start. Drawn in true 3D: depth z shrinks and pulls them toward
// the playfield centre.

#include <array>
#include <cstdint>

#include "core/ZunMath.hpp"
#include "th06/World.hpp"
#include "gfx/AnmManager.hpp"

namespace touhou::th06 {

inline constexpr int kAnmFileEffects = 10;
inline constexpr int kAnmOffsetEffects = 0x2b3;

// Effect types used by the engine.
enum EffectType : int {
    EffectBubbleSmall = 0,
    EffectGlowBig = 3,     // big splash
    EffectParticle = 4,    // 4..7 white/red/green/blue, 8..11 small versions (splash)
    EffectParticleWhite = 4,
    EffectParticleRed = 5,
    EffectParticleGreen = 6,
    EffectParticleSmall = 8,
    EffectFlash = 12,
    EffectOrbit = 13,      // 13..15
    EffectSpellBackground = 16,
    EffectConverge = 17,   // 17 (60 frames), 18 (240 frames)
    EffectDrift = 19,
};

class EffectManager {
  public:
    static constexpr int kMaxEffects = 512;
    static constexpr int kSink = kMaxEffects;  // where failed spawns write
    static constexpr int kTypes = 20;

    enum class Motion : std::uint8_t { None, Splash, BigSplash, Drift, Converge60, Converge240, Orbit };

    struct Effect {
        AnmVm vm;
        Vec3f pos, prevPos;
        Vec3f velocity, acceleration;  // splash / drift (not reset at spawn)
        Vec3f anchor;                  // converge, orbit (not reset at spawn)
        Vec3f axis;                    // converge direction, orbit axis (not reset at spawn)
        float radius = 0.0f;           // orbit (not reset at spawn)
        float halfAngle = 0.0f;        // orbit (not reset at spawn)
        std::int32_t timer = 0;
        std::int8_t fadeCounter = 0;
        std::uint8_t type = 0;
        Motion motion = Motion::None;
        bool inUse = false;
        bool fading = false;
    };

    void reset();
    // Spawns `count` effects of `type` at `pos` with colour `color` (ARGB), in consecutive
    // free slots of the ring. Returns the last slot filled, or kSink when the pool ran out
    // before `count` were placed (a count <= 0 fills every free slot).
    int spawn(World& world, int type, const Vec3f& pos, int count, std::uint32_t color);
    void update(World& world);
    void draw(World& world, SpriteSink& sink, float alpha);

    Effect& at(int i) { return effects_[static_cast<std::size_t>(i)]; }
    const Effect& at(int i) const { return effects_[static_cast<std::size_t>(i)]; }
    int activeCount() const;
    int cursor() const { return cursor_; }

    static std::uint32_t colorTable(int index);  // ECL 102 colours

  private:
    std::array<Effect, kMaxEffects + 1> effects_{};
    int cursor_ = 0;
    // Playfield rectangle as it was at the start of the frame (the original's camera is set
    // up before screen shake moves the playfield, so effects follow shake a frame late).
    float viewX_ = kGameRegionX, viewY_ = kGameRegionY;
    float viewW_ = kGameRegionWidth, viewH_ = kGameRegionHeight;
};

// Spawns through world.effects (returns EffectManager::kSink when there is none).
int spawnEffect(World& world, int type, const Vec3f& pos, int count, std::uint32_t color = 0xffffffffu);

}  // namespace touhou::th06
