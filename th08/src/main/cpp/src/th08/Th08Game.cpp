#include "th08/Th08Game.hpp"

#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"

namespace touhou::th08 {

const GameInfo& Th08Game::info() const {
    static const GameInfo kInfo{
        .id = "th08",
        .windowTitle = "TH08 Port",
        .prefOrg = "th08",
        .prefApp = "th08-android",
        .envPrefix = "TH08",
    };
    return kInfo;
}

bool Th08Game::start(const GameContext& ctx) {
    ui_ = ctx.ui;
    if (ctx.vfs) folder_ = inspectFolder(*ctx.vfs);
    log::info("th08.dat: {}; th08.exe 1.00d: {}; thbgm.dat: {}; {} files", folder_.archive ? "mounted" : "missing",
              folder_.exe ? "found" : "missing", folder_.thbgm ? "found" : "missing", folder_.files);
    return true;
}

void Th08Game::tick(const InputState&) { ++ticks_; }

void Th08Game::render(SpriteSink& sink, float, const FrameInfo&) {
    if (!ui_) return;
    const std::uint32_t moon = rgba(250, 226, 150), white = rgba(255, 255, 255), grey = rgba(170, 170, 170);
    const std::uint32_t good = rgba(140, 230, 140), bad = rgba(255, 140, 140);
    drawText(sink, *ui_, 128, 120, "IMPERISHABLE NIGHT", 3.0f, moon);
    drawText(sink, *ui_, 176, 168, "BEING PORTED", 3.0f, white);
    drawText(sink, *ui_, 136, 240, folder_.archive ? "TH08.DAT: FOUND" : "TH08.DAT: MISSING", 2.0f,
             folder_.archive ? good : bad);
    drawText(sink, *ui_, 136, 272, folder_.thbgm ? "THBGM.DAT: FOUND" : "THBGM.DAT: MISSING", 2.0f,
             folder_.thbgm ? good : bad);
    drawText(sink, *ui_, 136, 336, folder_.exe ? "TH08.EXE 1.00D: FOUND" : "TH08.EXE 1.00D: MISSING", 2.0f,
             folder_.exe ? good : bad);
    drawText(sink, *ui_, 136, 416, std::format("{} FILES IN THE GAME FOLDER", folder_.files), 2.0f, grey);
}

void Th08Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | status screen", frame.fps, frame.ticks);
}

}  // namespace touhou::th08
