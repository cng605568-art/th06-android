#pragma once

// Perfect Cherry Blossom's GUI and ASCII manager: the side panel, score popups, the cherry
// gauge, timed messages, the boss HUD and markers, spell card banners, the stage text,
// dialogue (msgN.dat) and the stage-clear tally. Logic priorities 1 (ASCII) and 13 (GUI),
// draw orders 11, 12 and 16.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gfx/SpriteSink.hpp"
#include "th07/Anm.hpp"
#include "th07/World.hpp"

namespace touhou {
class TextRenderer;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th07 {

class Gui final : public StageGui, public PopupSink {
  public:
    struct Resources {
        const Vfs* vfs = nullptr;
        TextureLibrary* textures = nullptr;
        TextRenderer* text = nullptr;
        const UiArt* ui = nullptr;
    };

    // Stage start: first stage of a game (front, faces, loading) or a stage change.
    bool stageStart(const World& w, const Resources& res, bool firstStage, std::string* error);
    void stageEnd(const World& w);
    void gameEnd(const World& w);

    void asciiUpdate(const World& w);  // logic order 1
    void update(const World& w, std::uint16_t input, std::uint16_t previousInput);  // logic order 13
    void drawPopups(SpriteSink& sink);  // draw order 11 (popups and the cherry gauge)
    void draw(SpriteSink& sink, AnmDrawState& state);  // draw order 12
    void drawText(SpriteSink& sink);   // draw order 16 (text queue, boss markers)

    // The stage ended: "go to the next stage" (stages 1-5) or the end of the game.
    bool nextStageRequested() const { return nextStage_; }
    void clearNextStageRequest() { nextStage_ = false; }
    bool gameFinished() const { return gameFinished_; }
    bool stageFinished() const { return stageClear_.active && stageClear_.vm.has(AnmVm::Stopped); }

    // StageGui.
    void startDialogue(int number) override;
    bool dialogueCurrent() const override { return msgIndex_ >= 0 || msgIndex_ == -2; }
    bool dialogueWait() const override { return msgIndex_ >= 0 && resume_ == 0; }
    void spellCardStart(int card, int portrait, const std::string& name) override;
    void spellCardEnd() override;
    void bombBanner(int sprite, const std::string& name) override;
    void bombBannerEnd() override;
    void bonus(std::uint32_t points) override;
    void spellCardBonus(std::uint32_t points) override;
    // PopupSink.
    void status(Status s, std::uint32_t value) override;
    void score(const Vec3f& pos, std::uint32_t value, std::uint32_t color, bool small) override;
    void powerUp(const Vec3f& pos) override;

    // Text queue (ASCII sprites), drawn at order 16.
    void queueText(float x, float y, const std::string& text, std::uint32_t color, float scaleX = 1.0f, float scaleY = 1.0f,
                   float spacing = 14.0f, bool playfield = false);

  private:
    struct Popup {
        bool live = false;
        Vec3f pos;
        std::array<std::uint8_t, 8> digits{};
        int count = 0;
        std::uint32_t color = 0;
        Timer timer;
    };
    struct TextItem {
        float x, y;
        std::string text;
        std::uint32_t color;
        float sx, sy, spacing;
        bool playfield;
    };
    struct Timed {
        bool shown = false;
        float x = 0, y = 0;
        std::uint32_t value = 0;
        int type = 0;
        Timer timer;
    };
    struct Element {  // a GUI VM with an optional draw offset
        AnmVm vm;
        float offsetX = 0, offsetY = 0;
        bool active = false;
    };
    void startVm(AnmVm& vm, int script);
    void renderText(AnmVm& vm, const std::string& sjis, std::uint32_t colorRef, bool rightAligned);
    void runDialogue(const World& w, std::uint16_t input, std::uint16_t previousInput);
    void bossHudUpdate();
    void drawSidePanel(SpriteSink& sink, const AnmDrawState& state);
    void drawBossHud(SpriteSink& sink, const AnmDrawState& state);
    void drawDialogue(SpriteSink& sink, const AnmDrawState& state);
    void drawDigits(SpriteSink& sink, AnmVm& vm, float x, float y, std::int64_t value, int digits, std::int64_t divisor,
                    float step, const AnmDrawState& state);
    void rect(SpriteSink& sink, float x0, float y0, float x1, float y1, std::uint32_t argb);
    void computeStageBonus(const World& w);
    void queueTally(const World& w);

    const World* world_ = nullptr;
    Resources res_;
    int stage_ = 0;
    bool frontLoaded_ = false;
    std::uint32_t frame_ = 0;

    std::array<AnmVm, 33> front_{};
    std::array<AnmVm, 5> stageText_{};
    std::array<bool, 5> stageTextUsed_{};
    Element bombPortrait_, bombDecorL_, bombDecorR_, bombName_, enemyPortrait_, enemyDecorU_, enemyDecorD_, enemyName_;
    AnmVm bombNameBg_, enemyNameBg_, captureIndicator_, digitVm_;
    Element stageClear_;
    AnmVm loadingVm_;
    bool showLoading_ = false;

    // ASCII manager.
    std::vector<TextItem> text_;
    std::array<Popup, 723> popups_{};
    int popupCursor_ = 0;
    AnmVm gaugeVm_, cherryDigitVm_, borderVm_;
    std::array<AnmVm, 4> markerVms_{};
    int gaugeState_ = 0;  // 0 in, 1 out
    AnmVm glyphVm_;

    // Timed messages: bonus, status, spell card bonus.
    Timed bonus_, status_, spellBonus_;

    // Boss HUD.
    int bossState_ = 0;
    std::uint32_t bossAlpha_ = 0;
    float bossFill_ = 0;
    std::int32_t lastSeconds_ = -1;

    // Dialogue.
    std::vector<std::uint8_t> msg_;
    std::vector<std::uint32_t> msgScripts_;
    int msgIndex_ = -1;
    std::size_t msgPos_ = 0;
    Timer msgTimer_;
    std::int32_t waited_ = 0;
    std::int32_t resume_ = 0;
    bool skippable_ = true;
    std::array<Element, 2> portraits_{};
    std::array<AnmVm, 2> lines_{}, introLines_{};
    bool line1Used_ = false;

    // Stage clear.
    int clearState_ = 0;  // 0 playing, 1 results recorded, 2 bonus given
    std::uint32_t clearPoints_ = 0, clearGraze_ = 0;
    std::int32_t clearCherryMax_ = 0;
    std::uint32_t clearBonus_ = 0;
    bool nextStage_ = false;
    bool gameFinished_ = false;
};

}  // namespace touhou::th07
