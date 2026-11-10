#pragma once

// Imperishable Night's stage backgrounds (claude/th08-specs/spec_stage_gui.md §1, as deltas
// from TH07): stageN.std (objects of ANM-driven quads placed by an instance list, and a
// script of camera, fog and backdrop instructions) and the 3D stage camera. New in TH08: a
// camera eye offset and sway (ops 32/33), a third backdrop VM that drives the clear colour
// (op 34), 36-byte "ribbon" quads drawn between two 3D points (the bamboo of 4A/4B/Extra), a
// per-stage cull distance, translucent clear colours, the human/youkai tint of stage 3's
// quads, and the lantern glow points the effects spawn from.
//
// The script, the camera and fog interpolation and the quad VMs run in the logic pass
// (priority 8); the two draw passes (6 and 7) render layers 0-1 and 2-3 with the stage
// camera. The camera's position and normalised facing feed gameplay (the weather
// particles), so they are computed in single precision in the original's order.

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "gfx/SpriteSink.hpp"
#include "th08/Anm.hpp"

namespace touhou {
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th08 {

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

// Stage indices (spec_flow_menus §6.1): 0-2 stages 1-3, 3 = 4A, 4 = 4B, 5 = stage 5,
// 6 = 6A, 7 = 6B, 8 = Extra.
inline constexpr int kStageCount = 9;
const char* stageFileName(int stage);       // "stage4a"
const char* stageBackgroundAnm(int stage);  // "stg4abg.anm" (4A and 4B share it)

class Background {
  public:
    static constexpr int kSlotBackground = 4;  // stgNbg.anm
    static constexpr int kSlotEffects = 9;     // effNN.anm: the spell-card backgrounds
    static constexpr int kTintScript = 33;     // text.anm: the human/youkai tint VM
    static constexpr int kMaxGlowPoints = 32;

    // Loads stgNbg.anm into slot 4 and stageN.std (stageN_s.std in spell practice), starts
    // every quad's script and the tint VM, sets the initial fog and camera (spec 1.1).
    bool load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, ZunRng& rng, int stage, bool spellPractice,
              std::string* error);
    void release(AnmLibrary& anm);
    bool loaded() const { return loaded_; }

    // The logic pass (spec 1.4). `frozen` is GameManager flag bit 10: nothing runs.
    void update(AnmLibrary& anm, ZunRng& rng, bool frozen = false);
    void drawHigh(SpriteSink& sink, AnmDrawState& state);  // draw priority 6: backdrops, layers 0-1
    void drawLow(SpriteSink& sink, AnmDrawState& state);   // draw priority 7: layers 2-3, spell backgrounds
    // Effect list B (weather, glows) is drawn inside the low pass, after layers 2 and 3.
    std::function<void(SpriteSink&, AnmDrawState&)> drawEffectsB;
    // The 6B moon effect's draw callback, drawn in the high pass after the backdrops.
    std::function<void(SpriteSink&, AnmDrawState&)> drawMoon;
    // The spell-background draw callback (stage 4A's "window", ECL ex 3/6/20), after the
    // spell VMs in the low pass.
    std::function<void(SpriteSink&, AnmDrawState&)> spellDrawCallback;
    // The glow particles (spec 1.4 step 13): twelve type-62 effects every third frame from
    // stage frame 700, at the recorded lantern points (or (0, 0, 0)); the effects hook in.
    std::function<void(const Vec3f& point)> spawnGlow;
    // A jump's teleport shifts the weather particles by this much (the effect manager hooks in).
    std::function<void(const Vec3f&)> onWeatherShift;

    // A camera-facing sprite at a 3D position (the VM's position), hand-fogged, as the
    // background's facing quads are drawn.
    void drawBillboard(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state) {
        if (vm.sprite) drawFacingQuad(sink, vm, state, vm.sprite->pixelWidth);
    }
    // The playfield camera's screen-space paths (ANM spec 4.6): "projected" and "billboard".
    void drawProjected(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state);
    void drawPlayfieldBillboard(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state);

    const SpriteRegion* white = nullptr;  // for filled rectangles (translucent clears, card fade)
    bool stageFinished = false;  // set by the game before drawing: the stage-clear picture stopped
    bool spellPractice = false;  // glow spawns from the first frame

    // ECL: resume the script at the matching label (opcode 31).
    void setResumeLabel(std::int32_t label) { resumeLabel_ = label; }
    std::int32_t resumeLabel() const { return resumeLabel_; }
    // Player+5: the youkai is in front (the stage-3 tint follows it, spec 1.6).
    void setYoukaiInFront(bool youkai) { youkaiFront_ = youkai; }
    // Bombs: the stage tint (0x409160) and the darkening flag (0x40BC60).
    void setTint(std::uint32_t argb) { tint_ = argb; }
    void setDarkening() { darkening_ = true; }

    // Spell-card backgrounds (spec 1.9): a card's start sets the state to starting and starts
    // VM i on slot-9 script base + i; 60 frames later the state becomes active and the stage
    // layers stop drawing. Count 2 and base 0 at stage start.
    enum class SpellState : std::uint8_t { Inactive, Starting, Active };
    void spellStart(AnmLibrary& anm, ZunRng& rng);
    void spellEnd() { spellState_ = SpellState::Inactive; }
    void spellInterrupt(int label);  // ECL ex 26/28/29: both VMs
    void setSpellBackgrounds(int base, int count);
    int spellBase() const { return spellBase_; }
    int spellCount() const { return spellCount_; }
    SpellState spellState() const { return spellState_; }
    std::int32_t spellCounter() const { return spellCounter_; }
    std::array<AnmVm, 32>& spellVms() { return spellVms_; }

    const std::string& songPath(int k) const { return songs_[static_cast<std::size_t>(k & 3)]; }
    const Vec3f& cameraPosition() const { return cam_.pos; }
    const Vec3f& cameraFacing() const { return cam_.facing; }  // the "look-at" vector, relative
    const Vec3f& cameraUp() const { return cam_.up; }
    const Vec3f& eyeOffset() const { return cam_.eye; }
    const Vec3f& normalizedFacing() const { return facingN_; }
    float cameraFov() const { return cam_.fov; }
    std::uint32_t clearColor() const { return clearColor_; }
    std::uint32_t fogColor() const { return fog_.color; }
    int swayMode() const { return sway_; }
    std::int32_t scriptTime() const { return time_.current; }
    std::uint32_t frames() const { return frames_; }
    int stage() const { return stage_; }
    float cullDistanceSquared() const { return cull2_; }
    std::size_t quadCount() const { return vms_.size(); }
    std::size_t ribbonCount() const;
    int glowCount() const { return glowCount_; }
    const AnmVm& tintVm() const { return tintVm_; }
    const AnmVm& quadVm(std::size_t i) const { return vms_[i]; }
    Viewport viewport;

  private:
    struct Quad {
        std::int16_t type = 0;  // 0: a sprite quad; 1: a ribbon from A to B
        std::int16_t script = 0;
        int vm = -1;
        Vec3f pos;               // type 1: point A
        float width = 0, height = 0;
        Vec3f pointB;            // type 1
        float ribbonWidth = 0;   // type 1 (0: the sprite's width)
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
        Vec3f pos{0, 0, 1000}, facing{0, 0, 0}, up{0, 1, 0}, eye{0, 0, 0};
        float fov = kZunPi / 6.0f;
    };
    struct Fog {
        std::uint32_t color = 0xFF000000u;
        float nearZ = 200, farZ = 500;
    };

    void execute(const Record& r, AnmLibrary& anm, ZunRng& rng);
    void updateObjects(ZunRng& rng);
    void drawLayer(SpriteSink& sink, AnmDrawState& state, int layer);
    void draw3DQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, int cameraMode);
    void drawRibbon(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, const Quad& q, const Vec3f& shift);
    void updateWorldMatrix(AnmVm& vm);
    void emitScreenQuad(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, const float xs[4], const float ys[4]);
    // Draws a facing quad; true when it got past the fog test (the glow recording follows).
    bool drawFacingQuad(SpriteSink& sink, AnmVm& vm, const AnmDrawState& state, float width);
    Vec3f eyePosition() const { return cam_.pos + cam_.eye; }
    void buildStageCamera();
    void buildPlayfieldCamera();

    bool loaded_ = false;
    std::vector<Object> objects_;
    std::vector<Instance> instances_;
    std::vector<Record> script_;
    std::vector<AnmVm> vms_;
    std::array<std::string, 4> songs_;
    int stage_ = -1;
    float cull2_ = 1150.0f * 1150.0f;

    Vec3f offset_;  // P, subtracted from every instance
    Camera cam_;
    std::array<Channel, 3> channels_{};
    float fovStart_ = 0, fovTarget_ = 0;
    std::int32_t fovDuration_ = 0;
    Timer fovCounter_;
    std::int32_t fovEase_ = 0;
    int sway_ = 0;          // op 33
    Timer swayClock_;       // channel 4's timer
    Vec3f facingN_;
    Fog fog_, fogTarget_, fogOrigin_;
    std::int32_t fogDuration_ = 0;
    Timer fogCounter_;
    std::uint32_t clearColor_ = 0;
    AnmVm backdropA_, backdropB_, backdropC_;
    AnmVm tintVm_;
    std::uint8_t tintState_ = 0;
    Timer tintTimer_;
    bool youkaiFront_ = false;
    std::uint32_t tint_ = 0x00808080u;
    bool darkening_ = false;
    Timer time_;
    std::size_t index_ = 0;
    std::int32_t resumeLabel_ = 0;
    bool teleported_ = false;
    std::uint32_t frames_ = 0;
    SpellState spellState_ = SpellState::Inactive;
    std::int32_t spellCounter_ = 0;
    int spellBase_ = 0, spellCount_ = 2;
    std::array<AnmVm, 32> spellVms_{};
    std::array<Vec3f, kMaxGlowPoints> glow_{};
    int glowCount_ = 0;
    bool glowEnable_ = false;

    // Draw-time state.
    Mat4 view_{}, proj_{};
    int currentCamera_ = -1;  // 0 playfield, 1 stage, -1 unknown
    const AnmSprite* uvCacheSprite_ = nullptr;
    float uvCacheU_ = 0, uvCacheV_ = 0;
};

}  // namespace touhou::th08
