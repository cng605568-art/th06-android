#include "th07/Th07Game.hpp"

#include <cstdlib>
#include <format>

#include "core/Log.hpp"
#include "gfx/Image.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextureLibrary.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th07/Version.hpp"
#if defined(__ANDROID__)
#include <algorithm>
#include <vector>

#include "platform/AndroidStorage.hpp"
#endif

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
        for (const auto exe : ctx.vfs->executables()) {
            std::string why;
            if (loadExeTables(exe, tables_, &why)) {
                exe_ = true;
                break;
            }
            log::info("Not th07.exe 1.00b: {}", why);
        }
        if (!exe_) log::warn("No th07.exe 1.00b in the game folder: sound effects and other tables are missing");
        if (exe_ && ctx.samples) {
            sound_ = std::make_unique<SoundPlayer>(ctx.samples, &tables_);
            soundsLoaded_ = sound_->loadSamples(*ctx.vfs);
            log::info("Loaded {} of {} sound effects", soundsLoaded_, ExeTables::kSamples);
        }
        if (ctx.music) {
            music_ = std::make_unique<BgmPlayer>(ctx.music);
            music_->setData(std::move(records), thbgm);
            music_->load(8, "bgm/th07_01.mid");  // the title song
        }
    }
#if !defined(__ANDROID__)
    std::string previewFile, previewBackground;
    Gameplay::Setup play;
    bool startPlay = false;
    bool testBullets = false;
    autoFire_ = ctx.controls.autoFire;
    for (int i = 1; i < ctx.argc; ++i) {
        if (ctx.argv[i] && std::string(ctx.argv[i]) == "--hold-focus") holdFocus_ = true;
        if (ctx.argv[i] && std::string(ctx.argv[i]) == "--test-bullets") testBullets = true;
    }
    for (int i = 1; i + 1 < ctx.argc; ++i) {
        const std::string a = ctx.argv[i] ? ctx.argv[i] : "";
        if (a == "--anm-preview") previewFile = ctx.argv[i + 1];
        else if (a == "--anm-bg") previewBackground = ctx.argv[i + 1];
        else if (a == "--play") play.stage = std::atoi(ctx.argv[i + 1]), startPlay = true;
        else if (a == "--shot") play.shotIndex = std::atoi(ctx.argv[i + 1]);
        else if (a == "--difficulty") play.difficulty = std::atoi(ctx.argv[i + 1]);
        else if (a == "--stage-preview" && ctx.vfs) {
            std::string err;
            if (!background_.load(*ctx.vfs, *ctx.textures, anm_, rng_, std::atoi(ctx.argv[i + 1]), &err)) {
                log::warn("Stage preview: {}", err);
            }
        }
    }
    if (!previewFile.empty() && ctx.vfs) startPreview(ctx, previewFile, previewBackground);
#else
    // The launcher's start options (GameActivity.getGameOptions()): shot type 0-5, difficulty
    // 0-5 (4 Extra, 5 Phantasm: their own stage), stage 1-6, spare lives 0-4, practice. Play
    // starts straight in that stage (the menus are not ported yet) when th07.dat and
    // th07.exe are both 1.00b; otherwise the status screen says what is missing.
    Gameplay::Setup play;
    const bool testBullets = false;
    autoFire_ = ctx.controls.autoFire;
    const std::vector<int> options = androidIntArray("getGameOptions");
    if (options.size() >= 4) {
        play.shotIndex = std::clamp(options[0], 0, 5);
        play.difficulty = std::clamp(options[1], 0, 5);
        play.stage = play.difficulty == Extra      ? 7
                     : play.difficulty == Phantasm ? 8
                                                   : std::clamp(options[2], 1, 6);
        play.lifeCount = std::clamp(options[3], 0, 4);
        play.practice = options.size() >= 5 && options[4] != 0 && play.difficulty < Extra;
    }
    const bool startPlay = options.size() >= 4 && archive_;
#endif
    if (startPlay && ctx.vfs && exe_) {
        rng_.seed(ctx.seed);
        Gameplay::Resources res;
        res.vfs = ctx.vfs;
        res.textures = ctx.textures;
        res.anm = &anm_;
        res.rng = &rng_;
        res.tables = &tables_;
        res.sound = sound_.get();
        res.music = music_.get();
        res.text = ctx.text;
        res.ui = ctx.ui;
        game_ = std::make_unique<Gameplay>();
        std::string err;
        if (!game_->start(res, play, &err)) {
            log::warn("Could not start stage {}: {}", play.stage, err);
            game_.reset();
        } else {
            titleSongRequested_ = true;
            game_->setTestPatterns(testBullets);
        }
    }
    return true;
}

void Th07Game::startPreview(const GameContext& ctx, const std::string& file, const std::string& background) {
    std::string err;
    if (!anm_.load(*ctx.vfs, *ctx.textures, file, 32, 0x900, &err)) {
        log::warn("ANM preview: {}", err);
        return;
    }
    if (!background.empty()) {
        const Vfs* vfs = ctx.vfs;
        if (auto bytes = vfs->read(background)) {
            if (auto img = decodeImage(*bytes, &err)) {
                const int w = img->width, h = img->height;
                previewBackground_ = ctx.textures->add("th07-preview:" + background, w, h,
                                                       [vfs, background]() -> std::optional<Image> {
                                                           std::string e;
                                                           const auto b = vfs->read(background);
                                                           return b ? decodeImage(*b, &e) : std::nullopt;
                                                       });
            }
        }
    }
    for (int idx = 0x900; idx < AnmLibrary::kIndices; ++idx) {
        if (!anm_.script(idx)) continue;
        AnmVm vm;
        vm.startFresh(anm_, rng_, idx);
        previewVms_.push_back(vm);
    }
    log::info("ANM preview: {} scripts of {}", previewVms_.size(), file);
}

namespace {

std::uint16_t buttonWord(const InputState& in) {
    std::uint16_t b = 0;
    if (in.shoot) b |= BtnShoot;
    if (in.bomb) b |= BtnBomb;
    if (in.focus) b |= BtnFocus;
    if (in.pause) b |= BtnMenu;
    if (in.up) b |= BtnUp;
    if (in.down) b |= BtnDown;
    if (in.left) b |= BtnLeft;
    if (in.right) b |= BtnRight;
    if (in.skip) b |= BtnSkip;
    if (in.quit) b |= BtnQ;
    return b;
}

}  // namespace

void Th07Game::tick(const InputState& input) {
    if (game_) {
        std::uint16_t word = buttonWord(input);
        if (autoFire_) word |= BtnShoot;
        if (holdFocus_) word |= BtnFocus;
        game_->tick(word);
        std::string err;
        if (!game_->stageAdvance(&err)) log::warn("Next stage: {}", err);
        if (game_->finished()) quit_ = true;  // (no title screen yet: back to the launcher)
        if (music_) {
            music_->fadeStep();
            music_->pump();
        }
        if (sound_) sound_->flush();
        if (input.quit) quit_ = true;
        ++frame_;
        return;
    }
    for (AnmVm& vm : previewVms_) vm.tick(anm_, rng_);
    if (background_.loaded()) background_.update(anm_, rng_);
    if (music_) music_->fadeStep();
    ++frame_;
    if (input.quit) quit_ = true;
    if (music_ && !titleSongRequested_) {  // the title plays its song on its first frame
        music_->playLoaded(8);
        titleSongRequested_ = true;
    }
    if (sound_ && input.shoot && !lastShoot_) sound_->playSound(10);  // the menu "OK" sound, as a test
    lastShoot_ = input.shoot;
    // The audio pump: one music command step, then the frame's sound effects.
    if (music_) music_->pump();
    if (sound_) sound_->flush();
}

void Th07Game::render(SpriteSink& sink, float, const FrameInfo&) {
    if (game_) {
        game_->draw(sink);
        return;
    }
    if (!ui_) return;
    if (background_.loaded()) {
        AnmDrawState state;
        background_.drawHigh(sink, state);
        background_.drawLow(sink, state);
        return;
    }
    if (!previewVms_.empty()) {
        if (previewBackground_ >= 0) {
            SpriteInstance s;
            s.x = 320, s.y = 240, s.w = 640, s.h = 480;
            sink.draw(static_cast<std::uint16_t>(previewBackground_), Blend::Alpha, s);
        }
        const AnmDrawState state;
        for (const AnmVm& vm : previewVms_) drawVm(sink, vm, state);
        return;
    }
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
    drawText(sink, *ui_, 136, 336, exe_ ? std::format("TH07.EXE 1.00B: FOUND, {} SOUNDS", soundsLoaded_) : "TH07.EXE 1.00B: MISSING",
             2.0f, exe_ ? good : bad);
    if (sound_) drawText(sink, *ui_, 136, 368, "PRESS SHOT TO TEST A SOUND", 2.0f, grey);
    if (music_ && music_->playing()) drawText(sink, *ui_, 136, 304, "NOW PLAYING: " + music_->current(), 2.0f, grey);
    drawText(sink, *ui_, 136, 416, std::format("{} FILES IN THE GAME FOLDER", files_), 2.0f, grey);
}

void Th07Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | placeholder screen", frame.fps, frame.ticks);
}

}  // namespace touhou::th07
