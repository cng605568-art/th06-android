#include "th07/Th07Game.hpp"

#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"

namespace touhou::th07 {

const GameInfo& Th07Game::info() const {
    static const GameInfo kInfo{
        .id = "th07",
        .windowTitle = "TH07 Port",
        .prefOrg = "th07",
        .prefApp = "th07-android",
        .envPrefix = "TH07",
    };
    return kInfo;
}

bool Th07Game::start(const GameContext& ctx) {
    ui_ = ctx.ui;
    files_ = ctx.vfs ? ctx.vfs->fileCount() : 0;
    log::info("Perfect Cherry Blossom: {} file(s) in the game folder; the game is not ported yet", files_);
    return true;
}

void Th07Game::tick(const InputState& input) {
    ++frame_;
    if (input.quit) quit_ = true;
}

void Th07Game::render(SpriteSink& sink, float, const FrameInfo&) {
    if (!ui_) return;
    const std::uint32_t pink = rgba(255, 183, 197), white = rgba(255, 255, 255), grey = rgba(170, 170, 170);
    drawText(sink, *ui_, 112, 168, "PERFECT CHERRY BLOSSOM", 3.0f, pink);
    drawText(sink, *ui_, 176, 232, "BEING PORTED", 3.0f, white);
    drawText(sink, *ui_, 176, 296, std::format("{} FILES FOUND", files_), 2.0f, grey);
}

void Th07Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | placeholder screen", frame.fps, frame.ticks);
}

}  // namespace touhou::th07
