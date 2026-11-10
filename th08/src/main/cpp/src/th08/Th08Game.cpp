#include "th08/Th08Game.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <random>

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

Th08Game::~Th08Game() {
    if (menu_ || musicRoom_ || gameplay_) saveConfig();
    gameplay_.reset();
    musicRoom_.reset();
    if (music_) music_->shutdown();
}

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

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

void Th08Game::saveConfig() const {
    if (storage_.empty()) return;
    const auto bytes = config_.save();
    std::ofstream f(storage_ + "th08.cfg", std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// score.dat as the port keeps it (the app's own copy): written whenever a record the port
// tracks changes, as a whole new file (spec_files §5.2).
void Th08Game::saveScores() {
    if (storage_.empty()) return;
    std::vector<std::uint8_t> image = scores_.imageToSave();
    // The two random header bytes; not from the game's RNG, so saving draws nothing from it.
    std::minstd_rand bytesRng(static_cast<std::uint32_t>(ticks_ * 2654435761u + scores_.totalPlays + 1));
    const auto seed = static_cast<std::uint8_t>(bytesRng() & 0xFF), filler = static_cast<std::uint8_t>(bytesRng() & 0xFF);
    const auto bytes = encodeScoreFile(image, seed, filler);
    std::ofstream f(storage_ + "score.dat", std::ios::binary | std::ios::trunc);
    if (!f) {
        log::warn("score.dat could not be written");
        return;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    // Later saves patch the image just written (a fresh one when there was no file).
    if (const auto decoded = decodeScoreFile(bytes)) {
        ScoreData reread;
        if (parseScoreImage(*decoded, reread)) {
            scores_.image = std::move(reread.image);
            scores_.plstOffset = reread.plstOffset;
        }
    }
}

// The Supervisor's scene change (spec §1.2), at the start of the pass after the request.
void Th08Game::applyScene() {
    const Scene from = active_, to = requested_;
    active_ = to;
    flow_.previous = from;
    std::string err;
    if (to == Scene::Exit) {
        quit_ = true;
        return;
    }
    if (to == Scene::Game) {
        menu_.reset();
        musicRoom_.reset();
        gameplay_ = std::make_unique<Gameplay>(flow_);
        Gameplay::Setup setup;
        setup.stage = flow_.stage;
        setup.character = flow_.character;
        setup.difficulty = config_.difficulty;
        setup.practice = flow_.practice;
        setup.spellPractice = flow_.spellPractice;
        setup.lives = config_.lives;
        // Developer aid: TH08_DEV_STAGE=<stage index 0-8> starts that stage instead.
        if (const char* dev = std::getenv("TH08_DEV_STAGE"); dev && *dev) setup.stage = std::clamp(std::atoi(dev), 0, kStageCount - 1);
        // TH08_DEV_PRACTICE=1 makes it a practice game (eight lives).
        if (const char* dev = std::getenv("TH08_DEV_PRACTICE"); dev && *dev == '1') setup.practice = true;
        // TH08_DEV_CHARACTER=<0-11> plays that character (0-3 teams, 4-11 solos).
        if (const char* dev = std::getenv("TH08_DEV_CHARACTER"); dev && *dev) setup.character = std::clamp(std::atoi(dev), 0, 11);
        if (gameplay_->start(setup, &err)) return;
        log::warn("The game could not start: {}", err);
        gameplay_.reset();
        active_ = requested_ = Scene::MainMenu;  // as Init
    }
    if (to == Scene::MusicRoom) {
        menu_.reset();
        musicRoom_ = std::make_unique<MusicRoom>(flow_);
        if (musicRoom_->create(&err)) return;
        log::warn("The Music Room could not be built: {}", err);
        musicRoom_.reset();
        active_ = requested_ = Scene::MainMenu;  // as Init
    }
    if (active_ == Scene::MainMenu) {
        musicRoom_.reset();
        gameplay_.reset();
        menu_ = std::make_unique<MainMenu>(flow_);
        if (!menu_->create(&err)) {
            log::warn("The title could not be built: {}", err);
            menu_.reset();
        }
        return;
    }
    log::warn("Scene {} is not ported yet: back to the title", static_cast<int>(to));
    requested_ = Scene::MainMenu;
    applyScene();
}

bool Th08Game::start(const GameContext& ctx) {
    ui_ = ctx.ui;
    storage_ = ctx.storageDir;
    rng_.seed(ctx.seed);
    if (ctx.vfs) folder_ = inspectFolder(*ctx.vfs);
    log::info("th08.dat 1.00d: {}; th08.exe 1.00d: {}; thbgm.dat: {}; {} files", folder_.archive ? "mounted" : "missing",
              folder_.exe ? "found" : "missing", folder_.thbgm ? "found" : "missing", folder_.files);
    if (!folder_.archive || !ctx.vfs || !ctx.textures) return true;  // the status screen

    // th08.cfg (the port's own copy), re-initialised and written back when invalid.
    if (!storage_.empty()) {
        if (!config_.load(readFile(storage_ + "th08.cfg"), folder_.thbgm)) log::info("th08.cfg initialised");
        saveConfig();
    } else {
        config_.load({}, folder_.thbgm);
    }
    // score.dat: the port's own copy, else the one in the game folder (desktop). A missing or
    // invalid file counts as empty.
    {
        std::vector<std::uint8_t> raw = storage_.empty() ? std::vector<std::uint8_t>{} : readFile(storage_ + "score.dat");
        if (raw.empty()) {
            const auto loose = ctx.vfs->looseFile("score.dat");
            raw.assign(loose.begin(), loose.end());
        }
        if (!raw.empty()) {
            const auto image = decodeScoreFile(raw);
            const std::pair<std::int32_t, std::int32_t> exeVersion{840704, static_cast<std::int32_t>(2724749753u)};
            if (!image || !parseScoreImage(*image, scores_, exeVersion)) log::warn("score.dat is not valid: starting from no records");
            else log::info("score.dat: {} plays, Extra {}, Spell Practice {}, solo characters {}", scores_.totalPlays,
                           scores_.extraUnlocked() ? "unlocked" : "locked", scores_.spellPracticeUnlocked() ? "unlocked" : "locked",
                           scores_.solosUnlocked() ? "unlocked" : "locked");
        }
    }
    if (folder_.exe) {
        std::string why;
        tablesLoaded_ = loadExeTables(folder_.exeBytes, tables_, &why);
        if (!tablesLoaded_) log::warn("th08.exe tables: {}", why);
    }
    if (tablesLoaded_ && ctx.samples) {
        sound_ = std::make_unique<SoundPlayer>(ctx.samples, &tables_);
        log::info("Loaded {} of {} sound effects", sound_->loadSamples(*ctx.vfs), ExeTables::kSamples);
        sound_->setVolumePercent(config_.seVolume);
    }
    if (ctx.music) {
        std::vector<BgmRecord> records;
        if (const auto fmt = ctx.vfs->read("bgm/thbgm.fmt")) records = parseThbgmFmt(*fmt);
        songs_ = records.size();
        std::span<const std::uint8_t> thbgm = ctx.vfs->looseFile("thbgm.dat");
        if (!thbgm.empty() && !validThbgmHeader(thbgm)) {
            log::warn("thbgm.dat has the wrong header: playing without music");
            thbgm = {};
        }
        music_ = std::make_unique<BgmPlayer>(ctx.music);
        music_->setData(std::move(records), thbgm);
        music_->setMode(config_.music);
        music_->setVolumePercent(config_.bgmVolume);
        if (!storage_.empty()) music_->setOverrideDir(storage_ + "music/");
        log::info("Music: {} song(s) in thbgm.fmt, thbgm.dat {}", songs_, thbgm.empty() ? "missing" : "found");
    }

    // The files the boot loads (spec 1.3): text.anm (text is drawn into it), ascii.anm,
    // capture.anm, nowloading.anm.
    std::string err;
    bool ok = anm_.load(*ctx.vfs, *ctx.textures, "text.anm", kSlotText, &err, true);
    ok = ok && anm_.load(*ctx.vfs, *ctx.textures, "ascii.anm", kSlotAscii, &err);
    ok = ok && anm_.load(*ctx.vfs, *ctx.textures, "capture.anm", kSlotCapture, &err, true);
    ok = ok && anm_.load(*ctx.vfs, *ctx.textures, "nowloading.anm", kSlotNowLoading, &err);
    if (!ok) {
        log::warn("{}", err);
        return true;
    }
    if (!ctx.text) fallbackText_ = std::make_unique<FallbackTextRenderer>();

    flow_.vfs = ctx.vfs;
    flow_.textures = ctx.textures;
    flow_.text = ctx.text ? ctx.text : fallbackText_.get();
    flow_.ui = ctx.ui;
    flow_.anm = &anm_;
    flow_.rng = &rng_;
    flow_.sound = sound_.get();
    flow_.music = music_.get();
    flow_.tables = tablesLoaded_ ? &tables_ : nullptr;
    flow_.config = &config_;
    flow_.request = [this](Scene s) { requested_ = s; };
    flow_.saveConfig = [this]() { saveConfig(); };
    if (const auto txt = ctx.vfs->read("musiccmt.txt")) musicTracks_ = parseMusicComments(*txt);
    flow_.musicHeard = &scores_.musicHeard;
    flow_.scores = &scores_;
    flow_.musicPlayed = [this](std::string_view path) {
        if (scores_.setMusicHeard(musicTrackIndex(musicTracks_, path))) saveScores();
    };
    flow_.extraUnlocked = scores_.extraUnlocked();
    flow_.spellPracticeUnlocked = scores_.spellPracticeUnlocked();
    flow_.solosUnlocked = scores_.solosUnlocked();
    flow_.totalPlays = static_cast<int>(scores_.totalPlays);
    menu_ = std::make_unique<MainMenu>(flow_);
    if (!menu_->create(&err)) {
        log::warn("The title could not be built: {}", err);
        menu_.reset();
    }
    return true;
}

void Th08Game::tick(const InputState& input) {
    ++ticks_;
    if (music_) music_->fadeStep();
    if (menu_ || musicRoom_ || gameplay_) {
        input_.sample(buttonWord(input));
        if (requested_ != active_) {
            applyScene();
            input_.clear();  // the transition zeroes the raw input and the repeat flag
        }
        if (gameplay_) {
            // Developer aid: TH08_DEV_HOLD="shoot+focus+left" holds those buttons in a game.
            static const std::uint16_t devHold = [] {
                std::uint16_t m = 0;
                if (const char* h = std::getenv("TH08_DEV_HOLD")) {
                    const std::string v(h);
                    const std::pair<const char*, std::uint16_t> names[] = {{"shoot", BtnShoot}, {"bomb", BtnBomb}, {"focus", BtnFocus},
                                                                           {"up", BtnUp},       {"down", BtnDown}, {"left", BtnLeft},
                                                                           {"right", BtnRight}, {"skip", BtnSkip}};
                    for (const auto& [n, b] : names) m |= v.find(n) != std::string::npos ? b : 0;
                }
                return m;
            }();
            if (devHold) {
                input_.clear();
                input_.sample(static_cast<std::uint16_t>(buttonWord(input) | devHold));
            }
            // Developer aid: TH08_DEV_LIVES=1 keeps the lives at 8 (to reach late scenes); 2 also
            // makes the player ignore hits.
            static const bool devLives = [] {
                const char* v = std::getenv("TH08_DEV_LIVES");
                if (v && *v == '2') Player::devNoDeath = true;
                return v && (*v == '1' || *v == '2');
            }();
            if (devLives && gameplay_->world().state) gameplay_->world().state->lives = 8.0f;
            gameplay_->tick(input_);
            if (gameplay_->outcome() == Gameplay::Outcome::Title) requested_ = Scene::MainMenu;
        } else if (musicRoom_) {
            musicRoom_->update(input_);
        } else if (menu_) {
            menu_->update(input_);
        }
    } else if (input.quit) {
        quit_ = true;
    }
    if (music_) music_->pump();
    if (sound_) sound_->flush();
}

void Th08Game::render(SpriteSink& sink, float, const FrameInfo&) {
    if (gameplay_) {
        gameplay_->draw(sink);
        return;
    }
    if (musicRoom_) {
        musicRoom_->draw(sink);
        return;
    }
    if (menu_) {
        menu_->draw(sink);
        return;
    }
    if (!ui_) return;
    const std::uint32_t moon = rgba(250, 226, 150), white = rgba(255, 255, 255), grey = rgba(170, 170, 170);
    const std::uint32_t good = rgba(140, 230, 140), bad = rgba(255, 140, 140);
    drawText(sink, *ui_, 128, 120, "IMPERISHABLE NIGHT", 3.0f, moon);
    drawText(sink, *ui_, 176, 168, "BEING PORTED", 3.0f, white);
    drawText(sink, *ui_, 136, 240, folder_.archive ? "TH08.DAT 1.00D: FOUND" : "TH08.DAT 1.00D: MISSING", 2.0f,
             folder_.archive ? good : bad);
    drawText(sink, *ui_, 136, 272, folder_.thbgm ? "THBGM.DAT: FOUND" : "THBGM.DAT: MISSING", 2.0f,
             folder_.thbgm ? good : bad);
    drawText(sink, *ui_, 136, 336, folder_.exe ? "TH08.EXE 1.00D: FOUND" : "TH08.EXE 1.00D: MISSING", 2.0f,
             folder_.exe ? good : bad);
    drawText(sink, *ui_, 136, 416, std::format("{} FILES IN THE GAME FOLDER", folder_.files), 2.0f, grey);
}

// Android Back: in a game, what Menu does (the pause menu).
// The Gui's playfield captures (the results' snapshot, the next stage's tiles), into
// capture.anm's texture at the snapshot sprite's rectangle.
bool Th08Game::takeCaptureRequest() { return gameplay_ && gameplay_->takeCaptureRequest(); }

int Th08Game::captureTexture() const {
    const AnmFile* f = anm_.file(kSlotCapture);
    if (!f || !f->loaded() || f->textures.empty()) return -1;
    const CaptureRect r = captureRect();
    return r.w > 0 && r.h > 0 ? f->textures[0] : -1;  // (an empty rectangle would fill the whole texture)
}

void Th08Game::setCaptureReady(bool ready) {
    if (gameplay_) gameplay_->setCaptureReady(ready);
}

Game::CaptureRect Th08Game::captureRect() const {
    CaptureRect r;
    if (gameplay_) gameplay_->captureRect(r.x, r.y, r.w, r.h);
    return r;
}

// The "Press Shot Button" wait between stages draws over the last frame (the original copies
// a capture of it back every frame).
bool Th08Game::keepsPreviousFrame() const { return gameplay_ && gameplay_->stageEndState() == 1; }

bool Th08Game::back() {
    if (!gameplay_) return false;
    gameplay_->requestPause();
    return true;
}

void Th08Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | {}", frame.fps, frame.ticks, gameplay_ ? std::format("stage index {} frame {}", gameplay_->state().stage, gameplay_->stageFrame())
                         : musicRoom_ ? std::string("Music Room") : menu_ ? std::string("title") : std::string("status screen"));
}

}  // namespace touhou::th08
