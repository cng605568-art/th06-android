#pragma once

// The in-game GUI, drawn from the game's own files: the side panel (front.anm frame,
// labels, life and bomb stars, power bar, scores in ascii.anm's font), the boss HUD (label,
// life bar, life count, spell timer with its countdown tick), spell-card and bomb portraits
// and names (faceNNx.anm, text.anm with run-time rendered text), the sliding BONUS / Full
// Power Mode!! / Spell Card Bonus! messages, score popups, and the stage-start banner.
//
// Dialogue (msgN.dat) runs in the GUI too: portraits from the face files, lines rendered
// into text.anm, the box, and the opcodes that drive the stage's end (results tally, next
// stage).
//
// Update order follows the original: popups move in the ASCII pass (before everything),
// the GUI itself updates after the bullets. Drawing: popups after bullets, then the GUI,
// then all queued ASCII text on top.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/ZunMath.hpp"
#include "game/Input.hpp"
#include "game/World.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/TextRenderer.hpp"
#include "gfx/TextureLibrary.hpp"

namespace th06 {

class Vfs;

// GUI ANM file slots and offsets (as in the original).
inline constexpr int kAnmFileText = 0, kAnmOffsetText = 0x700;
inline constexpr int kAnmFileAscii = 1, kAnmOffsetAscii = 0x000;
inline constexpr int kAnmFileFront = 13, kAnmOffsetFront = 0x600;
inline constexpr int kAnmFileLoading = 14, kAnmOffsetLoading = 0x619;
inline constexpr int kAnmFileFaceCharaA = 15, kAnmOffsetFaceCharaA = 0x4a0;
inline constexpr int kAnmFileFaceCharaB = 16, kAnmOffsetFaceCharaB = 0x4a2;
inline constexpr int kAnmFileFaceCharaC = 17, kAnmOffsetFaceCharaC = 0x4a4;
inline constexpr int kAnmFileFaceStageA = 18, kAnmOffsetFaceStageA = 0x4a8;
inline constexpr int kAnmFileFaceStageB = 19, kAnmOffsetFaceStageB = 0x4aa;
inline constexpr int kAnmFileFaceStageC = 20, kAnmOffsetFaceStageC = 0x4ab;

class Gui {
  public:
    static constexpr int kFrontSlots = 26;
    static constexpr int kPopups = 512;
    static constexpr int kSmallPopups = 3;
    static constexpr int kMaxStrings = 256;

    enum class BossHud : std::uint8_t { Idle, Entering, Shown, Leaving };
    enum class Align : std::uint8_t { Left, Right, Centre };

    struct Popup {
        Vec3f pos;
        std::uint32_t color = 0xffffffffu;  // ARGB
        std::array<char, 9> digits{};       // decimal digits, or empty for the special glyph
        std::uint8_t count = 0;             // characters (1 for the special glyph)
        bool special = false;
        bool inUse = false;
        std::int32_t timer = 0;
    };
    struct Slide {
        bool shown = false;
        std::int32_t timer = 0;
        std::int32_t value = 0;
        float x = 0.0f;
    };
    struct QueuedString {
        float x = 0, y = 0;
        std::string text;
        std::uint32_t color = 0xffffffffu;  // ARGB
        float scale = 1.0f;
        bool clipped = false;
    };

    Gui(TextureLibrary& textures, TextRenderer* text);

    // Loads ascii/text/front/loading and the character's faces (once per run or when the
    // character changes). Without front.anm, ascii.anm and text.anm the GUI is not ready
    // and the stage draws the port's own diagnostic HUD instead.
    bool loadRun(World& world, const Vfs& vfs, int character);
    // Per stage: stage faces, the stage title and first song name (from stageN.std, raw
    // Shift-JIS) and the resets of a stage start.
    void startStage(World& world, const Vfs& vfs, int stage, bool firstStage);
    bool ready() const { return ready_; }

    void updatePopups();           // ASCII pass (before the game manager)
    void update(World& world, const InputState& input = {});  // GUI pass (after the bullets)
    void drawPopups(World& world, SpriteSink& sink);
    void draw(World& world, SpriteSink& sink, const SpriteRegion& white, float fps);
    void drawStrings(World& world, SpriteSink& sink);  // the queued ASCII text, last

    // Events from the game.
    void popup(const Vec3f& fieldPos, std::int32_t value, std::uint32_t argb, bool small = false);
    void showBomb(World& world, int shotType);
    void endBombName();
    void spellStart(World& world, int portrait, std::span<const std::uint8_t> name);
    void spellNameExit();
    void showBonus(std::int32_t total);
    void showFullPower();
    void showSpellBonus(std::int32_t value);
    void queueString(float x, float y, std::string_view text, std::uint32_t argb, float scale = 1.0f,
                     bool clipped = false);

    // Dialogue. msgRead starts script character * 10 + n (the timeline's "read dialogue").
    void msgRead(World& world, int n);
    bool msgActive() const { return msg_.active; }
    bool msgBlocksTimeline() const { return msg_.active && msg_.resume == 0; }
    bool hasDialogue() const { return !msg_.offsets.empty(); }
    // Set by the dialogue's stage-end opcode; the stage loads the next one and clears it.
    bool takeStageEndRequest() {
        const bool r = stageEndRequested_;
        stageEndRequested_ = false;
        return r;
    }
    // Set by the stage-end opcode of the last stage (6, Easy's 5, Extra): the game is cleared.
    bool takeGameClearRequest() {
        const bool r = gameClearRequested_;
        gameClearRequested_ = false;
        return r;
    }
    bool tallyShown() const { return tallyOn_; }
    // The loading screen has stopped on its last frame: the stage background is not drawn.
    bool loadingHalted() const { return loadingActive_ && loading_.visible && loading_.stopped; }
    std::int32_t tallyTotal() const { return tallyTotal_; }
    int msgScript() const { return msg_.script; }
    // Song k's path from the stage file ("bgm/th06_02.mid"), empty if none.
    std::string songPath(int k) const { return k >= 0 && k < 4 ? songPaths_[static_cast<std::size_t>(k)] : std::string(); }
    int msgTimer() const { return msg_.timer; }

    // Inspection (tests, HUD).
    BossHud bossHudState() const { return bossState_; }
    std::uint32_t bossHudOpacity() const { return bossOpacity_; }
    float bossFill() const { return bossFill_; }
    const Popup& popupAt(int i) const { return popups_[static_cast<std::size_t>(i)]; }
    const Popup& smallPopupAt(int i) const { return smallPopups_[static_cast<std::size_t>(i)]; }
    int activePopups() const;
    const Slide& bonusMessage() const { return bonus_; }
    const Slide& fullPowerMessage() const { return fullPower_; }
    const Slide& spellBonusMessage() const { return spellBonus_; }
    const std::vector<QueuedString>& strings() const { return strings_; }
    float spellNameLength() const { return spellNameLength_; }
    float bombNameLength() const { return bombNameLength_; }
    const AnmVm& bombName() const { return bombName_; }
    const AnmVm& spellName() const { return spellName_; }
    std::uint32_t hiScore() const { return hiScore_; }
    void setHiScore(std::uint32_t v) { hiScore_ = v; }  // from score.dat at the start of a game
    // Renders text into the element's texture band (exposed for tests).
    bool renderText(World& world, AnmVm& element, std::span<const std::uint8_t> sjis, int fontSize,
                    std::uint32_t rgb, Align align);

  private:
    void updateBossHud(World& world);
    void updateSlide(Slide& s, int limit);
    void drawPanel(World& world, SpriteSink& sink, const SpriteRegion& white, float fps);
    void drawBossHud(World& world, SpriteSink& sink);
    void drawNameWithBar(World& world, SpriteSink& sink, AnmVm& name, AnmVm& bar, float barDx, float length);
    void stopElement(AnmVm& vm);
    void msgStep(World& world, const InputState& input);
    void drawDialogue(World& world, SpriteSink& sink, const SpriteRegion& white);
    void drawTally(World& world);

    struct Msg {
        std::vector<std::uint8_t> file;
        std::vector<std::uint32_t> offsets;
        bool active = false;
        int script = -1;
        std::uint32_t pc = 0;  // offset of the current instruction in file
        std::int32_t timer = 0;
        std::int32_t framesWaited = 0;
        std::int32_t resume = 0;
        bool skippable = true;
        bool halted = false;
        bool line1Used = false;
        int fontSize = 15;
        std::array<AnmVm, 2> portraits{};
        std::array<AnmVm, 2> lines{};
        std::array<AnmVm, 2> intro{};
    };

    TextureLibrary& textures_;
    TextRenderer* text_;
    bool ready_ = false;
    int character_ = -1;
    int stage_ = 1;

    std::array<AnmVm, kFrontSlots> front_{};
    AnmVm stageName_, songName_, bombPortrait_, bombName_, spellPortrait_, spellName_, bombBar_, spellBar_, loading_;
    bool loadingActive_ = false;
    float bombNameLength_ = 0.0f, spellNameLength_ = 0.0f;

    BossHud bossState_ = BossHud::Idle;
    std::uint32_t bossOpacity_ = 0;
    float bossFill_ = 0.0f;
    std::int32_t lastTimerSeen_ = -1;

    Slide bonus_, fullPower_, spellBonus_;
    std::array<Popup, kPopups> popups_{};
    std::array<Popup, kSmallPopups> smallPopups_{};
    int nextPopup_ = 0, nextSmallPopup_ = 0;
    std::vector<QueuedString> strings_;
    std::uint32_t hiScore_ = 0;
    const Vfs* vfs_ = nullptr;
    std::array<std::vector<std::uint8_t>, 4> songs_{};
    std::array<std::string, 4> songPaths_{};
    Msg msg_;
    bool previousShoot_ = false;
    bool tallyOn_ = false, tallyPending_ = false;
    std::int32_t tallyTotal_ = 0;
    bool stageEndRequested_ = false;
    bool gameClearRequested_ = false;
};

}  // namespace th06
