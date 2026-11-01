#pragma once

// The 3D stage background from the player's data/stageN.std and data/stgNbg.anm: objects
// made of quads (each an ANM animation), placed by an instance list, seen through a
// perspective camera that a small script moves along keyframes, with linear fog. Drawn in
// four layers behind everything else, culled per instance by the projected height of its
// box. During a spell card the background fades to black over 60 frames and the stage's
// spell-card background (eff0N.anm script 0) replaces it.
//
// It has no effect on gameplay.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"

namespace th06 {

class Vfs;

inline constexpr int kAnmFileStageBg = 4;
inline constexpr int kAnmOffsetStageBg = 0x300;

class StageBackground {
  public:
    enum class Spell : std::uint8_t { Off, Running, FullyOn };

    struct Quad {
        std::int16_t type = 0;
        std::int16_t script = 0;
        int vm = -1;  // animation instance index
        Vec3f pos;
        float width = 0, height = 0;
    };
    struct Object {
        std::int8_t layer = 0;
        Vec3f boxOrigin, boxSize;
        std::vector<Quad> quads;
        bool animating = true;
    };
    struct Instance {
        int object = 0;
        Vec3f pos;
    };
    struct Record {
        std::int32_t time = 0;
        std::int16_t op = 0;
        std::uint32_t a[3] = {0, 0, 0};
    };

    // Parses an STD image; false (and why) if it is malformed. Exposed for tests.
    bool parse(std::span<const std::uint8_t> bytes, std::string* error);
    // Loads stgNbg.anm and stageN.std, starts every quad's script. False if either is
    // missing (the stage then has no background).
    bool load(World& world, const Vfs& vfs, int stage);
    // Starts every quad's script (after parse()).
    void start(World& world);
    void clear();
    bool loaded() const { return loaded_; }

    void update(World& world);  // the stage pass (before the player)
    // Layers 0..3, the spell-card fade and background. `white` is a solid texel region.
    void draw(World& world, SpriteSink& sink, const SpriteRegion& white, float alpha, bool loadingHalted);

    void spellStart() {
        spell_ = Spell::Running;
        spellCounter_ = 0;
        spellRestarted_ = false;
    }
    void spellEnd() { spell_ = Spell::Off; }
    void unpause() { unpause_ = true; }

    // The fog colour (ARGB): the playfield is cleared to it.
    std::uint32_t fogColor() const { return fogColor_; }

    // Inspection (tests).
    const Vec3f& cameraPosition() const { return camera_; }
    const Vec3f& aim() const { return aim_; }
    float fogStart() const { return fogStartZ_; }
    float fogEnd() const { return fogEndZ_; }
    std::int32_t scriptTime() const { return scriptTime_; }
    Spell spell() const { return spell_; }
    std::int32_t spellCounter() const { return spellCounter_; }
    std::uint32_t spellColor() const { return spellVm_.color; }
    const std::vector<Object>& objects() const { return objects_; }
    const std::vector<Instance>& instances() const { return instances_; }
    // Projects an STD-space point with the background camera: screen x, y and eye depth.
    void project(const Vec3f& stdPoint, float& sx, float& sy, float& eyeZ) const;
    bool instanceVisible(const Instance& inst, const Vec3f& camera) const;

  private:
    struct Camera {
        Vec3f eye, right, up, forward;
        float x = 32, y = 16, w = 384, h = 448;
        float cot = 0;
    };
    void buildCamera(Camera& cam, const Vec3f& aim) const;
    void drawLayer(World& world, SpriteSink& sink, int layer, const Vec3f& camera);
    void drawQuad(World& world, SpriteSink& sink, AnmVm& vm, const Vec3f& pos, const Quad& q);

    bool loaded_ = false;
    std::vector<Object> objects_;
    std::vector<Instance> instances_;
    std::vector<Record> script_;
    std::vector<AnmVm> vms_;

    std::int32_t scriptTime_ = 0;
    std::size_t instr_ = 0;
    Vec3f camera_, prevCamera_;
    Vec3f p0_, p1_;
    std::int32_t t0_ = 0, t1_ = 0;
    std::uint32_t fogColor_ = 0xff000000u;
    float fogStartZ_ = 200.0f, fogEndZ_ = 500.0f;
    std::uint32_t fogFromColor_ = 0, fogToColor_ = 0;
    float fogFromStart_ = 0, fogFromEnd_ = 0, fogToStart_ = 0, fogToEnd_ = 0;
    std::int32_t fogDuration_ = 0, fogTimer_ = 0;
    Vec3f aimFrom_{0, 0, 1}, aimTo_{0, 0, 1};
    std::int32_t aimDuration_ = 1, aimTimer_ = 0;
    Vec3f aim_{0, 0, 1};        // the current aim (survives stage changes, like the original's)
    Vec3f cameraAim_{0, 0, 1};  // the aim the camera is built from: last frame's
    bool unpause_ = false;
    Spell spell_ = Spell::Off;
    std::int32_t spellCounter_ = 0;
    bool spellRestarted_ = false;
    AnmVm spellVm_;
    Camera camera3D_;  // built at the start of each update from the playfield and last aim
};

}  // namespace th06
