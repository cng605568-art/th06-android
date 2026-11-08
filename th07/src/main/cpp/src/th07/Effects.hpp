#pragma once

// Perfect Cherry Blossom's effect manager: 400 pooled effects, 8 fixed special slots and a
// sink, each an ANM VM plus a small motion routine chosen by the effect id (from the id
// table in th07.exe). Effects matter to the simulation: their init routines draw from the
// gameplay RNG, and a full pool spawns fewer of them (so draws fewer numbers).

#include <array>
#include <cstdint>

#include "core/ZunMath.hpp"
#include "th07/Anm.hpp"
#include "th07/ExeTables.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

class Background;
struct World;

struct Effect {
    AnmVm vm;
    Vec3f pos, custom, velocity, accel, base, emitter, direction;
    float radius = 0, angularVelocity = 0;
    Timer timer;
    ExeTables::EffectUpdate update = ExeTables::EffectUpdate::None;
    bool inUse = false;
    std::int8_t id = 0;
    bool fading = false;
    std::int32_t fadeCounter = 0;
    std::uint8_t drawType = 0;  // 0 2D, 1 billboard (stage), 2 billboard orbit marker, 3 projected (stage)
};

class Effects {
  public:
    static constexpr int kNormal = 400;
    static constexpr int kSpecial = 8;
    static constexpr int kSlots = kNormal + kSpecial + 1;  // + the sink

    void reset();  // stage start: the whole pool zeroed
    // Fills `count` free normal slots (count <= 0: every free slot) and returns the last one
    // filled, or the sink when the pool ran out.
    Effect& spawn(const World& w, int id, const Vec3f& pos, int count, std::uint32_t color);
    Effect& spawnWithVector(const World& w, int id, const Vec3f& pos, const Vec3f& custom, int count, std::uint32_t color);
    // Overwrites special slot 0-7 (0 bomb ring, 1 boss ring, 2 focus marker, 3 death flash,
    // 4 border).
    Effect& spawnSpecial(const World& w, int id, const Vec3f& pos, int slot, std::uint32_t color);
    Effect& special(int slot) { return slots_[static_cast<std::size_t>(kNormal + slot)]; }
    Effect& normal(int i) { return slots_[static_cast<std::size_t>(i)]; }  // 0..kNormal-1 (ECL ex routine 9)

    void update(const World& w);
    // The effect pass (draw order 9): layers 0, 2 and 3. Layer 1 (weather) belongs to the
    // stage's 3D pass.
    void draw(SpriteSink& sink, const AnmDrawState& state);
    // Layer 1 (weather and camera-attached effects), from the stage's 3D pass.
    void drawStageLayer(SpriteSink& sink, const AnmDrawState& state, Background& bg);
    int active() const { return active_; }
    // ECL opcode 140: effect 20's colour is multiplied by (r, g, b, a) while it is drawn
    // (each channel truncated, at most 255). The stages set (1, 1, 1, 1) on their second
    // frame and fade it later. Render only.
    void setColorMultiplier(float r, float g, float b, float a) { colorMultiplier_ = {r, g, b, a}; }

  private:
    Effect& fill(const World& w, int id, const Vec3f& pos, const Vec3f& custom, int count, std::uint32_t color, bool zwriteOff);
    void init(const World& w, Effect& e, int id, const Vec3f& pos, std::uint32_t color, const Vec3f& custom, bool zwriteOff);
    bool runUpdate(const World& w, Effect& e);  // false: free it

    std::array<Effect, kSlots> slots_{};
    int cursor_ = 0;
    int active_ = 0;
    std::uint32_t frames_ = 0;
    std::array<std::array<std::int16_t, kSlots>, 4> layers_{};
    std::array<int, 4> layerCount_{};
    std::array<float, 4> colorMultiplier_{1.0f, 1.0f, 1.0f, 1.0f};
    Background* background_ = nullptr;  // the playfield camera for layer 2 (from the World)
};

}  // namespace touhou::th07
