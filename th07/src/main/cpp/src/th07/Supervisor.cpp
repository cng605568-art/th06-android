#include "th07/Supervisor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "core/Log.hpp"
#include "io/Vfs.hpp"
#include "th07/Bgm.hpp"
#include "th07/Ending.hpp"
#include "th07/MainMenu.hpp"
#include "th07/MusicRoom.hpp"
#include "th07/Player.hpp"
#include "th07/ResultScreen.hpp"
#include "th07/SoundPlayer.hpp"

namespace touhou::th07 {

namespace {

std::vector<std::uint8_t> readWholeFile(const std::string& path, bool* exists) {
    std::vector<std::uint8_t> bytes;
    *exists = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return bytes;
    *exists = true;
    std::uint8_t buf[8192];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return bytes;
}

// Written beside the target first, then renamed over it, so a crash never leaves half a file.
bool writeWholeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!ok) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

}  // namespace

Supervisor::Supervisor() = default;

Supervisor::~Supervisor() {
    // The screens release their ANM slots while the library still exists.
    menu_.reset();
    result_.reset();
    ending_.reset();
    musicRoom_.reset();
    if (game_) game_->end();
}

std::uint64_t Supervisor::now() const { return boot_.clockMs ? boot_.clockMs() : 0; }

bool Supervisor::start(const Boot& boot) {
    boot_ = boot;
    if (!boot_.clockMs) {
        boot_.clockMs = []() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    }
    ctx_.vfs = boot_.vfs;
    ctx_.textures = boot_.textures;
    ctx_.text = boot_.text;
    ctx_.ui = boot_.ui;
    ctx_.anm = boot_.anm;
    ctx_.rng = boot_.rng;
    ctx_.sound = boot_.sound;
    ctx_.music = boot_.music;
    ctx_.tables = boot_.tables;
    ctx_.config = &config_;
    ctx_.scores = &scores_;
    ctx_.request = [this](FlowState s) { requested_ = s; };
    ctx_.readScores = [this]() { return readScoreFile(); };
    ctx_.writeScores = [this](const std::array<ScoreList, kDifficulties * kShots>& lists, const std::array<char, 12>& name,
                              const std::array<std::uint8_t, 0x1C>* header) { writeScores(lists, name, header); };
    ctx_.saveConfig = [this]() { saveConfig(); };
    ctx_.refreshRunningTime = [this]() { refreshRunningTime(); };

    // th07.cfg: read, rebuilt if invalid, written back at once.
    std::vector<std::uint8_t> cfg;
    if (!boot_.storageDir.empty()) {
        bool exists = false;
        cfg = readWholeFile(boot_.storageDir + "th07.cfg", &exists);
    }
    if (!config_.load(cfg, boot_.thbgm) && !cfg.empty()) log::info("th07.cfg was not valid: rebuilt with the defaults");
    saveConfig();
    if (boot_.music) boot_.music->setMode(config_.music);
    if (boot_.sound) boot_.sound->setEnabled(config_.sound != 0);

    // text.anm, ascii.anm and capture.anm live for the whole session.
    if (boot_.vfs && boot_.anm && boot_.textures) {
        std::string err;
        if (!boot_.anm->loaded(0) && !boot_.anm->load(*boot_.vfs, *boot_.textures, "data/text.anm", 0, 0x700, &err)) log::warn("{}", err);
        if (!boot_.anm->loaded(1) && !boot_.anm->load(*boot_.vfs, *boot_.textures, "data/ascii.anm", 1, 0x000, &err)) log::warn("{}", err);
        if (!boot_.anm->loaded(4) && !boot_.anm->load(*boot_.vfs, *boot_.textures, "data/capture.anm", 4, 0x724, &err)) log::warn("{}", err);
    }

    runningBase_ = playBase_ = now();
    scores_.stats = PlayStats{};
    if (const auto img = readScoreFile()) ScoreFile::readPlayStats(*img, scores_.stats);
    statsReady_ = true;
    started_ = true;
    // The first pass always performs the Init transition (to the title).
    active_ = previous_ = FlowState::Init;
    requested_ = FlowState::MainMenu;
    return true;
}

// ---- files ----------------------------------------------------------------------------------

std::optional<ScoreImage> Supervisor::readScoreFile() {
    std::vector<std::uint8_t> bytes;
    bool exists = false;
    if (!boot_.storageDir.empty()) bytes = readWholeFile(boot_.storageDir + "score.dat", &exists);
    // The game folder's score.dat (the player's PC file) until the port has written its own.
    if (!exists && boot_.vfs) {
        const auto pc = boot_.vfs->looseFile("score.dat");
        bytes.assign(pc.begin(), pc.end());
    }
    if (bytes.empty()) return std::nullopt;
    auto img = ScoreFile::decode(bytes, boot_.verFile);
    if (!img) {
        static bool warned = false;
        if (!warned) log::warn("score.dat is not valid: ignored");
        warned = true;
    }
    return img;
}

void Supervisor::writeScores(const std::array<ScoreList, kDifficulties * kShots>& lists, const std::array<char, 12>& lastName,
                             const std::array<std::uint8_t, 0x1C>* header) {
    refreshRunningTime();
    ScoreFile::WriteInput in;
    in.lists = &lists;
    in.data = &scores_;
    in.lastName = lastName;
    in.exe = boot_.exe;
    in.previousHeader = header;
    const std::vector<std::uint8_t> bytes = ScoreFile::encode(in, *boot_.rng);
    ++writes_;
    if (boot_.storageDir.empty()) return;
    const std::string path = boot_.storageDir + "score.dat";
    if (!writeWholeFile(path, bytes)) log::warn("Could not write {}", path);
}

void Supervisor::rewriteScoreFile() {
    const auto img = readScoreFile();
    std::array<ScoreList, kDifficulties * kShots> lists;
    scores_.phantasmCheck();
    ScoreFile::defaultLists(lists);
    if (img) ScoreFile::readHighScores(*img, lists);
    std::array<char, 12> name{};
    ScoreFile::readLastName(img ? &*img : nullptr, name);
    if (img) ScoreFile::readSpells(*img, scores_.spells);
    ScoreFile::readClears(img ? &*img : nullptr, scores_.clears);
    scores_.phantasmCheck();
    ScoreFile::readPractice(img ? &*img : nullptr, scores_);
    writeScores(lists, name, img ? &img->header : nullptr);
}

void Supervisor::saveConfig() {
    if (boot_.storageDir.empty()) return;
    const std::string path = boot_.storageDir + "th07.cfg";
    if (!writeWholeFile(path, config_.save())) log::warn("Could not write {}", path);
}

// ---- clocks ---------------------------------------------------------------------------------

void Supervisor::refreshRunningTime() {
    const std::uint64_t t = now();
    scores_.stats.running.add(t >= runningBase_ ? t - runningBase_ : t);
    runningBase_ = t;
}

void Supervisor::playTimeAdd() {
    const std::uint64_t t = now();
    scores_.stats.play.add(t >= playBase_ ? t - playBase_ : t);
    playBase_ = t;
}

void Supervisor::playTimeRebase() { playBase_ = now(); }

// ---- the pass -------------------------------------------------------------------------------

void Supervisor::pass(std::uint16_t raw) {
    if (!started_ || quit_) return;
    input_.sample(static_cast<std::uint16_t>(raw | injected_));
    injected_ = 0;
    if (requested_ != active_) {
        previous_ = active_;
        ctx_.previous = previous_;
        transition(active_);
        input_.clear();  // nothing is held for the rest of this pass
    }
    active_ = requested_;
    ++passes_;
    switch (active_) {
        case FlowState::Exit:
        case FlowState::ExitError: quit_ = true; break;
        case FlowState::MainMenu:
            if (menu_) {
                menu_->update(input_);
                if (menu_->done()) menu_.reset();
            }
            break;
        case FlowState::Game:
            if (game_ && game_->running()) {
                game_->tick(static_cast<std::uint16_t>(input_.current() | (game_->paused() ? 0 : gameExtras_)));
                if (game_->nextStagePending()) requested_ = FlowState::NextStage;
                switch (game_->outcome()) {
                    case Gameplay::Outcome::Title: requested_ = FlowState::MainMenu; break;
                    case Gameplay::Outcome::Restart: requested_ = FlowState::RestartGame; break;
                    case Gameplay::Outcome::Results: requested_ = FlowState::ResultAfterGame; break;
                    case Gameplay::Outcome::Ending: requested_ = FlowState::Ending; break;
                    case Gameplay::Outcome::None: break;
                }
            } else {
                requested_ = FlowState::MainMenu;
            }
            break;
        case FlowState::ResultViewer:
        case FlowState::ResultAfterGame:
            if (result_) {
                result_->update(input_);
                if (result_->done()) {
                    result_->finish();
                    result_.reset();
                }
            } else {
                requested_ = FlowState::MainMenu;
            }
            break;
        case FlowState::MusicRoom:
            if (musicRoom_) {
                musicRoom_->update(input_);
                if (musicRoom_->done()) musicRoom_.reset();
            } else {
                requested_ = FlowState::MainMenu;
            }
            break;
        case FlowState::Ending:
            if (ending_) {
                ending_->update(input_);
                if (ending_->done()) ending_.reset();
            } else {
                requested_ = FlowState::ResultAfterGame;
            }
            break;
        default: break;
    }
}

void Supervisor::transition(FlowState from) {
    const FlowState to = requested_;
    if (to == FlowState::Exit || to == FlowState::ExitError) {
        quit_ = true;
        return;
    }
    switch (from) {
        case FlowState::Init:
            requested_ = FlowState::MainMenu;
            openMainMenu();
            break;
        case FlowState::MainMenu:
            menu_.reset();
            if (to == FlowState::Game) {
                openGame(true, false);
            } else if (to == FlowState::ResultViewer) {
                openResult(0);
            } else if (to == FlowState::MusicRoom) {
                musicRoom_ = std::make_unique<MusicRoom>(ctx_);
                std::string err;
                if (!musicRoom_->create(&err)) {
                    log::warn("Music Room: {}", err);
                    musicRoom_.reset();
                }
            } else if (to == FlowState::Ending) {
                cutGame();
                openEnding();
            }
            break;
        case FlowState::Game: {
            const bool live = game_ && game_->running();
            switch (to) {
                case FlowState::MainMenu:
                    cutGame();
                    openMainMenu();
                    break;
                case FlowState::ResultAfterGame: {
                    if (live) {
                        // Extra and Phantasm clears (the next-level event): Extra unlocks its
                        // part of Phantasm, both count a clear.
                        const GameState& g = game_->state();
                        if (g.finished && g.difficulty >= Extra && !g.replay) {
                            if (g.difficulty == Extra) scores_.clears[static_cast<std::size_t>(std::clamp(g.shotIndex, 0, 5))].noContinue[Extra] = kCleared;
                            scores_.stats.countClear(g.difficulty);
                        }
                    }
                    const bool practice = live && game_->state().practice;
                    cutGame();
                    openResult(practice ? 2 : 1);
                    break;
                }
                case FlowState::RestartGame:
                case FlowState::RestartStage:
                    cutGame();
                    requested_ = FlowState::Game;
                    openGame(true, true);
                    break;
                case FlowState::NextStage:
                case FlowState::NextStageVariant:
                    requested_ = FlowState::Game;
                    if (live) {
                        std::string err;
                        if (!game_->nextStage(&err)) {
                            log::warn("Next stage: {}", err);
                            cutGame();
                            requested_ = FlowState::MainMenu;
                            openMainMenu();
                        }
                    }
                    break;
                case FlowState::ReplayEnd:
                    cutGame();
                    requested_ = FlowState::MainMenu;
                    openMainMenu();
                    break;
                case FlowState::Ending:
                    if (live) scores_.stats.countClear(game_->state().difficulty);
                    cutGame();
                    openEnding();
                    break;
                default: break;
            }
            break;
        }
        case FlowState::ResultViewer:
        case FlowState::ResultAfterGame:
        case FlowState::MusicRoom:
            closeScreens();
            if (to == FlowState::MainMenu) openMainMenu();
            break;
        case FlowState::Ending:
            closeScreens();
            if (to == FlowState::MainMenu) openMainMenu();
            else if (to == FlowState::ResultAfterGame) openResult(1);
            break;
        default: break;
    }
}

void Supervisor::closeScreens() {
    menu_.reset();
    if (result_) result_->finish();
    result_.reset();
    ending_.reset();
    musicRoom_.reset();
}

void Supervisor::openMainMenu() {
    closeScreens();
    menu_ = std::make_unique<MainMenu>(ctx_);
    std::string err;
    if (!menu_->create(&err)) {
        log::warn("Title: {}", err);
        menu_.reset();
    }
}

void Supervisor::openResult(int mode) {
    result_ = std::make_unique<ResultScreen>(ctx_, mode == 0 ? ResultScreen::Mode::Viewer
                                                   : mode == 1 ? ResultScreen::Mode::AfterGame
                                                               : ResultScreen::Mode::PracticeEnd);
    std::string err;
    if (!result_->create(&err)) {
        log::warn("Result screen: {}", err);
        result_.reset();
    }
}

void Supervisor::openEnding() {
    const GameState& g = ctx_.lastGame;
    ending_ = std::make_unique<Ending>(ctx_);
    std::string err;
    if (!ending_->create(g.shotIndex, g.difficulty, g.continues > 0, &err)) {
        log::warn("Ending: {}", err);
        ending_.reset();
        requested_ = FlowState::Ending;  // the next pass goes on to the result screen
    }
}

bool Supervisor::openGame(bool fresh, bool restart) {
    (void)fresh;
    Gameplay::Setup s;
    if (restart) {
        s = setup_;
        if (!s.practice && s.difficulty < Extra) s.stage = 1;
        s.restart = true;
    } else {
        s.stage = ctx_.stage + 1;
        s.shotIndex = ctx_.shotIndex();
        s.difficulty = config_.difficulty;
        s.practice = ctx_.practice;
        s.lifeCount = config_.lives;
        s.slowMode = config_.slowMode != 0;
        s.restart = false;
    }
    s.maxContinues = ctx_.maxContinues;
    setup_ = s;
    if (!boot_.canPlay || !boot_.vfs) {
        log::warn("Cannot start a game: th07.exe 1.00b or the 1.00b th07.dat is missing");
        requested_ = FlowState::MainMenu;
        openMainMenu();
        return false;
    }
    if (!game_) game_ = std::make_unique<Gameplay>();
    gameHooks(*game_);
    Gameplay::Resources res;
    res.vfs = boot_.vfs;
    res.textures = boot_.textures;
    res.anm = boot_.anm;
    res.rng = boot_.rng;
    res.tables = boot_.tables;
    res.sound = boot_.sound;
    res.music = boot_.music;
    res.text = boot_.text;
    res.ui = boot_.ui;
    std::string err;
    if (!game_->start(res, s, &err)) {
        log::warn("Could not start stage {}: {}", s.stage, err);
        game_->end();
        requested_ = FlowState::MainMenu;
        openMainMenu();
        return false;
    }
    return true;
}

void Supervisor::cutGame() {
    if (!game_ || !game_->running()) return;
    GameState& g = game_->state();
    g.score = std::min<std::uint32_t>(g.score, 999999999u);
    g.guiScore = g.score;
    game_->end();
    if (!g.replay) playTimeAdd();
    playTimeRebase();
    refreshRunningTime();
    ctx_.lastGame = g;
    ctx_.lastValid = true;
    ctx_.lastSlow = 0.0f;  // the port's timing is exact: no slowdown is measured
    ctx_.stage = g.stage;
    ctx_.practice = g.practice;
    ctx_.character = std::clamp(g.shotIndex, 0, 5) / 2;
    ctx_.shotType = g.shotIndex & 1;
}

void Supervisor::gameHooks(Gameplay& game) {
    Gameplay::Hooks h;
    h.readScores = [this](GameState& gs) {
        rewriteScoreFile();  // the parse-only pass: score.dat rewritten (two RNG draws)
        const auto img = readScoreFile();
        auto hi = ScoreFile::hiScore(img ? &*img : nullptr, gs.difficulty, gs.shotIndex);
        scores_.resetSpells();
        if (img) ScoreFile::readSpells(*img, scores_.spells);
        ScoreFile::readClears(img ? &*img : nullptr, scores_.clears);
        ScoreFile::readPractice(img ? &*img : nullptr, scores_);
        ctx_.spellSnapshot = scores_.spells;
        if (gs.practice && gs.difficulty < Extra) {
            PracticeRecord& r = scores_.practiceAt(std::clamp(gs.shotIndex, 0, 5), std::clamp(gs.stage, 0, 5), gs.difficulty);
            hi = {static_cast<std::uint32_t>(std::max(r.score, 0)), 0};
            if (!gs.replay) ++r.plays;
        }
        gs.hiScore = hi.first;
        gs.hiScoreContinues = hi.second;
    };
    h.countPlay = [this](const GameState& gs, bool restart) {
        scores_.stats.countPlay(gs.difficulty, gs.shotIndex);
        if (restart) scores_.stats.countRetry(gs.difficulty);
        if (gs.practice) scores_.stats.countPractice(gs.difficulty);
    };
    h.stageEntered = [this](const GameState& gs) { scores_.stageEntered(gs.shotIndex, gs.difficulty, gs.stage, gs.continues > 0); };
    Gameplay* g = &game;
    h.spellDeclared = [this, g](int card, const std::string& name) { scores_.spellDeclared(card, g->state().shotIndex, name); };
    h.spellCaptured = [this, g](int card, std::uint32_t bonus) { scores_.spellCaptured(card, g->state().shotIndex, bonus); };
    h.continued = [this](const GameState& gs) {
        scores_.stats.countPlay(gs.difficulty, gs.shotIndex);
        scores_.stats.countContinue(gs.difficulty);
    };
    h.playTimeAdd = [this]() { playTimeAdd(); };
    h.playTimeRebase = [this]() { playTimeRebase(); };
    game.setHooks(std::move(h));
}

bool Supervisor::startGame(const Gameplay::Setup& setup) {
    if (!started_) return false;
    ctx_.character = std::clamp(setup.shotIndex, 0, 5) / 2;
    ctx_.shotType = setup.shotIndex & 1;
    ctx_.practice = setup.practice;
    ctx_.stage = setup.stage - 1;
    previous_ = ctx_.previous = FlowState::Init;
    setup_ = setup;
    setup_.restart = false;
    // (The maximum continues come from the play time, as the title would set them.)
    const std::uint32_t hours = scores_.stats.play.hours;
    ctx_.maxContinues = hours < 7 ? 3 : hours < 14 ? 4 : 5;
    if (!boot_.canPlay || !boot_.vfs) return false;
    if (!game_) game_ = std::make_unique<Gameplay>();
    gameHooks(*game_);
    Gameplay::Setup s = setup_;
    s.maxContinues = ctx_.maxContinues;
    setup_ = s;
    Gameplay::Resources res;
    res.vfs = boot_.vfs;
    res.textures = boot_.textures;
    res.anm = boot_.anm;
    res.rng = boot_.rng;
    res.tables = boot_.tables;
    res.sound = boot_.sound;
    res.music = boot_.music;
    res.text = boot_.text;
    res.ui = boot_.ui;
    std::string err;
    if (!game_->start(res, s, &err)) {
        log::warn("Could not start stage {}: {}", s.stage, err);
        game_->end();
        return false;
    }
    active_ = requested_ = FlowState::Game;
    return true;
}

void Supervisor::startEnding(int shot, int difficulty, bool continued) {
    if (!started_) return;
    GameState g;
    g.shotIndex = std::clamp(shot, 0, 5);
    g.difficulty = std::clamp(difficulty, 0, 3);
    g.continues = continued ? 1 : 0;
    g.stage = 6;
    g.finished = true;
    ctx_.lastGame = g;
    ctx_.lastValid = true;
    ctx_.character = g.shotIndex / 2;
    ctx_.shotType = g.shotIndex & 1;
    previous_ = ctx_.previous = FlowState::Game;
    active_ = requested_ = FlowState::Ending;
    openEnding();
}

void Supervisor::draw(SpriteSink& sink) {
    switch (active_) {
        case FlowState::MainMenu:
            if (menu_) menu_->draw(sink);
            break;
        case FlowState::Game:
            if (game_ && game_->running()) game_->draw(sink);
            break;
        case FlowState::ResultViewer:
        case FlowState::ResultAfterGame:
            if (result_) result_->draw(sink);
            break;
        case FlowState::MusicRoom:
            if (musicRoom_) musicRoom_->draw(sink);
            break;
        case FlowState::Ending:
            if (ending_) ending_->draw(sink);
            break;
        default: break;
    }
}

void Supervisor::shutdown() {
    if (shutDown_ || !started_) return;
    shutDown_ = true;
    if (statsReady_) rewriteScoreFile();
    if (result_) result_->finish();
    if (game_ && game_->running()) cutGame();
    saveConfig();
}

void Supervisor::pauseNow() {
    if (inGame() && !game_->paused()) game_->requestPause();
}

bool Supervisor::back() {
    if (inGame()) {
        if (!game_->paused()) game_->requestPause();
        else injected_ |= BtnMenu;
        return true;
    }
    if (active_ == FlowState::MainMenu && (!menu_ || menu_->atQuit())) return false;
    injected_ |= BtnMenu;
    return true;
}

}  // namespace touhou::th07
