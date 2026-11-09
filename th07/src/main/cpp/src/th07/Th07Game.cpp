#include "th07/Th07Game.hpp"

#include <cstdlib>
#include <format>

#include "core/Log.hpp"
#include "gfx/Image.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextureLibrary.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th07/MainMenu.hpp"
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

Th07Game::~Th07Game() {
    if (flow_) flow_->shutdown();
    flow_.reset();
    if (vfs_) vfs_->setReadFilter(nullptr);  // (the files outlive the game)
}

void Th07Game::setUpEnglish(const GameContext& ctx) {
    std::string root;
    bool wanted = false;
#if !defined(__ANDROID__)
    // Desktop: --english reads the thcrap folder inside the game folder (the folder given
    // first, as the data mount takes it, or $TH07_DATA, or ./gamedata); --thcrap DIR names a
    // thcrap folder (the one holding repos/) and implies --english.
    std::string gameDir = "gamedata";
    if (ctx.argc > 1 && ctx.argv[1] && ctx.argv[1][0] != '-') gameDir = ctx.argv[1];
    else if (const char* env = std::getenv("TH07_DATA")) gameDir = env;
    for (int i = 1; i < ctx.argc; ++i) {
        const std::string a = ctx.argv[i] ? ctx.argv[i] : "";
        if (a == "--english") wanted = true;
        else if (a == "--thcrap" && i + 1 < ctx.argc) root = ctx.argv[++i], wanted = true;
    }
    if (wanted && root.empty()) root = gameDir + "/thcrap";
#else
    // The launcher's "English text" option (GameActivity.getGameOptions(), the 7th value);
    // the launcher imports the patch's files into the app's storage under thcrap/.
    const std::vector<int> options = androidIntArray("getGameOptions");
    wanted = options.size() >= 7 && options[6] != 0;
    root = ctx.storageDir + "thcrap";
#endif
    if (!wanted || !ctx.vfs) return;
    auto stack = English::discoverStack(root);
    if (stack.empty()) {
        log::warn("English text: no thcrap English patch (repos/*/lang_en) in {}: the original text is shown", root);
        return;
    }
    english_ = std::make_unique<English>(std::move(stack));
    const English* e = english_.get();
    ctx.vfs->setReadFilter([e](const std::string& name, std::optional<std::vector<std::uint8_t>> bytes) {
        return e->filterFile(name, std::move(bytes));
    });
}

bool Th07Game::start(const GameContext& ctx) {
    ui_ = ctx.ui;
    vfs_ = ctx.vfs;
    setUpEnglish(ctx);
    files_ = ctx.vfs ? ctx.vfs->fileCount() : 0;
    if (ctx.vfs) {
        if (const auto ver = ctx.vfs->read(kVersionFile)) {
            const auto line = findVersionLine(*ver, kVersionTag);
            archive_ = line.has_value();
            if (line) {
                log::info("th07.dat is 1.00b (th07.exe: {} bytes, checksum {:08x})", line->size, line->checksum);
                exeId_ = *line;
                verFile_ = *ver;
            }
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
    // The supervisor's start options.
    Gameplay::Setup play;
    bool startPlay = false;
    int endingShot = -1, endingDifficulty = Normal;
    bool endingContinued = false;
    std::uint32_t endingScore = 0;
    bool unlockAll = false;
#if !defined(__ANDROID__)
    // Desktop developer options: --play STAGE [--shot N] [--difficulty D] [--practice]
    // [--lives N] starts a game at once; --ending SHOT [--continued] [--difficulty D] [--score
    // N] shows that ending (and the result screen with that score); --unlock-all saves every
    // clear record at 99 (Extra, Phantasm, all practice stages); --anm-preview FILE [--anm-bg PICTURE] and --stage-preview N show ANM scripts
    // or a stage background on their own.
    std::string previewFile, previewBackground;
    bool testBullets = false;
    // --test-msg N and --test-spell N show dialogue script N / spell card N's banner at the
    // started stage's frame 30 (to look at their text, e.g. with --english).
    int testMsg = -1, testSpell = -1;
    // --replay FILE [--replay-stage N (1-7)] [--replay-mode M (0-2)] plays a replay file;
    // --demo N plays attract demo N (0-2) from th07.dat; --replay-check compares each played
    // frame's event flags with the recorded ones; --replay-log N logs the state every N frames.
    std::string replayPath;
    int replayStage = -1, replayMode = 0, demoIndex = -1;
    autoFire_ = ctx.controls.autoFire;
    for (int i = 1; i < ctx.argc; ++i) {
        const std::string a = ctx.argv[i] ? ctx.argv[i] : "";
        if (a == "--hold-focus") holdFocus_ = true;
        else if (a == "--test-bullets") testBullets = true;
        else if (a == "--practice") play.practice = true;
        else if (a == "--continued") endingContinued = true;
        else if (a == "--unlock-all") unlockAll = true;
        else if (a == "--replay-check") check_.on = true;
    }
    for (int i = 1; i + 1 < ctx.argc; ++i) {
        const std::string a = ctx.argv[i] ? ctx.argv[i] : "";
        if (a == "--replay") replayPath = ctx.argv[i + 1];
        else if (a == "--replay-stage") replayStage = std::clamp(std::atoi(ctx.argv[i + 1]), 1, 7) - 1;
        else if (a == "--replay-mode") replayMode = std::clamp(std::atoi(ctx.argv[i + 1]), 0, 2);
        else if (a == "--demo") demoIndex = std::clamp(std::atoi(ctx.argv[i + 1]), 0, 2);
        else if (a == "--replay-log") check_.logEvery = std::max(0, std::atoi(ctx.argv[i + 1]));
        else if (a == "--test-msg") testMsg = std::max(0, std::atoi(ctx.argv[i + 1]));
        else if (a == "--test-spell") testSpell = std::clamp(std::atoi(ctx.argv[i + 1]), 0, ExeTables::kSpellCards - 1);
        else if (a == "--anm-preview") previewFile = ctx.argv[i + 1];
        else if (a == "--anm-bg") previewBackground = ctx.argv[i + 1];
        else if (a == "--play") play.stage = std::atoi(ctx.argv[i + 1]), startPlay = true;
        else if (a == "--shot") play.shotIndex = std::clamp(std::atoi(ctx.argv[i + 1]), 0, 5);
        else if (a == "--difficulty") play.difficulty = endingDifficulty = std::clamp(std::atoi(ctx.argv[i + 1]), 0, 5);
        else if (a == "--lives") play.lifeCount = std::clamp(std::atoi(ctx.argv[i + 1]), 0, 4);
        else if (a == "--ending") endingShot = std::clamp(std::atoi(ctx.argv[i + 1]), 0, 5);
        else if (a == "--score") endingScore = static_cast<std::uint32_t>(std::atoll(ctx.argv[i + 1]));
        else if (a == "--stage-preview" && ctx.vfs) {
            std::string err;
            if (!background_.load(*ctx.vfs, *ctx.textures, anm_, rng_, std::atoi(ctx.argv[i + 1]), &err)) {
                log::warn("Stage preview: {}", err);
            }
        }
    }
    if (play.difficulty >= Extra) play.stage = play.difficulty + 3, play.practice = false;
    if (!previewFile.empty() && ctx.vfs) startPreview(ctx, previewFile, previewBackground);
    const bool preview = !previewVms_.empty() || background_.loaded();
#else
    // The launcher's start options (GameActivity.getGameOptions()): shot type 0-5, difficulty
    // 0-5 (4 Extra, 5 Phantasm: their own stage), stage 1-6, spare lives 0-4, practice, and
    // "skip the title screen". Without the last one (or with it off) the title opens.
    const bool testBullets = false;
    const int testMsg = -1, testSpell = -1;
    const bool preview = false;
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
    startPlay = options.size() >= 6 && options[5] != 0;
    const std::string replayPath;
    const int replayStage = -1, replayMode = 0, demoIndex = -1;
#endif
    if (archive_ && ctx.vfs && !preview) {
        rng_.seed(ctx.seed);
        if (!ctx.text) fallbackText_ = std::make_unique<FallbackTextRenderer>();
        Supervisor::Boot b;
        b.vfs = ctx.vfs;
        b.textures = ctx.textures;
        b.text = ctx.text ? ctx.text : fallbackText_.get();
        b.ui = ctx.ui;
        b.sound = sound_.get();
        b.music = music_.get();
        b.tables = &tables_;
        b.anm = &anm_;
        b.rng = &rng_;
        b.english = english_.get();
        b.storageDir = ctx.storageDir;
        b.verFile = verFile_;
        b.exe = exeId_;
        b.thbgm = thbgm_;
        b.canPlay = exe_;
        flow_ = std::make_unique<Supervisor>();
        flow_->start(b);
        titleSongRequested_ = true;
        if (unlockAll) flow_->unlockAll();
        if (check_.on || check_.logEvery > 0) installReplayCheck();
        std::shared_ptr<ReplayFile> replay;
        if (!replayPath.empty() || demoIndex >= 0) {
            std::optional<std::vector<std::uint8_t>> bytes;
            if (demoIndex >= 0) {
                bytes = ctx.vfs->read(std::format("data/demo/demorpy{}.rpy", demoIndex));
            } else if (std::FILE* f = std::fopen(replayPath.c_str(), "rb")) {
                bytes.emplace();
                std::uint8_t buf[8192];
                for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes->insert(bytes->end(), buf, buf + n);
                std::fclose(f);
            }
            std::string err = "cannot be read";
            auto parsed = bytes ? ReplayFile::parse(*bytes, verFile_, &err) : std::nullopt;
            if (parsed) replay = std::make_shared<ReplayFile>(std::move(*parsed));
            else log::warn("Replay {}: {}", demoIndex >= 0 ? std::format("demo {}", demoIndex) : replayPath, err);
            check_.file = replay;
        }
        if (endingShot >= 0) {
            flow_->startEnding(endingShot, endingDifficulty, endingContinued, endingScore);
        } else if (replay && exe_) {
            const ReplaySummary& s = replay->summary;
            log::info("th07: replay {} {} \"{}\" {} score {}0, stages {:07b}", shotName7(s.shot()), difficultyName(s.difficulty()), s.name(),
                      s.date(), s.score(), replay->stageMask());
            for (int i = 0; i < kReplayStages; ++i) {
                const ReplayStage& st = replay->stages[static_cast<std::size_t>(i)];
                if (!st.present) continue;
                log::info("th07: replay block {}: end score {}0, seed {:04x}, {} entries, lives {} bombs {} power {} rank {}", i + 1,
                          st.endScore, st.seed, st.entries.size(), st.lives, st.bombs, st.power, st.rank);
            }
            if (!flow_->startReplay(replay, replayStage, demoIndex >= 0 ? 0 : replayMode, demoIndex >= 0, std::max(demoIndex, 0))) {
                log::warn("Could not start the replay: the title opens instead");
            }
        } else if (startPlay && exe_) {
            if (!flow_->startGame(play)) log::warn("Could not start stage {}: the title opens instead", play.stage);
            else if (flow_->game()) {
                flow_->game()->setTestPatterns(testBullets);
                flow_->game()->setTestText(testMsg, testSpell);
            }
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
    if (flow_ && flow_->loadingFrame()) {
        // No logic while the loading frame is presented (render shows it); a frame that is
        // never rendered does not hold the game back for more than a few passes.
        if (++loadingTicks_ <= 4) return;
        flow_->dropLoadingFrame();
    }
    loadingTicks_ = 0;
    if (flow_) {
        if (music_) music_->fadeStep();
        flow_->setGameExtras(static_cast<std::uint16_t>((autoFire_ ? BtnShoot : 0) | (holdFocus_ ? BtnFocus : 0)));
        flow_->pass(buttonWord(input));
        if (flow_->quit()) {
            flow_->shutdown();
            quit_ = true;
        }
        if (music_) music_->pump();
        if (sound_) sound_->flush();
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

void Th07Game::render(SpriteSink& sink, float, const FrameInfo& frame) {
    // The FPS measurement: passes per real second, every 0.5 s or more. A window longer than
    // 2 s (the app was in the background) starts over instead of reporting a slowdown.
    if (flow_) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - fpsStart_).count();
        if (!fpsStarted_ || elapsed > 2.0 || frame.ticks < fpsTicks_) {
            fpsStart_ = now;
            fpsTicks_ = frame.ticks;
            fpsStarted_ = true;
        } else if (elapsed >= 0.5) {
            flow_->fpsMeasured(static_cast<float>(static_cast<double>(frame.ticks - fpsTicks_) / elapsed));
            fpsStart_ = now;
            fpsTicks_ = frame.ticks;
        }
    }
    if (flow_ && flow_->loadingFrame()) {
        flow_->drawLoadingFrame(sink);
        return;
    }
    // The supervisor's screens (the status screen below when the title cannot open).
    if (flow_ && !(flow_->active() == FlowState::MainMenu && !flow_->mainMenu())) {
        flow_->draw(sink);
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

bool Th07Game::takeCaptureRequest() {
    Gameplay* g = flow_ ? flow_->game() : nullptr;
    return g && g->takeCaptureRequest();
}

int Th07Game::captureTexture() const {
    const std::uint16_t id = anm_.slotTexture(4);  // capture.anm, loaded at boot
    if (id == 0) return -1;
    // An empty rectangle would make the host fill the whole texture: refuse instead.
    const CaptureRect r = captureRect();
    return r.w > 0 && r.h > 0 ? id : -1;
}

void Th07Game::setCaptureReady(bool ready) {
    if (Gameplay* g = flow_ ? flow_->game() : nullptr) g->setCaptureReady(ready);
}

Game::CaptureRect Th07Game::captureRect() const {
    CaptureRect r;
    if (Gameplay* g = flow_ ? flow_->game() : nullptr) g->captureRect(r.x, r.y, r.w, r.h);
    return r;
}

void Th07Game::installReplayCheck() {
    flow_->setPlaybackObserver([this](int stage, std::int32_t frame, const ReplayEntry& rec, std::uint32_t flags) {
        ReplayCheck& c = check_;
        const Gameplay* game = flow_->game();
        if (!game) return;
        const GameState& g = game->state();
        if (stage != c.stage) {
            closeReplayCheckStage();
            c = ReplayCheck{c.on, c.logEvery, stage, 0, 0, -1, 0, g.livesI(), 0, std::move(c.summary), c.file};
        }
        ++c.frames;
        if (g.livesI() < c.lastLives) ++c.deaths;
        c.lastLives = g.livesI();
        c.lastScore = g.score;
        // (Bit 0x0100 is "paused before this frame", the recording's own; the terminators'
        // uninitialised flags are never compared, playback ends before them.)
        const std::uint32_t want = rec.flags & ~0x0100u, got = flags & 0xFFFFu & ~0x0100u;
        if (c.on && want != got) {
            ++c.mismatches;
            if (c.firstMismatch < 0) c.firstMismatch = frame;
            if (c.mismatches <= 12) {
                const Vec3f p = game->player().position();
                log::info("th07 replay check: stage {} frame {}: recorded flags {:04x}, simulated {:04x} (score {}0, lives {}, bombs {}, "
                          "power {}, player ({:.3f}, {:.3f}), seed {:04x})",
                          stage, frame, want, got, g.score, g.livesI(), g.bombsI(), g.powerI(), p.x, p.y, flow_->context().rng->currentSeed());
            }
        }
        if (c.logEvery > 0 && frame % c.logEvery == 0) {
            const Vec3f p = game->player().position();
            log::info("th07 replay: stage {} frame {} score {}0 lives {} bombs {} power {} graze {} cherry {} rank {} player ({:.3f}, {:.3f}) seed {:04x}",
                      stage, frame, g.score, g.livesI(), g.bombsI(), g.powerI(), g.totalGraze, g.cherry, g.rank, p.x, p.y,
                      flow_->context().rng->currentSeed());
        }
    });
}

void Th07Game::closeReplayCheckStage() const {
    ReplayCheck& c = check_;
    if (c.stage <= 0 || c.frames == 0) return;
    const int index = c.stage >= 7 ? 6 : c.stage - 1;
    std::string recorded = "?";
    if (c.file && c.file->hasStage(index)) {
        const ReplayStage& st = c.file->stages[static_cast<std::size_t>(index)];
        // Recorded frames: the entries less entry 0 and the terminators (one, or two for the last block).
        const bool last = index == 6 || !c.file->hasStage(index + 1);
        const std::size_t n = st.entries.size() > (last ? 3u : 2u) ? st.entries.size() - (last ? 3u : 2u) : 0;
        recorded = std::format("{}0 after {} frames", st.endScore, n);
    }
    c.summary.push_back(std::format("stage {}: {} frames played, {} with other event flags (first at {}), {} lives lost; score {}0 (recorded {})",
                                    c.stage, c.frames, c.mismatches, c.firstMismatch, c.deaths, c.lastScore, recorded));
    c.frames = 0;
}

void Th07Game::logSummary() const {
    closeReplayCheckStage();
    for (const std::string& s : check_.summary) log::info("th07 replay check: {}", s);
}

void Th07Game::logStatus(const FrameInfo& frame) const {
    if (!flow_) {
        log::info("fps {:.1f} | ticks {} | status screen", frame.fps, frame.ticks);
        return;
    }
    log::info("fps {:.1f} | ticks {} | state {}", frame.fps, frame.ticks, static_cast<int>(flow_->active()));
}

}  // namespace touhou::th07
