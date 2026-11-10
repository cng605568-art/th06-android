#pragma once

// Imperishable Night's ANM animation system (claude/th08-specs/spec_anm_text.md): version-3
// .anm files, one file per slot (25 slots), each with its own sprite and script arrays
// numbered by position in the file (the id fields are ignored), and the per-object VM that
// runs their scripts. As in TH07 the VM is part of the simulation (etama.anm's RNG draws,
// lifetimes), so it follows the original's arithmetic exactly: single-precision floats in
// the stated order and timers with sub-frames for the game-speed multiplier.
//
// TH08 over TH07: seven interpolation channels (the alternative colour's RGB and alpha are
// new), the sprite size copied into the VM (every draw path reads the copy), a two-bit blend
// mode, a draw mode, opcodes 82-89 (89 returns from an interrupt to the point the interrupt
// saved), a "frozen" flag that makes a tick do nothing, and the re-laid-out flag word.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/Thtx.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th08 {

// Game-wide timing: the game-speed multiplier (1, or less during a slow-down) and the sticky
// "force integer" flag (set only by ECL ex-routine 29 with an argument >= 2; never by the
// shipped data). Same algorithm as TH07.
struct Timing {
    float speed = 1.0f;
    bool forceInteger = false;
};
inline Timing g_timing;

// A frame counter with a sub-frame fraction, as the game keeps its clocks.
struct Timer {
    std::int32_t current = 0;
    float sub = 0.0f;
    std::int32_t previous = -999;

    void reset() { *this = Timer{}; }
    void assign(std::int32_t v) {
        current = v;
        sub = 0.0f;
        previous = -999;
    }
    void tick();
    void decrement(std::int32_t v = 1);
    void increment(std::int32_t v = 1);
    float asFloat() const { return static_cast<float>(current) + sub; }
};

// A sprite as the game derives it at load time.
struct AnmSprite {
    int slot = -1;              // the owning file's slot
    std::uint16_t texture = 0;  // engine texture id of its entry
    float startX = 0, startY = 0, endX = 0, endY = 0;  // texture pixels
    float textureWidth = 0, textureHeight = 0;
    float uvStartX = 0, uvStartY = 0, uvEndX = 0, uvEndY = 0;
    float pixelWidth = 0, pixelHeight = 0;
    float cols = 1, rows = 1;
};

// One loaded .anm file (the original's file record).
struct AnmFile {
    int slot = -1;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;  // null: empty record
    std::string name;
    int entries = 0;
    std::vector<AnmSprite> sprites;        // all entries' sprites, by position
    std::vector<std::size_t> scripts;      // all entries' scripts: offsets into `bytes`
    std::vector<std::uint16_t> textures;   // one per entry

    bool loaded() const { return bytes != nullptr; }
    const AnmSprite* sprite(int index) const {
        return loaded() && index >= 0 && index < static_cast<int>(sprites.size()) ? &sprites[static_cast<std::size_t>(index)] : nullptr;
    }
    std::uint8_t* script(int index) const {
        return loaded() && index >= 0 && index < static_cast<int>(scripts.size()) ? bytes->data() + scripts[static_cast<std::size_t>(index)]
                                                                                  : nullptr;
    }
    const std::uint8_t* end() const { return loaded() ? bytes->data() + bytes->size() : nullptr; }
};

class AnmLibrary {
  public:
    static constexpr int kSlots = 25;  // usable slots (the original has 256 records, refuses >= 25)

    // Loads `path` into `slot` (released first). Textures are registered with `textures`
    // (bilinear, wrap), one per entry; with `cpuPixels` they are kept CPU-side (dynamic), for
    // files the game draws text or captures into. The port loads synchronously.
    bool load(const Vfs& vfs, TextureLibrary& textures, const std::string& path, int slot, std::string* error,
              bool cpuPixels = false);
    void release(int slot);

    AnmFile* file(int slot) { return slot >= 0 && slot < kSlots ? &files_[static_cast<std::size_t>(slot)] : nullptr; }
    const AnmFile* file(int slot) const { return slot >= 0 && slot < kSlots ? &files_[static_cast<std::size_t>(slot)] : nullptr; }
    bool loaded(int slot) const {
        const AnmFile* f = file(slot);
        return f && f->loaded();
    }

  private:
    std::array<AnmFile, kSlots> files_{};
};

struct Vec2f {
    float x = 0, y = 0;
};

// One animated object's VM state. Game code reads and writes these fields directly.
struct AnmVm {
    enum Flag : std::uint32_t {
        Visible = 1u << 0,
        Active = 1u << 1,
        RotationDirty = 1u << 2,
        ScaleDirty = 1u << 3,
        BlendMask = 3u << 4,  // 0 normal, 1 additive, 2/3 leave the last destination blend
        UseOffset = 1u << 8,
        FlipX = 1u << 9,
        FlipY = 1u << 10,
        AnchorLeft = 1u << 11,
        AnchorTop = 1u << 12,
        ZWriteDisable = 1u << 13,
        Stopped = 1u << 14,
        CameraMode = 1u << 15,
        SkipTransform = 1u << 16,
        UseAltColor = 1u << 17,
        Frozen = 1u << 19,  // a tick does nothing at all
    };
    enum Channel { Position = 0, Color = 1, Alpha = 2, Rotation = 3, Scale = 4, AltColor = 5, AltAlpha = 6, kChannels = 7 };

    // Base block (cleared by initialise()).
    Vec3f rotation, angularVelocity;
    Vec2f scale{1, 1}, scaleGrowth;
    float spriteWidth = 0, spriteHeight = 0;  // the selected sprite's pixel size; draws use these
    Vec2f uvScroll;
    Timer clock, waitTimer;
    std::array<Timer, kChannels> progress{}, duration{};
    std::array<std::uint8_t, kChannels> ease{};
    std::array<std::int32_t, 4> intRegs{};
    std::array<float, 4> floatRegs{};
    std::array<std::int32_t, 2> intRegs2{};
    Vec2f uvVelocity;
    std::array<float, 16> spriteMatrix{}, worldMatrix{}, uvMatrix{};
    std::uint32_t color = 0xFFFFFFFFu;  // 0xAARRGGBB
    std::uint32_t altColor = 0;
    std::uint32_t flags = 0;
    std::int16_t autoRotate = 0;
    std::int16_t pendingInterrupt = 0;
    std::int32_t drawMode = 0;   // 0-5 (draw paths for player VMs, drawVmByMode)
    AnmFile* file = nullptr;     // the file whose scripts and sprites this VM uses

    // Extended block (kept by initialise()).
    Vec3f position;
    std::int16_t activeSprite = -1;
    std::int16_t fileSlot = 0;
    std::int16_t baseSprite = 0;
    std::int16_t scriptIndex = 0;
    std::uint8_t* scriptStart = nullptr;
    std::uint8_t* current = nullptr;  // null: finished
    const std::uint8_t* scriptEnd = nullptr;
    const AnmSprite* sprite = nullptr;
    Timer savedClock;                       // the interrupt's return point (opcode 89)
    std::uint8_t* savedCurrent = nullptr;
    Vec3f posStart, posEnd, rotStart, rotEnd;
    Vec2f scaleStart, scaleEnd;
    std::uint32_t colorStart = 0, colorEnd = 0;
    std::uint32_t altColorStart = 0, altColorEnd = 0;
    Vec3f offset;
    std::int32_t lastSpriteTime = 0;
    std::uint8_t fontWidth = 0, fontHeight = 0;

    bool has(Flag f) const { return (flags & f) != 0; }
    void set(Flag f, bool on) { flags = on ? (flags | f) : (flags & ~f); }
    std::uint32_t blend() const { return (flags >> 4) & 3u; }
    void setBlend(std::uint32_t mode) { flags = (flags & ~BlendMask) | ((mode & 3u) << 4); }
    std::uint32_t anchor() const { return (flags >> 11) & 3u; }

    void initialise();
    // Starts script `index` of the file in `slot` (keeping the position) and runs its first
    // tick. A missing script, or a file that is not loaded, clears the whole VM.
    void start(AnmLibrary& lib, ZunRng& rng, int slot, int index);
    // The same for HUD and menu arrays: position and offset zeroed, font 15x15.
    void startFresh(AnmLibrary& lib, ZunRng& rng, int slot, int index);
    // Initialise, then select sprite `index` of the file in `slot` (no script).
    void initialiseWithSprite(AnmLibrary& lib, int slot, int index);
    // Selects sprite `index` of the owning file; false (nothing changed) when there is none.
    bool selectSprite(int index);
    // One tick. Returns true once the script has finished (now or before).
    bool tick(ZunRng& rng);
    void interrupt(int label) { pendingInterrupt = static_cast<std::int16_t>(label); }
    bool finished() const { return current == nullptr; }
};

// Where and how VMs are drawn this frame: the screen-shake offset and the colour multiplier
// (0x80 = identity per channel), as the original keeps them globally.
struct AnmDrawState {
    float offsetX = 0, offsetY = 0;
    bool multiply = false;
    std::uint32_t multiplyColor = 0x80808080u;
};

// The 2D draw paths. drawVm is "draw" (mode 2): rotated (unrounded) when rotation.z is not
// 0, otherwise rounded to the pixel grid. drawVmByMode dispatches on the VM's draw mode as the
// player's shot and familiar draw does (0 rounded, 1 unrounded, 2 draw, 3 rotated or
// unrounded; 4 and 5 are the stage camera paths and are drawn unrounded here).
// drawVmFacingCamera is the stage path (the unrotated rectangle, unrounded).
void drawVm(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state);
void drawVmByMode(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state);
void drawVmFacingCamera(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, float z);
// The effective colour (alternative colour if selected, times the multiplier).
std::uint32_t effectiveColor(const AnmVm& vm, const AnmDrawState& state);
// Blend modes 2 and 3 set no render state: they draw with the destination blend the last
// mode-0 or mode-1 draw left (as the original's device state). The host's frame start does not
// reset it; screen-effect squares count as mode 0.
void noteAlphaBlendDrawn();
// The blend a VM draws with (spec 4.3): mode 0 alpha, 1 additive, 2 and 3 the last applied.
Blend drawBlend(const AnmVm& vm);

}  // namespace touhou::th08
