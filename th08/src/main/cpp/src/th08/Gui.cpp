#include "th08/Gui.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/ProceduralArt.hpp"
#include "io/Vfs.hpp"

namespace touhou::th08 {

namespace {

const char* stageTextAnm(int stage) {
    static const char* const kNames[9] = {"stg1txt.anm", "stg2txt.anm", "stg3txt.anm", "stg4atxt.anm", "stg4btxt.anm",
                                          "stg5txt.anm", "stg6txt.anm", "stg7txt.anm", "stg8txt.anm"};
    return kNames[std::clamp(stage, 0, 8)];
}

// The loading screen by character (table 0x4C72C4): the teams' own, then the solo human
// ("h") or youkai ("a") of each team.
std::string loadingAnm(int character) {
    if (character < 4) return std::format("loading0{}.anm", std::max(0, character));
    const int solo = std::min(character, 11) - 4;
    return std::format("loading0{}{}.anm", solo / 2, solo % 2 == 0 ? 'h' : 'a');
}

std::uint32_t withAlpha(std::uint32_t argb, std::uint32_t alpha) { return (argb & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24); }

}  // namespace

bool Gui::stageStart(const FlowContext& ctx, const GameState& g, bool firstStage, std::string* error) {
    if (!ctx.vfs || !ctx.anm || !ctx.textures || !ctx.rng) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx.anm;
    ZunRng& rng = *ctx.rng;
    anm_ = &lib;
    ui_ = ctx.ui;
    spellPractice_ = g.spellPractice;
    if (firstStage) {
        if (!lib.load(*ctx.vfs, *ctx.textures, "front.anm", kSlotFront, error)) return false;
        if (!lib.load(*ctx.vfs, *ctx.textures, "times.anm", kSlotTimes, error)) return false;
        if (!lib.load(*ctx.vfs, *ctx.textures, loadingAnm(g.character), kSlotLoading, error)) return false;
        lastSpellMarker_ = AnmVm{};
        lastSpellMarker_.start(lib, rng, kSlotAscii, 26);
        difficultyLabel_ = AnmVm{};
        difficultyLabel_.start(lib, rng, kSlotAscii, 25);
        difficultyLabel_.selectSprite(283 + std::clamp(g.difficulty, 0, 4));
    }
    loaded_ = true;
    // The clocks: the in-stage one starts hidden (ECL starts it); the title one shows the night.
    stageClock_ = AnmVm{};
    stageClock_.color &= 0x00FFFFFFu;
    titleClock_ = AnmVm{};
    titleClock_.startFresh(lib, rng, kSlotTimes, 0);
    titleClock_.selectSprite(g.nightSteps);
    if (!lib.load(*ctx.vfs, *ctx.textures, stageTextAnm(g.stage), kSlotStageText, error)) return false;
    if (firstStage) {
        for (int i = 0; i < kFrontVms; ++i) {
            front_[static_cast<std::size_t>(i)] = AnmVm{};
            front_[static_cast<std::size_t>(i)].start(lib, rng, kSlotFront, i);
        }
    }
    frame_ = 0;
    const AnmFile* text = lib.file(kSlotStageText);
    for (int i = 0; i < 4; ++i) {
        const auto k = static_cast<std::size_t>(i);
        stageText_[k] = AnmVm{};
        hasStageText_[k] = !g.spellPractice && text && text->script(i) != nullptr;
        if (!hasStageText_[k]) continue;
        stageText_[k].start(lib, rng, kSlotStageText, i);
        stageText_[k].baseSprite = stageText_[k].activeSprite;
    }
    nightCaption_ = AnmVm{};
    nightCaption_.start(lib, rng, kSlotAscii, 3);
    refresh_.fill(2);
    fullRedraw_ = 16;
    return true;
}

void Gui::release(AnmLibrary& anm) {
    if (!loaded_) return;
    for (int slot : {kSlotFront, kSlotTimes, kSlotLoading, kSlotStageText}) anm.release(slot);
    loaded_ = false;
}

void Gui::update(ZunRng& rng, const GameState& g, const Vec3f& playerPos, bool frozen) {
    (void)g;
    if (!loaded_ || frozen) return;
    for (AnmVm& vm : front_) vm.tick(rng);
    for (int i = 0; i < 4; ++i) {
        if (hasStageText_[static_cast<std::size_t>(i)]) stageText_[static_cast<std::size_t>(i)].tick(rng);
    }
    if (!spellPractice_ && hasStageText_[0] && (stageText_[0].color >> 24) != 0) titleClock_.tick(rng);
    nightCaption_.tick(rng);
    stageClock_.tick(rng);
    // The in-stage clock fades to 64 while the player is under it.
    std::uint32_t a = stageClock_.color >> 24;
    if (playerPos.x >= 64.0f && playerPos.y < 128.0f) {
        if (a > 64) a = a - 4 < 64 ? 64 : a - 4;
    } else {
        a = std::min<std::uint32_t>(255, a + 4);
    }
    stageClock_.color = withAlpha(stageClock_.color, a);
    lastSpellMarker_.tick(rng);
    difficultyLabel_.tick(rng);
    ++frame_;
}

void Gui::draw(SpriteSink& sink, const GameState& g, AsciiText& ascii) {
    if (!loaded_) return;
    const AnmDrawState none;
    for (int i = 0; i < 4; ++i) {
        if (hasStageText_[static_cast<std::size_t>(i)]) drawVm(sink, stageText_[static_cast<std::size_t>(i)], none);
    }
    drawVm(sink, titleClock_, none);
    drawVm(sink, stageClock_, none);
    drawVm(sink, nightCaption_, none);
    drawPanel(sink, g, ascii);
}

// The side panel (spec 3.4). The original redraws its parts only when they change, over what
// the previous frames left; the port draws every part every frame in the same order.
void Gui::drawPanel(SpriteSink& sink, const GameState& g, AsciiText& ascii) {
    const AnmDrawState none;
    AnmVm& tile = front_[13];
    for (float y = 0; y <= 448.0f; y += 32.0f) {
        tile.position = {0.0f, y, 0.49f};
        drawVm(sink, tile, none);
    }
    for (float x = 416.0f; x <= 608.0f; x += 32.0f) {
        for (float y = 16.0f; y <= 448.0f; y += 32.0f) {
            tile.position = {x, y, 0.49f};
            drawVm(sink, tile, none);
        }
    }
    AnmVm& strip = front_[14];
    for (float x = 0; x <= 512.0f; x += 128.0f) {
        for (float y : {0.0f, 464.0f}) {
            strip.position = {x, y, 0.49f};
            drawVm(sink, strip, none);
        }
    }
    for (int k = 0; k <= 9; ++k) drawVm(sink, front_[static_cast<std::size_t>(k)], none);
    drawVm(sink, difficultyLabel_, none);
    // The row erasers (VM 15) under every value.
    AnmVm& eraser = front_[15];
    for (float y : {40.0f, 56.0f, 88.0f, 104.0f, 136.0f, 152.0f, 168.0f, 184.0f}) {
        eraser.position = {480.0f, y, y < 88.0f ? 0.49f : 0.48f};
        drawVm(sink, eraser, none);
    }
    eraser.position = {512.0f, 464.0f, 0.49f};
    drawVm(sink, eraser, none);
    for (int i = 0; i < g.livesI(); ++i) {
        front_[10].position = {488.0f + 16.0f * static_cast<float>(i), 88.0f, 0.46f};
        drawVm(sink, front_[10], none);
    }
    for (int i = 0; i < g.bombsI(); ++i) {
        front_[11].position = {488.0f + 16.0f * static_cast<float>(i), 104.0f, 0.46f};
        drawVm(sink, front_[11], none);
    }
    for (float x : {32.0f, 160.0f, 288.0f}) {
        strip.position = {x, 464.0f, 0.49f};
        drawVm(sink, strip, none);
    }
    ascii.add(488.0f, 56.0f, std::format("{:09d}", g.guiScore));
    ascii.add(488.0f + 117.0f, 56.0f, std::format("{}", std::min(g.continues, 9)));
    ascii.add(488.0f, 40.0f, std::format("{:09d}", g.hiScore));
    ascii.add(488.0f + 117.0f, 40.0f, std::format("{}", std::min(g.hiScoreContinues, 9)));
    ascii.add(488.0f, 152.0f, std::format("{}", g.graze));
    auto ratio = [&](float y, std::int32_t a, std::int32_t b, std::uint32_t color) {
        const std::string first = std::format("{}", a);
        float x = 488.0f;
        ascii.add(x, y, first, color);
        x += 13.0f * static_cast<float>(first.size());
        ascii.add(x, y, "/", color, 1.0f, 13.0f, 0.5f);
        x += 6.0f;
        ascii.add(x, y, std::format("{}", b), color);
    };
    ratio(168.0f, g.pointItems, g.nextExtend, 0xFFFFFFFFu);
    ratio(184.0f, g.timePoints, g.timeQuota, g.timePoints >= g.timeQuota ? 0xFFFFF0C0u : 0xFFFFFFFFu);
    const int power = g.powerI();
    if (power > 0 && ui_) {
        // The gradient bar: left 0xE0E0E0FF, right 0x80E0E0FF, in 4-pixel steps.
        for (int x = 0; x < power; x += 4) {
            const int w = std::min(4, power - x);
            const float t = static_cast<float>(x) / 128.0f;
            const auto alpha = static_cast<std::uint8_t>(0xE0 + (0x80 - 0xE0) * t);
            sink.fillRect(ui_->white, 488.0f + static_cast<float>(x), 136.0f, static_cast<float>(w), 16.0f, rgba(0xE0, 0xE0, 0xFF, alpha));
        }
    }
    ascii.add(488.0f, 136.0f, power < 128 ? std::format("{}", power) : std::string("MAX"));
    for (int& r : refresh_) r = r > 0 ? r - 1 : 0;
    if (fullRedraw_ > 0) --fullRedraw_;
}

}  // namespace touhou::th08
