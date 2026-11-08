#pragma once

// Perfect Cherry Blossom's stage backgrounds: stageN.std (objects of ANM-driven quads placed
// by an instance list, and a script of camera, fog and backdrop instructions) and the 3D
// stage camera. The script, the camera and fog interpolation and the quad VMs run in the
// logic pass (order 7); the two draw passes (orders 3 and 4) render layers 0-1 and 2-3 with
// the stage camera. The camera's position and normalised facing feed gameplay (the weather
// particles), so they are computed in single precision in the original's order.

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "gfx/SpriteSink.hpp"
#include "th07/Anm.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th07 {

using Mat4 = std::array<float, 16>;  // row-vector convention, m[row * 4 + column]

Vec3f normalize(const Vec3f& v);
Vec3f cross(const Vec3f& a, const Vec3f& b);
float dot(const Vec3f& a, const Vec3f& b);
Mat4 lookAtLH(const Vec3f& eye, const Vec3f& at, const Vec3f& up);
Mat4 perspectiveLH(float fovY, float aspect, float zn, float zf);
Mat4 multiply(const Mat4& a, const Mat4& b);

// The playfield viewport (the Direct3D viewport the stage draws into).
struct Viewport {
    float x = 32, y = 16, w = 384, h = 448;
};

class Background {
  public:
    static constexpr int kScriptBase = 0x300;  // stgNbg.anm's index offset

    // Loads data/stgNbg.anm (+ stage 4's four extra files) and data/stageN.std, starts every
    // quad's script, sets the initial fog and camera.
    bool load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, ZunRng& rng, int stage, std::string* error);
    void release(AnmLibrary& anm);
    bool loaded() const { return !objects_.empty() || !script_.empty(); }

    void update(AnmLibrary& anm, ZunRng& rng);
    void drawHigh(SpriteSink& sink, AnmDrawState& state);  // draw order 3: backdrops, layers 0-1
    void drawLow(SpriteSink& sink, AnmDrawState& state);   // draw order 4: layers 2-3
    // The effect manager's layer-1 effects (weather) are drawn inside the low pass, after
    // layers 2 and 3, with the stage camera and the depth test.
    std::function<void(SpriteSink&, AnmDrawState&)> drawWeather;
    // A camera-facing sprite at a 3D position (the VM's position), hand-fogged, as the
    // background's facing quads are drawn.
    void drawBillboard(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state) {
        if (vm.sprite) drawFacingQuad(sink, vm, state, vm.sprite->pixelWidth);
    }
    // The playfield camera's screen-space paths (ANM spec 4.6), drawn on top of everything
    // (the depth function is "always" outside the stage pass) and clipped to the playfield:
    // "draw projected" (the VM's 3D-rotated quad, affine textured; the GUI's stage
    // transition tiles) and "draw billboard" (a camera-facing quad sized by the stage
    // camera's right vector; the effects' orbiting spell circles). Field coordinates.
    void drawProjected(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state);
    void drawPlayfieldBillboard(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state);
    const SpriteRegion* white = nullptr;  // for the darkening rectangle of a starting card

    // ECL: resume the script at the matching label (opcode 31).
    void setResumeLabel(std::int32_t label) { resumeLabel_ = label; }

    // Spell-card backgrounds (spec 2.6 step 8, 2.7, 2.13): a boss's card start sets the state
    // to starting and starts VM i on script 0x2DC + base + i (first tick at once: RNG); 60
    // frames later the state becomes active and the stage layers stop drawing. The VMs run
    // here, in the background's logic slot. Base 0 and the per-stage count are set at load;
    // the boss dialogue may change them.
    enum class SpellState : std::uint8_t { Inactive, Starting, Active };
    static constexpr int kSpellScripts = 0x2DC;
    void spellStart(AnmLibrary& anm, ZunRng& rng);
    void spellEnd() { spellState_ = SpellState::Inactive; }
    void spellInterrupt(int vm, int label);  // ECL ex routines 10 and 11 (VMs 0 and 1)
    void setSpellBackgrounds(int base, int count);
    SpellState spellState() const { return spellState_; }
    std::int32_t spellCounter() const { return spellCounter_; }
    // A jump's teleport shifts the weather particles by this much (the effect manager hooks in).
    std::function<void(const Vec3f&)> onWeatherShift;

    const std::string& songPath(int k) const { return songs_[static_cast<std::size_t>(k & 3)]; }
    const Vec3f& cameraPosition() const { return cam_.pos; }
    const Vec3f& cameraFacing() const { return cam_.facing; }  // the "look-at" vector, relative
    const Vec3f& normalizedFacing() const { return facingN_; }
    std::int32_t scriptTime() const { return time_.current; }
    Viewport viewport;

  private:
    struct Quad {
        std::int16_t type = 0;
        std::int16_t script = 0;
        int vm = -1;
        Vec3f pos;
        float width = 0, height = 0;
    };
    struct Object {
        std::int8_t layer = 0;
        std::uint8_t flags = 0;
        Vec3f origin, size;
        std::vector<Quad> quads;
    };
    struct Instance {
        int object = 0;
        Vec3f pos;
    };
    struct Record {
        std::int32_t frame;
        std::int16_t op;
        std::array<std::uint32_t, 3> a;
        float f(int i) const;
        std::int32_t i(int k) const { return static_cast<std::int32_t>(a[static_cast<std::size_t>(k)]); }
    };
    struct Channel {  // one camera channel: 0 position, 1 facing, 2 up (FOV is separate)
        Vec3f start, target, tangent0, tangent1;
        std::int32_t duration = 0;
        Timer counter;
        std::int32_t ease = 0;
    };
    struct Camera {
        Vec3f pos{0, 0, 1000}, facing{0, 0, 0}, up{0, 1, 0};
        float fov = kZunPi / 6.0f;
    };
    struct Fog {
        std::uint32_t color = 0xFF000000u;
        float nearZ = 200, farZ = 500;
    };

    void execute(const Record& r);
    void drawLayer(SpriteSink& sink, AnmDrawState& state, int layer);
    void draw3DQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, int cameraMode);
    void updateWorldMatrix(AnmVm& vm);
    void emitScreenQuad(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, const float xs[4], const float ys[4]);
    void drawFacingQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, float width);
    void buildStageCamera();
    void buildPlayfieldCamera();

    std::vector<Object> objects_;
    std::vector<Instance> instances_;
    std::vector<Record> script_;
    std::vector<AnmVm> vms_;
    std::array<std::string, 4> songs_;
    int stage_ = 0;

    Vec3f offset_;  // P, subtracted from every instance
    Camera cam_;
    std::array<Channel, 3> channels_{};
    float fovStart_ = 0, fovTarget_ = 0;
    std::int32_t fovDuration_ = 0;
    Timer fovCounter_;
    std::int32_t fovEase_ = 0;
    Vec3f facingN_;
    Fog fog_, fogTarget_, fogOrigin_;
    std::int32_t fogDuration_ = 0;
    Timer fogCounter_;
    std::uint32_t clearColor_ = 0;
    AnmVm backdropA_, backdropB_;
    Timer time_;
    std::size_t index_ = 0;
    std::int32_t resumeLabel_ = 0;
    bool teleported_ = false;
    std::uint32_t frames_ = 0;
    SpellState spellState_ = SpellState::Inactive;
    std::int32_t spellCounter_ = 0;
    int spellBase_ = 0, spellCount_ = 1;
    std::array<AnmVm, 2> spellVms_{};

    // Draw-time state.
    Mat4 view_{}, proj_{};
    int currentCamera_ = -1;  // 0 playfield, 1 stage, -1 unknown
    const AnmSprite* uvCacheSprite_ = nullptr;
    float uvCacheU_ = 0, uvCacheV_ = 0;
};

}  // namespace touhou::th07
