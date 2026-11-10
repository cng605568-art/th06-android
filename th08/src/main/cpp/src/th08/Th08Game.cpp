#include "th08/Th08Game.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <ctime>
#include <filesystem>
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
    if (anyScene()) {
        saveConfig();
        // The program exit writes score.dat (spec_result_ending 1.5 #3; the in-memory records).
        saveScores(true);
    }
    result_.reset();
    ending_.reset();
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
void Th08Game::saveScores(bool gameRng) {
    std::uint8_t seed, filler;
    if (gameRng) {
        // Two range(256) draws from the game's RNG, byte 1 then byte 6 (spec_result_ending 1.6),
        // even when nothing is written (no storage).
        seed = static_cast<std::uint8_t>(rng_.u16InRange(256));
        filler = static_cast<std::uint8_t>(rng_.u16InRange(256));
    } else {
        std::minstd_rand bytesRng(static_cast<std::uint32_t>(ticks_ * 2654435761u + scores_.totalPlays + 1));
        seed = static_cast<std::uint8_t>(bytesRng() & 0xFF);
        filler = static_cast<std::uint8_t>(bytesRng() & 0xFF);
    }
    if (config_.slowMode != 0) scores_.cards = savedCards_;
    savedCards_ = scores_.cards;
    if (storage_.empty()) return;
    std::vector<std::uint8_t> image = scores_.imageToSave();
    const auto bytes = encodeScoreFile(image, seed, filler);
    std::ofstream f(storage_ + "score.dat", std::ios::binary | std::ios::trunc);
    if (!f) {
        log::warn("score.dat could not be written");
        return;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    // The original keeps only what it wrote (the first 10 nodes of each high-score list).
    if (const auto decoded = decodeScoreFile(bytes)) {
        ScoreData reread;
        if (parseScoreImage(*decoded, reread)) {
            scores_.image = std::move(reread.image);
            scores_.highScores = std::move(reread.highScores);
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
    if (to == Scene::Ending) {
        // The ending (scene 9): the run's records first, then its script.
        gameplay_.reset();
        ending_ = std::make_unique<Ending>(flow_);
        Ending::Run run;
        run.character = lastGame_.character;
        run.difficulty = lastGame_.difficulty;
        run.stage = lastGame_.stage;
        run.finished = lastGame_.finished;
        run.continued = lastGame_.continues > 0;
        if (ending_->create(run, &scores_, &err)) return;
        log::warn("The ending could not start: {}", err);
        ending_.reset();
        active_ = requested_ = Scene::ResultAfterGame;  // (create() asked for it)
        applyScene();
        return;
    }
    if (to == Scene::ResultViewer) {
        // Player Data (scene 5).
        menu_.reset();
        result_ = std::make_unique<ResultScreen>(flow_, ResultScreen::Mode::Viewer, GameState{}, scores_);
        if (result_->create(&err)) return;
        log::warn("Player Data could not be built: {}", err);
        result_.reset();
        active_ = requested_ = Scene::MainMenu;
    }
    if (to == Scene::ResultAfterGame) {
        // The result screen (scene 6) after a game, a practice or a spell practice.
        gameplay_.reset();
        ending_.reset();
        const ResultScreen::Mode mode = lastGame_.spellPractice ? ResultScreen::Mode::SpellPracticeEnd
                                        : lastGame_.practice    ? ResultScreen::Mode::PracticeEnd
                                                                : ResultScreen::Mode::AfterGame;
        result_ = std::make_unique<ResultScreen>(flow_, mode, lastGame_, scores_);
        if (result_->create(&err)) return;
        log::warn("The result screen could not be built: {}", err);
        result_.reset();
        saveScores(true);
        active_ = requested_ = Scene::MainMenu;
    }
    if (to == Scene::Game) {
        menu_.reset();
        musicRoom_.reset();
        result_.reset();
        ending_.reset();
        gameplay_ = std::make_unique<Gameplay>(flow_);
        Gameplay::Setup setup;
        setup.stage = flow_.stage;
        setup.character = flow_.character;
        setup.difficulty = config_.difficulty;
        setup.practice = flow_.practice;
        setup.spellPractice = flow_.spellPractice;
        if (setup.spellPractice) {  // (the card's difficulty, not the remembered one)
            setup.card = flow_.card;
            setup.difficulty = flow_.spellDifficulty;
        }
        setup.lives = config_.lives;
        // Developer aid: TH08_DEV_STAGE=<stage index 0-8> starts that stage instead.
        if (const char* dev = std::getenv("TH08_DEV_STAGE"); dev && *dev) setup.stage = std::clamp(std::atoi(dev), 0, kStageCount - 1);
        // TH08_DEV_PRACTICE=1 makes it a practice game (eight lives).
        if (const char* dev = std::getenv("TH08_DEV_PRACTICE"); dev && *dev == '1') setup.practice = true;
        // TH08_DEV_CHARACTER=<0-11> plays that character (0-3 teams, 4-11 solos).
        if (const char* dev = std::getenv("TH08_DEV_CHARACTER"); dev && *dev) setup.character = std::clamp(std::atoi(dev), 0, 11);
        // TH08_DEV_CARD=<0-221> plays that card in Spell Card Practice.
        if (const char* dev = std::getenv("TH08_DEV_CARD"); dev && *dev && tablesLoaded_) {
            const int card = std::clamp(std::atoi(dev), 0, 221);
            setup.spellPractice = true;
            setup.card = card;
            const int d = MainMenu::cardDifficulty(&tables_, card);
            setup.difficulty = d == 5 ? 1 : d;
            for (int s = 0; s < 9; ++s) {
                const auto& ids = tables_.stageCards[static_cast<std::size_t>(s)];
                if (std::find(ids.begin(), ids.end(), card) != ids.end()) setup.stage = s;
            }
            if (d == 5) setup.stage = MainMenu::lastWordStage(card);
            flow_.practice = flow_.spellPractice = true;
            flow_.card = card;
        }
        // A playback (spec_replays 3.1): the file read again and validated.
        if (flow_.replayFlag) {
            std::string why;
            const auto bytes = flow_.readReplay ? flow_.readReplay(flow_.replayPath) : std::vector<std::uint8_t>{};
            auto file = ReplayFile::parse(bytes, &why);
            if (!file || !file->hasStage(flow_.replayStage)) {
                log::warn("th08: {} cannot be played back: {}", flow_.replayPath, why.empty() ? "no such stage" : why);
                gameplay_.reset();
                active_ = requested_ = Scene::MainMenu;
                applyScene();
                return;
            }
            playback_ = std::move(*file);
            setup = Gameplay::Setup{};
            setup.replay = &playback_;
            setup.stage = flow_.replayStage;
            setup.playbackMode = flow_.replayMode;
            setup.demo = flow_.demo;
            setup.demoIndex = flow_.demoIndex;
        }
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
    if (to == Scene::ReplayEnd) active_ = requested_ = Scene::MainMenu;  // (scene 7: the title, which opens the replay menu)
    if (active_ == Scene::MainMenu) {
        musicRoom_.reset();
        gameplay_.reset();
        ending_.reset();
        result_.reset();
        refreshUnlocks();  // (every title creation reads the clear records again)
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
            savedCards_ = scores_.cards;
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
    flow_.records = &scores_;
    flow_.saveScores = [this](bool gameRng) { saveScores(gameRng); };
    flow_.replaySlot = [this](int slot) -> std::optional<std::array<std::uint8_t, 0xCC>> {
        if (storage_.empty()) return std::nullopt;
        const auto bytes = readFile(storage_ + std::format("replay/th8_{:02d}.rpy", slot));
        if (bytes.empty()) return std::nullopt;
        const auto r = ReplayFile::parse(bytes, nullptr, true);
        if (!r) return std::nullopt;
        return r->summary.bytes;
    };
    flow_.saveReplay = [this](int slot, const std::string& name) { return saveReplay(std::format("replay/th8_{:02d}.rpy", slot), name); };
    flow_.replayFiles = [this]() {
        // replay/th8_01.rpy .. th8_15.rpy, then up to 45 th8_ud????.rpy (FLOW 4.8).
        std::vector<std::string> out;
        if (storage_.empty()) return out;
        for (int i = 1; i <= kReplaySlots; ++i) {
            const std::string p = std::format("replay/th8_{:02d}.rpy", i);
            std::error_code ec;
            if (std::filesystem::exists(storage_ + p, ec)) out.push_back(p);
        }
        std::error_code ec;
        std::vector<std::string> user;
        for (const auto& e : std::filesystem::directory_iterator(storage_ + "replay", ec)) {
            const std::string n = e.path().filename().string();
            // th8_ud????.rpy, with Windows' rule that a '?' before the dot may match nothing
            if (n.size() >= 10 && n.size() <= 14 && n.rfind("th8_ud", 0) == 0 && n.ends_with(".rpy") &&
                n.find('.') == n.size() - 4) {
                user.push_back("replay/" + n);
            }
        }
        std::sort(user.begin(), user.end());
        for (std::size_t i = 0; i < user.size() && i < static_cast<std::size_t>(kReplayUserFiles); ++i) out.push_back(user[i]);
        return out;
    };
    flow_.readReplay = [this](const std::string& path) -> std::vector<std::uint8_t> {
        if (flow_.demo && flow_.vfs) {  // the attract demo: from the archive
            if (const auto b = flow_.vfs->read(path)) return *b;
            return {};
        }
        return storage_.empty() ? std::vector<std::uint8_t>{} : readFile(storage_ + path);
    };
    refreshUnlocks();
    menu_ = std::make_unique<MainMenu>(flow_);
    if (!menu_->create(&err)) {
        log::warn("The title could not be built: {}", err);
        menu_.reset();
    }
    // Developer aid: TH08_DEV_SCENE=result|ending|badending starts on that scene with a made-up
    // run (team TH08_DEV_CHARACTER, Normal), to look at those screens.
    // Developer aid: TH08_DEV_DEMO=1 lets the title start the attract demo when left idle.
    if (const char* d = std::getenv("TH08_DEV_DEMO"); d && *d == '1') flow_.attractDemo = true;
    if (const char* dev = std::getenv("TH08_DEV_SCENE"); dev && *dev) {
        const std::string v(dev);
        lastGame_ = GameState{};
        if (const char* c = std::getenv("TH08_DEV_CHARACTER"); c && *c) lastGame_.character = std::clamp(std::atoi(c), 0, 11);
        lastGame_.stage = v == "badending" ? 5 : 6;
        lastGame_.finished = v == "ending";
        lastGame_.score = lastGame_.guiScore = 123456789;
        lastGame_.runFrames = 216000;
        requested_ = v == "result" ? Scene::ResultAfterGame : Scene::Ending;
    }
    return true;
}

std::string Th08Game::replayUserText(const std::string& name) const {
    if (!tablesLoaded_) return {};
    const ExeTables::UserText& u = tables_.userText;
    const GameState& g = lastGame_;
    std::string t;
    char when[32] = {};
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(when, sizeof when, u.timeFormat.c_str(), &tm);
    t += sjisPrintf(u.name.c_str(), name.c_str());
    t += sjisPrintf(u.time.c_str(), when);
    t += sjisPrintf(u.character.c_str(), tables_.fullNames[static_cast<std::size_t>(std::clamp(g.character, 0, 11))].c_str());
    t += sjisPrintf(u.score.c_str(), static_cast<int>(g.guiScore));
    t += sjisPrintf(u.difficulty.c_str(), u.difficulties[static_cast<std::size_t>(std::clamp(g.difficulty, 0, 4))].c_str());
    if (g.spellPractice && g.card >= 0) {
        t += sjisPrintf(u.card.c_str(), g.card + 1, lastReplay_.summary.cardName().c_str());
    } else {
        t += sjisPrintf(u.lastStage.c_str(), g.finished || g.stage >= 9 ? u.clear.c_str() : u.stages[static_cast<std::size_t>(g.stage)].c_str());
    }
    t += sjisPrintf(u.misses.c_str(), static_cast<int>(g.misses));
    t += sjisPrintf(u.bombs.c_str(), static_cast<int>(g.bombsUsedGame));
    t += sjisPrintf(u.slowdown.c_str(), 0.0);
    const int h = g.countedFrames != 0 ? static_cast<int>(static_cast<float>(g.humanFrames) / static_cast<float>(g.countedFrames) * 10000.0f) : 0;
    t += sjisPrintf(u.humanRate.c_str(), static_cast<double>(h) / 100.0);
    t += sjisPrintf(u.version.c_str(), 1, 0, 'd');
    return t;
}

bool Th08Game::saveReplay(const std::string& path, const std::string& name) {
    if (!flow_.replayToSave || storage_.empty()) return false;
    ReplayFile r = lastReplay_;
    if (r.summary.slowModeRecorded()) return false;
    // Step 6: the stage closed again, its end pointer 6 bytes on (two unwritten entries); the
    // final score into its block.
    const int s = std::clamp(lastGame_.stage, 0, kReplayStages - 1);
    if (r.hasStage(s)) {
        ReplayStage& b = r.stages[static_cast<std::size_t>(s)];
        b.entries.insert(b.entries.end(), {0, 0, 0});
        b.carriedScore = lastGame_.score;
    }
    // Step 7: the summary.
    ReplaySummary& sm = r.summary;
    sm.setScore(lastGame_.guiScore);
    if (lastGame_.spellPractice && lastGame_.card >= 0 && lastGame_.card < kCardCount) {
        sm.setCardName(scores_.cards[static_cast<std::size_t>(lastGame_.card)].name);
    }
    sm.setSlowdown(0.0f);
    sm.setName(name);
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        sm.setDate(std::format("{:02d}/{:02d}", tm.tm_mon + 1, tm.tm_mday));
    }
    const std::vector<std::uint8_t> bytes = r.serialize(rng_, replayUserText(name));
    std::error_code ec;
    std::filesystem::create_directories(storage_ + "replay", ec);
    std::ofstream f(storage_ + path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    log::info("th08: replay written to {} ({} bytes)", path, bytes.size());
    return static_cast<bool>(f);
}

void Th08Game::refreshUnlocks() {
    flow_.extraUnlocked = scores_.extraUnlocked();
    flow_.spellPracticeUnlocked = scores_.spellPracticeUnlocked();
    flow_.solosUnlocked = scores_.solosUnlocked();
    flow_.totalPlays = static_cast<int>(scores_.totalPlays);
}

void Th08Game::tick(const InputState& input) {
    ++ticks_;
    // The PLST times (running, and play while a game runs), in milliseconds of 60 Hz ticks.
    if (anyScene()) {
        const auto ms = static_cast<std::uint32_t>(ticks_ * 1000 / 60 - (ticks_ - 1) * 1000 / 60);
        scores_.addTime(ScoreData::kRunningTime, ms);
        if (gameplay_) scores_.addTime(ScoreData::kPlayTime, ms);
    }
    if (music_) music_->fadeStep();
    if (anyScene()) {
        input_.sample(static_cast<std::uint16_t>(buttonWord(input) | injected_));
        injected_ = 0;
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
            if (devHold && !gameplay_->menuOpen()) {  // (not in the pause / continue menu)
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
            // Playback speed: whole extra passes in the same frame (input re-sampled: no
            // second press).
            for (int k = 0; k < 8 && gameplay_->runAgain(); ++k) {
                input_.sample(input_.current());
                gameplay_->tick(input_);
            }
            // A demo ends on any new press (spec_replays 5.2).
            if (flow_.demo && gameplay_->replaying() && input_.current() != 0 && input_.pressed(0xFFFF)) {
                requested_ = Scene::MainMenu;
            }
            const auto outcome = gameplay_->outcome();
            if (outcome == Gameplay::Outcome::Ending || outcome == Gameplay::Outcome::Results) {
                // The run's recording goes on to the result screen's prompt (the GameManager's
                // removal closes the stage being played, spec_replays 2.3).
                lastGame_ = gameplay_->state();
                flow_.replayToSave = gameplay_->recording() && !gameplay_->replaying();
                if (flow_.replayToSave) {
                    gameplay_->closeRecording();
                    lastReplay_ = gameplay_->recorded();
                }
            }
            switch (outcome) {
                case Gameplay::Outcome::Title:
                    flow_.replayToSave = false;  // (a quit drops the recording)
                    requested_ = Scene::MainMenu;
                    break;
                case Gameplay::Outcome::ReplayEnd: requested_ = Scene::ReplayEnd; break;
                case Gameplay::Outcome::Ending: requested_ = Scene::Ending; break;
                case Gameplay::Outcome::Results: requested_ = Scene::ResultAfterGame; break;
                case Gameplay::Outcome::None: break;
            }
        } else if (ending_) {
            ending_->update(input_);
        } else if (result_) {
            result_->update(input_);
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
    if (ending_) {
        ending_->draw(sink);
        return;
    }
    if (result_) {
        result_->draw(sink);
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

// Android Back: in a game, what Menu does (the pause menu); elsewhere Menu (cancel).
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
    if (gameplay_) {
        gameplay_->requestPause();
        return true;
    }
    // Elsewhere Back is Menu (cancel) for one pass; on the title with the cursor on Quit it
    // leaves the app.
    if (!anyScene() || (menu_ && menu_->state() == MainMenu::State::Title && menu_->cursor() == MainMenu::Quit)) return false;
    injected_ |= BtnMenu;
    return true;
}

void Th08Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | {}", frame.fps, frame.ticks, gameplay_ ? std::format("stage index {} frame {}", gameplay_->state().stage, gameplay_->stageFrame())
                         : ending_ ? std::string("ending") : result_ ? std::string("result screen")
                         : musicRoom_ ? std::string("Music Room") : menu_ ? std::string("title") : std::string("status screen"));
}

}  // namespace touhou::th08
