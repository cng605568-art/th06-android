#include "th07/Th07Game.hpp"

#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th07/Version.hpp"

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
    if (ctx.vfs) {
        if (const auto ver = ctx.vfs->read(kVersionFile)) {
            const auto line = findVersionLine(*ver, kVersionTag);
            archive_ = line.has_value();
            if (line) log::info("th07.dat is 1.00b (th07.exe: {} bytes, checksum {:08x})", line->size, line->checksum);
        }
        if (!archive_) log::warn("No 1.00b th07.dat (its {} is missing)", kVersionFile);
        std::vector<BgmRecord> records;
        if (const auto fmt = ctx.vfs->read("bgm/thbgm.fmt")) records = parseThbgmFmt(*fmt);
        songs_ = records.size();
        std::span<const std::uint8_t> thbgm = ctx.vfs->looseFile("thbgm.dat");
        thbgmBad_ = !thbgm.empty() && !validThbgmHeader(thbgm);
        if (thbgmBad_) {
            log::warn("thbgm.dat has the wrong header: playing without music");
            thbgm = {};
        }
        thbgm_ = !thbgm.empty();
        log::info("Music: {} song(s) in thbgm.fmt, thbgm.dat {}", songs_, thbgm_ ? "found" : "missing");
        if (ctx.music) {
            music_ = std::make_unique<BgmPlayer>(ctx.music);
            music_->setData(std::move(records), thbgm);
            music_->load(8, "bgm/th07_01.mid");  // the title song
        }
    }
    return true;
}

void Th07Game::tick(const InputState& input) {
    if (music_) music_->fadeStep();
    ++frame_;
    if (input.quit) quit_ = true;
    if (music_ && !titleSongRequested_) {  // the title plays its song on its first frame
        music_->playLoaded(8);
        titleSongRequested_ = true;
    }
    if (music_) music_->pump();
}

void Th07Game::render(SpriteSink& sink, float, const FrameInfo&) {
    if (!ui_) return;
    const std::uint32_t pink = rgba(255, 183, 197), white = rgba(255, 255, 255), grey = rgba(170, 170, 170);
    const std::uint32_t good = rgba(140, 230, 140), bad = rgba(255, 140, 140);
    drawText(sink, *ui_, 112, 120, "PERFECT CHERRY BLOSSOM", 3.0f, pink);
    drawText(sink, *ui_, 176, 168, "BEING PORTED", 3.0f, white);
    drawText(sink, *ui_, 136, 240, archive_ ? "TH07.DAT 1.00B: FOUND" : "TH07.DAT 1.00B: MISSING", 2.0f,
             archive_ ? good : bad);
    const std::string music = thbgmBad_   ? "THBGM.DAT: WRONG FILE"
                              : !thbgm_   ? "THBGM.DAT: MISSING"
                              : songs_ == 0 ? "THBGM.FMT: MISSING"
                                            : std::format("MUSIC: {} SONGS", songs_);
    drawText(sink, *ui_, 136, 272, music, 2.0f, thbgm_ && songs_ ? good : bad);
    if (music_ && music_->playing()) drawText(sink, *ui_, 136, 304, "NOW PLAYING: " + music_->current(), 2.0f, grey);
    drawText(sink, *ui_, 136, 352, std::format("{} FILES IN THE GAME FOLDER", files_), 2.0f, grey);
}

void Th07Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | placeholder screen", frame.fps, frame.ticks);
}

}  // namespace touhou::th07
