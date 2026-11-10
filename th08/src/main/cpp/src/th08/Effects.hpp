#pragma once

// Imperishable Night's effect manager (claude/th08-specs/spec_stage_gui.md §4): 653 effects
// (a ring of 512, 128 for other allocators, 13 special slots), each an etama.anm VM plus the
// motion routine its type selects (from the effect table in th08.exe). Effects matter to the
// simulation: their scripts (run one tick at spawn) and init routines draw from the gameplay
// RNG, and a full ring spawns fewer of them. Ported so far: allocation, the script start, the
// update loop and the draw lists; the per-type init and update routines follow their spec.

#include <array>
#include <cstdint>

#include "core/ZunMath.hpp"
#include "th08/Anm.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

class Background;
struct World;

struct Effect {
    AnmVm vm;
    Vec3f pos;
    Timer timer;
    std::uint32_t update = 0;  // the routine's address (0: none)
    bool inUse = false;
    std::uint8_t type = 0;
    std::uint8_t layer = 0;    // 0 playfield (lists A/E, or D), 1 and >= 3 stage (B), 2 billboard (C)
    bool listD = false;        // +0x355: drawn with the bullets
    bool ignoreFreeze = false; // +0x357
};

class Effects {
  public:
    static constexpr int kRing = 512, kOther = 128, kSpecial = 13;
    static constexpr int kSlots = kRing + kOther + kSpecial;
    static constexpr int kSlotEtama = 6;  // etama.anm

    void reset();  // stage start: the whole pool zeroed
    // SpawnEffect (spec 4.2): fills `count` free ring slots and returns the last one filled,
    // or the sink when the ring ran out (fewer scripts and inits run: fewer RNG draws).
    Effect& spawn(const World& w, int type, const Vec3f& pos, int count, std::uint32_t color);
    // The special allocator: overwrites slot 640 + k.
    Effect& spawnSpecial(const World& w, int type, const Vec3f& pos, int k, int count, std::uint32_t color);
    Effect& special(int k) { return slots_[static_cast<std::size_t>(kRing + kOther + k)]; }
    bool isSink(const Effect& e) const { return &e == &sink_; }

    // Calc priority 13 (spec 4.3).
    void update(const World& w);
    // Draw priority 12: lists A (layer 0), C (layer 2, billboards) and E (layer 0, additive).
    void draw(SpriteSink& sink, const AnmDrawState& state, Background* bg);
    // List D (layer 0 marked "with the bullets"), from the bullet manager's draw.
    void drawListD(SpriteSink& sink, const AnmDrawState& state);
    // List B (layers 1 and >= 3: weather, glows), inside the stage's low pass.
    void drawListB(SpriteSink& sink, const AnmDrawState& state, Background& bg);
    int active() const { return active_; }
    std::uint32_t frames() const { return frames_; }
    void setQuality(int quality) { quality_ = quality; }

  private:
    void start(const World& w, Effect& e, int type, const Vec3f& pos, std::uint32_t color);
    bool runInit(const World& w, Effect& e);    // false: the init refused (slot freed)
    bool runUpdate(const World& w, Effect& e);  // false: free it

    std::array<Effect, kSlots> slots_{};
    Effect sink_;
    int cursor_ = 0;
    int active_ = 0;
    std::uint32_t frames_ = 0;
    int quality_ = 2;
    // This frame's draw lists (slot indices), rebuilt by the update.
    std::array<std::array<std::int16_t, kSlots>, 5> lists_{};
    std::array<int, 5> listCount_{};
};

}  // namespace touhou::th08
