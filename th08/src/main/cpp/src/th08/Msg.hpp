#pragma once

// Imperishable Night's dialogue (claude/th08-specs/spec_msg_stage_end.md): the MSG interpreter
// the Gui runs after its HUD update (calc priority 15). Timeline op 6 starts a script; while
// one runs the timeline's op 7 waits (each MSG op 6 lets it through for one frame), the
// player cannot start firing or bomb, and items are collected every frame. Every start also
// cancels the bullets, kills the non-boss enemies and collects the items. Faces and text are
// ANM VMs (face files in slots 16-19, text.anm); the results (op 9) and the stage end of the
// final stages (op 11) are handed to the game.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "th08/Anm.hpp"

namespace touhou {
class SpriteSink;
class TextRenderer;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th08 {

struct ExeTables;
struct ScoreData;
struct World;

class Dialogue {
  public:
    static constexpr int kSlotDecor = 15, kSlotFaceHuman = 16, kSlotFaceYoukai = 17, kSlotFaceBoss = 18, kSlotFaceBoss2 = 19;

    struct Context {
        AnmLibrary* anm = nullptr;
        TextureLibrary* textures = nullptr;
        TextRenderer* text = nullptr;
        const ExeTables* tables = nullptr;
        const ScoreData* scores = nullptr;
    };
    // LoadMsg (spec 2.1): the stage's file for this character; the face files' order reset.
    bool load(const Context& ctx, const Vfs& vfs, int stage, int character, std::string* error);
    void release();
    bool loaded() const { return !file_.empty(); }

    // MsgStart (spec 4).
    void start(const World& w, int n);
    // RunMsg (spec 5) with the game input word (current and previous).
    void run(const World& w, std::uint16_t input, std::uint16_t previous);
    // The MSG layer (spec 8), drawn before the HUD.
    void draw(SpriteSink& sink, const UiArt* ui);

    bool running() const { return index_ >= 0; }
    bool mustWait() const { return index_ >= 0 && resume_ == 0; }
    bool skippable() const { return skippable_ != 0; }
    int script() const { return index_; }
    int choice() const { return choice_; }

    // What the game does for some opcodes.
    std::function<void(int n)> onScriptZero;     // MsgStart(0): the stage's boss label and backgrounds
    std::function<void(int track)> onMusic;      // op 7 (track < 0: stop)
    std::function<void()> onResults;             // op 9
    std::function<void()> onStageEnd;            // op 11 in 6A, 6B and Extra
    std::function<void()> onMusicFade;           // op 12
    std::function<void()> onWhiteFade;           // op 14
    std::function<void(int id)> onSound;

  private:
    enum class End { Advance, Stall, Return, Restart };
    End execute(const World& w, const std::uint8_t* ins, std::uint16_t input, std::uint16_t previous);
    const std::uint8_t* instruction() const;
    AnmVm& vmAt(int t);
    void speakerSwitch(int s);
    void renderLine(const World& w, AnmVm& vm, int script, std::string_view masked, int color);
    void selectFace(int k, int sprite);

    Context ctx_;
    std::vector<std::uint8_t> file_;
    std::vector<std::uint32_t> offsets_;
    int stage_ = 0, character_ = 0;
    std::array<int, 4> faceSlot_{kSlotFaceHuman, kSlotFaceYoukai, kSlotFaceBoss, kSlotFaceBoss2};

    // The interpreter block (spec 3), zeroed by every start.
    int index_ = -1;
    std::uint32_t ip_ = 0;
    Timer timer_;
    std::int32_t waited_ = 0, minimum_ = 0;
    std::array<AnmVm, 4> faces_{};
    std::array<AnmVm, 2> lines_{};
    AnmVm vm6_, vm7_;
    std::array<std::uint32_t, 4> colors_{};
    std::int32_t fontSize_ = 15;
    std::uint32_t resume_ = 0;
    std::uint8_t skippable_ = 0, textSpeaker_ = 0, newBox_ = 0, lineCounter_ = 0, speaker_ = 0xFF, boxVisible_ = 0, choice_ = 0;
};

}  // namespace touhou::th08
