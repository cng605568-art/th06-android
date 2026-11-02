#pragma once

// ANM animation engine of the 2002 game: a global sprite table and script table shared by
// every loaded .anm file (each file registers at a fixed index offset), and a small VM
// per animated object that runs one script per tick.
//
// Instruction layout (little-endian, packed):  i16 time, u8 opcode, u8 argBytes, args...
// A VM executes every instruction whose time is <= its script clock, then advances the
// clock by one tick. Opcodes 0..31 are implemented as in the original engine (semantics
// from the th06 decompilation, CC0); 32+ do not occur in this game and are ignored.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/ZunMath.hpp"
#include "gfx/Anm.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"

namespace th06 {

class Vfs;

namespace anm_op {
enum : std::uint8_t {
    Delete = 0,
    SetSprite = 1,
    Scale = 2,
    Alpha = 3,
    Color = 4,
    Jump = 5,
    Nop = 6,
    ScaleFlipX = 7,
    ScaleFlipY = 8,
    Rotation = 9,
    RotationSpeed = 10,
    ScaleSpeed = 11,
    AlphaInterpLinear = 12,
    BlendAdditive = 13,
    BlendNormal = 14,
    Static = 15,
    SpriteSetRandom = 16,
    MovePosition = 17,
    MovePositionInterpLinear = 18,
    MovePositionInterpDecelerateSlow = 19,
    MovePositionInterpDecelerateFast = 20,
    Halt = 21,
    InterruptLabel = 22,
    AnchorTopLeft = 23,
    HaltInvisible = 24,
    PositionMode = 25,
    SetAutoRotate = 26,
    ScrollX = 27,
    ScrollY = 28,
    FlagVisible = 29,
    ScaleInterpLinear = 30,
    FlagDisableZWrite = 31,
};
}  // namespace anm_op

enum class AnmAnchor : std::uint8_t { Center = 0, Left = 1, Top = 2, TopLeft = 3 };
enum class AnmInterp : std::uint8_t { Linear = 0, DecelerateSlow = 1, DecelerateFast = 2 };

// One loaded sprite: a rectangle of a texture (sizes in texture pixels).
struct AnmLoadedSprite {
    std::int16_t sourceFile = -1;  // ANM file slot; -1 = empty entry
    std::uint16_t texture = 0;     // TextureLibrary id
    float startX = 0, startY = 0, endX = 0, endY = 0;
    float textureWidth = 1, textureHeight = 1;  // from the ANM header (UV normalisation)
    float widthPx = 0, heightPx = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

// Per-object animation state. Plain data: copying a VM (as bullet templates are copied
// into bullets) copies the animation exactly.
struct AnmVm {
    Vec3f rotation;
    Vec3f angleVel;
    float scaleY = 1.0f;
    float scaleX = 1.0f;
    float scaleInterpFinalY = 0.0f;
    float scaleInterpFinalX = 0.0f;
    float uvScrollX = 0.0f;
    float uvScrollY = 0.0f;
    std::int32_t timeInScript = 0;
    std::uint32_t color = 0xffffffffu;  // D3DCOLOR (ARGB)

    // flags
    bool visible = false;
    bool visibleOverride = false;  // engine-side visibility override
    bool additive = false;         // blend mode
    bool colorAdd = false;         // colour op (false = modulate)
    bool usePosOffset = false;
    bool zWriteDisable = false;
    bool stopped = false;
    std::uint8_t flip = 0;  // bit 0 = mirrored in X, bit 1 = mirrored in Y
    AnmAnchor anchor = AnmAnchor::Center;
    AnmInterp moveInterpMode = AnmInterp::Linear;

    std::int16_t alphaInterpEndTime = 0;
    std::int16_t scaleInterpEndTime = 0;
    std::int16_t autoRotate = 0;
    std::int16_t pendingInterrupt = 0;
    std::int16_t posInterpEndTime = 0;

    Vec3f pos;
    float scaleInterpInitialY = 0.0f;
    float scaleInterpInitialX = 0.0f;
    std::int32_t scaleInterpTime = 0;
    std::int16_t activeSpriteIndex = -1;
    std::int16_t baseSpriteIndex = 0;
    std::int16_t anmFileIndex = -1;  // script table index this VM was started from

    const std::uint8_t* scriptBegin = nullptr;
    const std::uint8_t* currentInstruction = nullptr;  // null = script finished
    const std::uint8_t* scriptEnd = nullptr;           // end of the owning file's bytes

    std::uint32_t alphaInterpInitial = 0;
    std::uint32_t alphaInterpFinal = 0;
    Vec3f posInterpInitial;
    Vec3f posInterpFinal;
    Vec3f posOffset;
    std::int32_t posInterpTime = 0;
    std::int32_t timeOfLastSpriteSet = 0;
    std::int32_t alphaInterpTime = 0;

    void initialize();
};

class AnmManager {
  public:
    static constexpr int kMaxSprites = 4096;
    static constexpr int kMaxScripts = 4096;
    static constexpr int kMaxFiles = 64;

    AnmManager(TextureLibrary& textures, ZunRng& rng);

    // LoadAnm: registers the file's sprites at (sprite ID + offset) and its scripts at
    // (script ID + offset). Several files may share an offset (stgNenm2 does) when their
    // IDs do not overlap.
    bool loadAnm(const Vfs& vfs, int fileSlot, std::string_view path, int offset, std::string* error);
    bool loadAnmFromBytes(std::vector<std::uint8_t> bytes, std::uint16_t texture, int fileSlot,
                          int offset, std::string* error);
    // ReleaseAnm: forgets the slot's sprites and scripts (loading into an occupied slot
    // does this first). VMs still running one of its scripts must be reset by the caller.
    void releaseAnm(int fileSlot);
    bool fileLoaded(int fileSlot) const;
    // Goes up whenever a file is loaded or released (animations from before point into
    // files that may be gone: a practice save state is only good while it stays put).
    std::uint64_t generation() const { return generation_; }
    // TextureLibrary id of a loaded file's texture, -1 if the slot is empty.
    int fileTexture(int fileSlot) const {
        if (fileSlot < 0 || fileSlot >= kMaxFiles || !files_[static_cast<std::size_t>(fileSlot)].loaded) return -1;
        return files_[static_cast<std::size_t>(fileSlot)].texture;
    }
    bool hasScript(int scriptIndex) const;
    const AnmLoadedSprite* sprite(int spriteIndex) const;
    const AnmLoadedSprite* sprite(const AnmVm& vm) const { return sprite(vm.activeSpriteIndex); }
    std::size_t loadedSpriteCount() const;

    bool setActiveSprite(AnmVm& vm, int spriteIndex) const;
    void initializeAndSetSprite(AnmVm& vm, int spriteIndex) const;
    void setAndExecuteScript(AnmVm& vm, int scriptIndex);  // SetAndExecuteScriptIdx

    // Runs one tick. Returns 1 when the script has finished (or never started), else 0.
    int executeScript(AnmVm& vm);

    // Drawing. A frame must start with beginFrame() (it resets the render-state cache the
    // original keeps between draws, which decides when UV scrolling is picked up).
    void beginFrame();
    // Draw2 / Draw3: an object in the playfield (vm.pos in field pixels). Unrotated sprites
    // are snapped to whole pixels like Draw2 unless `roundPosition` is false (the 3D path
    // effects use).
    void drawWorld(SpriteSink& sink, const AnmVm& vm, float screenX, float screenY, bool roundPosition = true);
    // Draw / DrawNoRotation: a screen-space sprite (vm.pos in screen pixels).
    void drawScreen(SpriteSink& sink, const AnmVm& vm);

  private:
    struct ScriptRef {
        const std::uint8_t* begin = nullptr;
        const std::uint8_t* end = nullptr;
        int spriteOffset = 0;
        int fileSlot = -1;
    };
    struct FileSlot {
        std::vector<std::uint8_t> bytes;
        bool loaded = false;
        std::uint16_t texture = 0;
    };

    bool uvFor(const AnmVm& vm, const AnmLoadedSprite& s, float& u0, float& v0, float& u1, float& v1);

    TextureLibrary& textures_;
    ZunRng& rng_;
    std::vector<AnmLoadedSprite> sprites_;
    std::vector<ScriptRef> scripts_;
    std::array<FileSlot, kMaxFiles> files_;

    // DrawInner only refreshes texture coordinates when the sprite differs from the
    // previously drawn one; reproduced so uvScroll behaves as in the original.
    int lastSprite_ = -1;
    float lastU0_ = 0, lastV0_ = 0, lastU1_ = 0, lastV1_ = 0;
    int runawayWarnings_ = 0;
    std::uint64_t generation_ = 0;
};

}  // namespace th06
