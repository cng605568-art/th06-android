#pragma once

// Perfect Cherry Blossom's ANM animation system: version-2 .anm files (chains of entries
// with embedded THTX textures) loaded into 50 slots and one 2560-entry sprite/script index
// space, and the per-object VM that runs their scripts. In PCB the VM is part of the
// simulation (bullet spawn lengths, shot lifetimes, RNG draws by opcodes 59 and 60), so it
// follows the original's arithmetic exactly: single-precision floats in the stated order,
// timers with sub-frames for the game-speed multiplier, mutable script bytes.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th07 {

// Game-wide timing: the game-speed multiplier (1, or 1/n during Youmu's slow-down) and the
// "force integer" flag set when such a slow-down ends (see Timer).
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
    // One frame: whole frames are reached through the sub-frame while the game is slowed.
    void tick();
    void decrement(std::int32_t v = 1);
    void increment(std::int32_t v = 1);
    float asFloat() const { return static_cast<float>(current) + sub; }
};

// A sprite as the game derives it at load time (pixel rectangle, UVs, the "pixel size"
// that gameplay bounds use).
struct AnmSprite {
    int sourceSlot = -1;  // -1: empty
    std::uint16_t texture = 0;  // engine texture id
    float startX = 0, startY = 0, endX = 0, endY = 0;  // texture pixels
    float textureWidth = 0, textureHeight = 0;
    float uvStartX = 0, uvStartY = 0, uvEndX = 0, uvEndY = 0;
    float pixelWidth = 0, pixelHeight = 0;
    float cols = 1, rows = 1;
};

class AnmLibrary {
  public:
    static constexpr int kSlots = 50;
    static constexpr int kIndices = 2560;

    // Loads `path` into `slot` (and the following slots, one per entry of its chain) with
    // the given base index offset. Re-registers textures with `textures` (bilinear, wrap).
    // With `cpuPixels` the textures are kept CPU-side (dynamic), for files the game edits at
    // run time (front.anm's boss name label).
    bool load(const Vfs& vfs, TextureLibrary& textures, const std::string& path, int slot, int indexOffset,
              std::string* error, bool cpuPixels = false);
    void release(int slot);

    const AnmSprite* sprite(int index) const;  // null when empty or out of range
    const std::uint8_t* script(int index) const;
    int scriptEntryOffset(int index) const;
    const std::uint8_t* bufferEnd(int index) const;  // the end of the file a script lives in
    // The engine texture of a slot (0 when empty), and the slot's entry index offset.
    std::uint16_t slotTexture(int slot) const;
    int slotIndexOffset(int slot) const;
    bool loaded(int slot) const { return slot >= 0 && slot < kSlots && slots_[slot].used; }
    // The file bytes of a slot's chain (script bytes are mutable: save states snapshot them).
    std::vector<std::uint8_t>* slotBytes(int slot);

  private:
    struct Slot {
        bool used = false;
        int childCount = 0;
        std::shared_ptr<std::vector<std::uint8_t>> bytes;
        std::size_t entryOffset = 0;
        int indexOffset = 0;
        std::uint16_t texture = 0;
        std::string name;
    };
    struct ScriptRef {
        const Slot* slot = nullptr;
        std::size_t offset = 0;
        int entryOffset = 0;
    };
    std::array<Slot, kSlots> slots_{};
    std::array<AnmSprite, kIndices> sprites_{};
    std::array<ScriptRef, kIndices> scripts_{};
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
        Additive = 1u << 4,
        UseOffset = 1u << 7,
        FlipX = 1u << 8,
        FlipY = 1u << 9,
        AnchorLeft = 1u << 10,
        AnchorTop = 1u << 11,
        ZWriteDisable = 1u << 12,
        Stopped = 1u << 13,
        CameraMode = 1u << 14,
        SkipTransform = 1u << 15,
        UseAltColor = 1u << 16,
    };
    enum Channel { Position = 0, Color = 1, Alpha = 2, Rotation = 3, Scale = 4 };

    // Base block (cleared by initialise()).
    Vec3f rotation, angularVelocity;
    Vec2f scale{1, 1}, scaleGrowth;
    Vec2f uvScroll;
    Timer clock, waitTimer;
    std::array<Timer, 5> progress{}, duration{};
    std::array<std::uint8_t, 5> ease{};
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

    // Extended block (kept by initialise()).
    Vec3f position;
    std::int16_t activeSprite = -1;
    std::int16_t baseSprite = 0;
    std::int16_t scriptIndex = 0;
    std::uint8_t* scriptStart = nullptr;
    std::uint8_t* current = nullptr;  // null: finished
    const std::uint8_t* scriptEnd = nullptr;
    const AnmSprite* sprite = nullptr;
    Vec3f posStart, posEnd, rotStart, rotEnd;
    Vec2f scaleStart, scaleEnd;
    std::uint32_t colorStart = 0, colorEnd = 0;
    Vec3f offset;
    std::int32_t lastSpriteTime = 0;
    std::uint8_t fontWidth = 0, fontHeight = 0;

    bool has(Flag f) const { return (flags & f) != 0; }
    void set(Flag f, bool on) { flags = on ? (flags | f) : (flags & ~f); }
    std::uint32_t flip() const { return (flags >> 8) & 3u; }
    std::uint32_t anchor() const { return (flags >> 10) & 3u; }

    void initialise();
    // Starts the script with the given index (keeping the position) and runs its first tick.
    void start(AnmLibrary& lib, ZunRng& rng, int index);
    // The same for HUD and menu arrays: position and offset zeroed, font 15x15.
    void startFresh(AnmLibrary& lib, ZunRng& rng, int index);
    bool selectSprite(const AnmLibrary& lib, int index);
    // One tick. Returns true once the script has finished (now or before).
    bool tick(AnmLibrary& lib, ZunRng& rng);
    void interrupt(int label) { pendingInterrupt = static_cast<std::int16_t>(label); }
    bool finished() const { return current == nullptr; }
};

// Where and how VMs are drawn this frame: the screen-shake offset and the stage's colour
// multiplier (0x80 = identity per channel), as the original keeps them globally.
struct AnmDrawState {
    float offsetX = 0, offsetY = 0;
    bool multiply = false;
    std::uint32_t multiplyColor = 0x80808080u;
};

// The 2D draw paths: "draw" (rotated, unrounded) when rotation.z is not 0, otherwise "draw
// without rotation" (rounded to the pixel grid). `facingCamera` is the stage path (the
// unrotated rectangle, unrounded).
void drawVm(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state);
void drawVmFacingCamera(SpriteSink& sink, const AnmVm& vm, const AnmDrawState& state, float z);
// The effective colour (alternative colour if selected, times the multiplier).
std::uint32_t effectiveColor(const AnmVm& vm, const AnmDrawState& state);

// Decodes a THTX pixel block (formats 1-5) into RGBA8. False on an unknown format or short data.
bool decodeThtx(std::span<const std::uint8_t> pixels, int format, int width, int height, std::vector<std::uint8_t>& rgba);

}  // namespace touhou::th07
