#include "game/Stage.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <span>

#include "audio/MusicPlayer.hpp"
#include "audio/SoundPlayer.hpp"
#include "core/Log.hpp"
#include "game/Resources.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

constexpr const char* kEclFiles[7] = {"data/ecldata1.ecl", "data/ecldata2.ecl", "data/ecldata3.ecl", "data/ecldata4.ecl",
                                      "data/ecldata5.ecl", "data/ecldata6.ecl", "data/ecldata7.ecl"};
constexpr const char* kEnemyAnm[7][2] = {
    {"data/stg1enm.anm", "data/stg1enm2.anm"}, {"data/stg2enm.anm", "data/stg2enm2.anm"}, {"data/stg3enm.anm", nullptr},
    {"data/stg4enm.anm", nullptr},             {"data/stg5enm.anm", "data/stg5enm2.anm"}, {"data/stg6enm.anm", "data/stg6enm2.anm"},
    {"data/stg7enm.anm", "data/stg7enm2.anm"},
};
// Stage 6 reuses stage 5's effects and the Extra stage stage 4's.
constexpr const char* kEffectAnm[7] = {"data/eff01.anm", "data/eff02.anm", "data/eff03.anm", "data/eff04.anm",
                                       "data/eff05.anm", "data/eff05.anm", "data/eff04.anm"};
constexpr const char* kDifficultyNames[5] = {"EASY", "NORMAL", "HARD", "LUNATIC", "EXTRA"};
constexpr int kClearDelay = 180;

}  // namespace

Stage::Stage(TextureLibrary& textures, const UiArt& ui, const Vfs* vfs, SoundPlayer* sound, TextRenderer* text)
    : sound_(sound),
      vfs_(vfs),
      ui_(ui),
      rng_(0),
      anm_(textures, rng_),
      bullets_(std::make_unique<BulletManager>()),
      enemies_(std::make_unique<EnemyManager>()),
      items_(std::make_unique<ItemManager>()),
      player_(std::make_unique<Player>()),
      gui_(textures, text ? text : &fallbackText_),
      textures_(textures) {
    world_.anm = &anm_;
    world_.bullets = bullets_.get();
    world_.enemies = enemies_.get();
    world_.items = items_.get();
    world_.player = player_.get();
    world_.state = &state_;
    world_.rng = &rng_;
    world_.sound = sound;
    world_.unported = &unported_;
    world_.shake = &shake_;
    world_.effects = &effects_;
    world_.gui = &gui_;
    world_.background = &background_;
    world_.scores = &scores_;
}

bool Stage::load(int stageNumber, int difficulty) {
    ready_ = false;
    ++loadSerial_;  // a save state from the previous stage no longer applies
    stageNumber_ = std::clamp(stageNumber, 1, 7);
    const int s = stageNumber_ - 1;
    if (!vfs_ || vfs_->fileCount() == 0) {
        error_ = "NO GAME DATA";
        return false;
    }
    if (!vfs_->hasFormat("PBG3")) {
        error_ = "NEW CLASSIC DATA ONLY - THE STAGE ENGINE NEEDS THE 2002 FILES";
        return false;
    }
    std::string err;
    auto need = [&](bool ok, const char* what) {
        if (!ok) error_ = std::string(what) + ": " + err;
        return ok;
    };
    const int character = state_.shotType / 2;
    if (loadedCharacter_ != character || !anm_.fileLoaded(kAnmFilePlayer)) {
        const char* playerAnm = character == 0 ? "data/player00.anm" : "data/player01.anm";
        if (!need(anm_.loadAnm(*vfs_, kAnmFilePlayer, playerAnm, kAnmOffsetPlayer, &err), character == 0 ? "PLAYER00.ANM" : "PLAYER01.ANM")) {
            return false;
        }
        loadedCharacter_ = character;
    }
    if (!shotsSearched_) {
        shotsSearched_ = true;
        std::string why = "no game program found in the folder";
        for (std::span<const std::uint8_t> exe : vfs_->executables()) {
            if (loadShotData(exe, shots_, &why)) break;
        }
        if (!shots_.loaded) log::warn("Shot tables unavailable ({}); using a basic shot", why);
    }
    world_.shots = shots_.loaded ? &shots_ : nullptr;
    if (!anm_.fileLoaded(kAnmFileBullet3) && !need(anm_.loadAnm(*vfs_, kAnmFileBullet3, "data/etama3.anm", kAnmOffsetBullet3, &err), "ETAMA3.ANM")) return false;
    if (!anm_.fileLoaded(kAnmFileBullet4) && !need(anm_.loadAnm(*vfs_, kAnmFileBullet4, "data/etama4.anm", kAnmOffsetBullet4, &err), "ETAMA4.ANM")) return false;
    // The previous stage's enemy art (and every enemy using it) goes away first.
    anm_.releaseAnm(kAnmFileEnemy);
    anm_.releaseAnm(kAnmFileEnemy2);
    if (!need(anm_.loadAnm(*vfs_, kAnmFileEnemy, kEnemyAnm[s][0], kAnmOffsetEnemy, &err), "STAGE ENEMY ANM")) return false;
    if (kEnemyAnm[s][1] && !need(anm_.loadAnm(*vfs_, kAnmFileEnemy2, kEnemyAnm[s][1], kAnmOffsetEnemy, &err), "STAGE ENEMY ANM 2")) return false;
    // The 3D background (optional: without it the playfield is plain).
    background_.load(world_, *vfs_, stageNumber_);
    // The stage's effect file (spell-card background effect). Optional: without it that one
    // effect type simply never shows.
    anm_.releaseAnm(kAnmFileEffects);
    if (!anm_.loadAnm(*vfs_, kAnmFileEffects, kEffectAnm[s], kAnmOffsetEffects, &err)) {
        log::warn("{} not loaded: {}", kEffectAnm[s], err);
    }

    auto eclBytes = vfs_->read(kEclFiles[s]);
    if (!eclBytes) {
        error_ = std::string(kEclFiles[s]) + " NOT FOUND";
        return false;
    }
    if (!ecl_.load(std::move(*eclBytes), &err)) {
        error_ = std::string(kEclFiles[s]) + " - " + err;
        return false;
    }
    world_.ecl = &ecl_;

    state_.setDifficulty(difficulty);
    bullets_ = std::make_unique<BulletManager>();
    world_.bullets = bullets_.get();
    if (!need(bullets_->initializeTemplates(world_, &err), "BULLET TYPES")) return false;
    items_->reset();
    effects_.reset();
    shake_.clear(world_);
    state_.grazes = 0;
    state_.stagePointItems = 0;
    state_.timeStop = 0;
    player_->reset(world_);
    // The game's own GUI (side panel, boss HUD, names, popups); loaded with the first stage
    // of a run and again when the character changes.
    const bool newRun = !guiLoadedOnce_ || gui_.ready() != true || loadedGuiCharacter_ != character || runStartStage_ == 0;
    if (newRun) {
        gui_.loadRun(world_, *vfs_, character);
        loadedGuiCharacter_ = character;
        guiLoadedOnce_ = true;
    }
    gui_.startStage(world_, *vfs_, stageNumber_, newRun);
    if (runStartStage_ == 0) startRun();
    gui_.setHiScore(std::max(gui_.hiScore(), state_.displayedScore));
    if (recordable_) scores_.stageEntered(state_.shotType, state_.difficulty, stageNumber_, state_.continuesUsed > 0);
    if (music_) {  // the stage theme, from its beginning (unless the music is off)
        music_->setUnported(&unported_);
        music_->play(gui_.songPath(0));
    }
    if (!shots_.loaded) unported_.hit("shot types (needs 東方紅魔郷.exe 1.02h in the game folder)");
    // The menus (reset at every stage load) and the playfield snapshot they draw over.
    menus_.reset();
    state_.continueMenu = false;
    clearInput_ = true;
    if (!anm_.fileLoaded(kAnmFileCapture) &&
        !anm_.loadAnm(*vfs_, kAnmFileCapture, "data/capture.anm", kAnmOffsetCapture, &err)) {
        log::warn("capture.anm not loaded ({}): the menus show the live playfield", err);
    }
    captureTexture_ = anm_.fileTexture(kAnmFileCapture);
    captureReady_ = false;
    enemies_->startStage(world_);
    clearTimer_ = 0;
    ready_ = true;
    error_.clear();
    log::info("Stage {} ({}) loaded", stageNumber_, kDifficultyNames[state_.difficulty]);
    return true;
}

void Stage::tick(const InputState& raw) {
    // Supervisor: input for this pass (nothing on the pass after a stage transition, so a
    // key held through it counts as a fresh press afterwards) and the music fades, which
    // run on through the menus.
    InputState in = clearInput_ ? InputState{} : raw;
    clearInput_ = false;
    const MenuInput edges{in.up && !previousInput_.up, in.down && !previousInput_.down,
                          in.shoot && !previousInput_.shoot, (in.pause && !previousInput_.pause) || menuPress_,
                          in.quit && !previousInput_.quit};
    previousInput_ = in;
    const bool menuPress = menuPress_;
    menuPress_ = false;
    if (inTitle_) {
        tickTitle(in, menuPress);
        return;
    }
    if (tickTitleScreens(in, menuPress)) return;
    if (music_ && ready_) music_->tick();
    if (!ready_ || quitRequested_) return;
    if (inEnding_) {
        if (ending_.update(world_, gui_, in)) {
            inEnding_ = false;
            endingMusic_ = true;
            gameOver(true);
        }
        if (sound_) sound_->flush();
        return;
    }
    if (inResults_) {
        if (results_.update(world_, gui_, in)) {
            saveScores();
            leaveGame();  // to the title
        }
        if (sound_) sound_->flush();
        return;
    }

    // The menu manager: the menus run instead of the score popups.
    if (menus_.frozen() || state_.continueMenu) runMenus(edges);
    else gui_.updatePopups();
    // Game over: the stage is gone (the result screen has taken the enemy art's slots).
    if (quitRequested_ || inResults_ || inEnding_ || inTitle_) return;
    // The game manager: the pause check, then the freeze.
    if (!menus_.frozen() && !state_.continueMenu && edges.menu) menus_.openPause();
    if (menus_.frozen()) return;
    // Practice save states (F5 / F9, pad L3 / R3, the touch buttons).
    if (saveStates_) {
        if (in.saveState && !saveEdge_) saveState();
        const bool loadNow = in.loadState && !loadEdge_;
        saveEdge_ = in.saveState;
        loadEdge_ = in.loadState;
        if (loadNow && loadState()) return;
    }
    if (toastTimer_ > 0) --toastTimer_;

    ++state_.frame;
    if (state_.cheat(MischiefFullPower)) state_.power = GameState::kMaxPower;
    rollScore(world_);
    background_.update(world_);
    InputState play = in;
    if (autoFire_) play.shoot = true;
    player_->update(world_, play);
    enemies_->update(world_);
    effects_.update(world_);
    bullets_->update(world_);
    if (state_.timeStop == 0) {  // the GUI freezes during a time stop (score popups do not)
        gui_.update(world_, in);
        player_->bomb().updateGui();
    }
    shake_.update(world_);
    if (sound_) sound_->flush();

    // The stage ends through its post-boss dialogue (results, then the next stage). Without
    // the dialogue file, it counts as cleared once the timeline has run out and every enemy
    // is gone.
    if (gui_.ready() && gui_.hasDialogue()) {
        if (gui_.takeStageEndRequest() && stageNumber_ < 7) {
            if (practiceRun_) practiceOver();  // a practice run is one stage
            else load(stageNumber_ + 1, state_.difficulty);
        } else if (gui_.takeGameClearRequest()) {
            gameCleared();  // the last stage's end
        }
    } else if (enemies_->timelineFinished() && enemies_->activeCount() == 0) {
        if (++clearTimer_ >= kClearDelay && stageNumber_ < 6) {
            state_.score += stageClearBonus(state_, stageNumber_, state_.startingLives);
            if (practiceRun_) practiceOver();
            else load(stageNumber_ + 1, state_.difficulty);  // keeps score, power, lives and bombs
        }
    } else {
        clearTimer_ = 0;
    }
}

void Stage::runMenus(const MenuInput& edges) {
    // The player ran out of lives on the previous pass: the continue menu opens now, unless
    // there is nothing to continue with (Extra, or all three credits used).
    if (state_.continueMenu && !menus_.continueActive()) {
        if (practiceRun_ || (state_.continuesUsed >= 3 && !state_.cheat(MischiefContinues)) || state_.difficulty == Extra) {
            gameOver(false);
            return;
        }
        menus_.openContinue();
    }
    switch (menus_.update(anm_, edges)) {
        case StageMenus::Result::Quit: leaveGame(); break;  // nothing is saved
        case StageMenus::Result::Continue: applyContinue(); break;
        case StageMenus::Result::GiveUp: gameOver(false); break;
        default: break;
    }
}

bool Stage::saveState() {
    if (!ready_ || inMenus() || inResults_ || inEnding_ || state_.continueMenu) return false;
    if (!save_) {
        save_ = std::unique_ptr<SaveSlot>(new SaveSlot{false, 0, 0, state_, rng_, *bullets_, *enemies_, *items_, *player_, shake_,
                                                       effects_, gui_, background_, {}, {}, 0, {}});
    } else {
        save_->state = state_;
        save_->rng = rng_;
        save_->bullets = *bullets_;
        save_->enemies = *enemies_;
        save_->items = *items_;
        save_->player = *player_;
        save_->shake = shake_;
        save_->effects = effects_;
        save_->gui = gui_;
        save_->background = background_;
    }
    SaveSlot& s = *save_;
    s.valid = true;
    s.generation = anm_.generation();
    s.loadSerial = loadSerial_;
    s.ecl.assign(ecl_.bytes().begin(), ecl_.bytes().end());
    s.clearTimer = clearTimer_;
    s.song = music_ ? music_->current() : std::string();
    s.text = Image();
    if (const int t = anm_.fileTexture(kAnmFileText); t >= 0) {
        if (const Image* px = textures_.pixels(static_cast<std::uint16_t>(t))) s.text = *px;
    }
    toast_ = "STATE SAVED";
    toastTimer_ = 60;
    return true;
}

bool Stage::loadState() {
    if (!ready_ || inMenus() || inResults_ || inEnding_) return false;
    if (!save_ || !save_->valid) {
        toast_ = "NO STATE SAVED";
        toastTimer_ = 60;
        return false;
    }
    SaveSlot& s = *save_;
    if (s.loadSerial != loadSerial_ || s.generation != anm_.generation() || !ecl_.restoreBytes(s.ecl)) {
        s.valid = false;  // another stage (or reloaded art): the state no longer fits
        toast_ = "STATE EXPIRED";
        toastTimer_ = 60;
        return false;
    }
    const std::uint32_t mischief = state_.mischief;
    state_ = s.state;
    state_.mischief = mischief;
    rng_ = s.rng;
    *bullets_ = s.bullets;
    *enemies_ = s.enemies;
    *items_ = s.items;
    *player_ = s.player;
    shake_ = s.shake;
    effects_ = s.effects;
    gui_ = s.gui;
    background_ = s.background;
    clearTimer_ = s.clearTimer;
    if (const int t = anm_.fileTexture(kAnmFileText); t >= 0 && s.text.width > 0) {
        if (Image* px = textures_.pixels(static_cast<std::uint16_t>(t)); px && px->width == s.text.width && px->height == s.text.height) {
            *px = s.text;
            textures_.touch(static_cast<std::uint16_t>(t));
        }
    }
    if (music_ && !s.song.empty() && music_->current() != s.song) music_->play(s.song);
    // Practice from here on: nothing from this game is saved.
    recordable_ = false;
    world_.scores = nullptr;
    toolsUsed_ = true;
    toast_ = "STATE LOADED";
    toastTimer_ = 60;
    return true;
}

void Stage::renderToast(SpriteSink& s) {
    if (toastTimer_ <= 0 || toast_.empty()) return;
    const auto a = static_cast<std::uint8_t>(std::min(255, toastTimer_ * 12));
    const float w = textWidth(toast_, 2.0f) + 24.0f;
    const float x = kGameRegionX + (kGameRegionWidth - w) * 0.5f, y = kGameRegionY + 56.0f;
    s.fillRect(ui_.white, x, y, w, 30.0f, rgba(60, 0, 25, static_cast<std::uint8_t>(a * 0.75f)));
    drawText(s, ui_, x + 12.0f, y + 8.0f, toast_, 2.0f, rgba(255, 210, 225, a));
}

// The first stage of a game: score.dat (the port's own, else the player's PC file) and the
// HUD's hi-score (in practice, the stage's practice best).
void Stage::startRun() {
    runStartStage_ = stageNumber_;
    recordable_ = state_.mischief == 0 && !stageSelect_ && speedPercent_ >= 100;
    toolsUsed_ = speedPercent_ < 100;
    if (!recordable_) world_.scores = nullptr;  // no spell history either
    loadScores();
    const std::uint32_t hi = practiceRun_ ? scores_.practice(state_.shotType, std::clamp(stageNumber_ - 1, 0, 5), std::min(state_.difficulty, 3))
                                          : scores_.hiScore(state_.difficulty, state_.shotType);
    gui_.setHiScore(hi);
}

void Stage::loadScores() {
    scores_ = ScoreFile{};
    bool loaded = false;
    if (!scorePath_.empty()) {
        if (std::FILE* f = std::fopen(scorePath_.c_str(), "rb")) {
            std::vector<std::uint8_t> bytes;
            std::uint8_t buf[4096];
            for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
            std::fclose(f);
            loaded = scores_.load(bytes);
            if (!loaded) log::warn("{} is not a valid score file; starting with empty tables", scorePath_);
        }
    }
    if (!loaded && vfs_) {
        if (const auto pc = vfs_->looseFile("score.dat"); !pc.empty()) {
            loaded = scores_.load(pc);
            log::info("High scores from the game folder's score.dat: {}", loaded ? "read" : "not valid");
        }
    }
}

// The last stage is over (stage 6, Easy's stage 5, or Extra): a practice run ends, Extra
// goes straight to the results, the main game to its ending first.
void Stage::gameCleared() {
    state_.displayedScore = state_.score;
    if (practiceRun_ || state_.difficulty == Extra) {
        gameOver(true);
        return;
    }
    startEnding();
}

void Stage::startEnding() {
    if (music_) music_->stop();  // the ending starts silent
    const bool continued = state_.continuesUsed > 0;
    const int d = std::clamp(state_.difficulty, 0, 3);
    const ScoreFile::Clear& c = scores_.clear(state_.shotType);
    Ending::Setup setup;
    setup.shotType = state_.shotType;
    setup.difficulty = state_.difficulty;
    setup.continued = continued;
    // Seen before: fast-forward is allowed from the start (the original only looks at the
    // record matching how this clear was made).
    setup.seen = (continued ? c.cleared[static_cast<std::size_t>(d)] : c.noContinue[static_cast<std::size_t>(d)]) ==
                 ScoreFile::kStageCleared;
    if (recordable_) scores_.gameCleared(state_.shotType, state_.difficulty, continued);
    if (!vfs_ || !ending_.start(world_, *vfs_, textures_, gui_, setup)) {
        unported_.hit("ending (the ED archive's end files are missing)");
        gameOver(true);
        return;
    }
    inEnding_ = true;
    clearInput_ = true;
}

// The game ended: a full game goes to the result screen (name entry, statistics), a
// practice run straight back to the title.
void Stage::gameOver(bool cleared) {
    state_.continueMenu = false;
    state_.displayedScore = state_.score;
    if (practiceRun_) {
        practiceOver();
        return;
    }
    if (music_ && !endingMusic_) music_->stop();
    ResultScreen::Run run;
    run.difficulty = state_.difficulty;
    run.shotType = state_.shotType;
    run.stage = stageNumber_;
    run.cleared = cleared;
    run.score = state_.displayedScore;
    run.continues = state_.continuesUsed;
    run.deaths = state_.deaths;
    run.bombs = state_.bombsUsed;
    run.spellsCaptured = state_.spellsCaptured;
    run.completion = state_.completion;
    run.recordable = recordable_;
    if (!vfs_ || !results_.start(world_, *vfs_, textures_, gui_, scores_, run)) {
        unported_.hit("results screen (result files missing)");
        saveScores();
        leaveGame();
        return;
    }
    inResults_ = true;
    clearInput_ = true;
}

void Stage::saveScores() {
    if (recordable_) writeScores();
}

void Stage::writeScores() {
    if (scorePath_.empty()) return;
    const std::vector<std::uint8_t> bytes = scores_.save(rng_);
    if (std::FILE* f = std::fopen(scorePath_.c_str(), "wb")) {
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        if (ok) log::info("Scores saved to {}", scorePath_);
        else log::warn("Could not write {}", scorePath_);
    } else {
        log::warn("Could not open {} for writing", scorePath_);
    }
}

// "Yes" at the continue menu: a fresh set of lives and bombs, the score restarting from the
// number of continues used, and the respawn already under way carries on.
void Stage::applyContinue() {
    GameState& gs = state_;
    gs.continueMenu = false;
    ++gs.continuesUsed;
    gs.score = gs.displayedScore = static_cast<std::uint32_t>(gs.continuesUsed);
    gs.scoreStep = 0;
    gs.livesRemaining = gs.startingLives;
    gs.bombs = gs.startingBombs;
    gs.power = 0;
    gs.grazes = 0;
    gs.stagePointItems = 0;
    gs.extendIndex = 0;
}

// ---- the title and the start and end of a game ---------------------------------------

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}

bool writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

void Stage::setConfigPath(std::string path) {
    configPath_ = std::move(path);
    if (!configPath_.empty()) {
        const std::vector<std::uint8_t> bytes = readFile(configPath_);
        if (!bytes.empty() && !config_.load(bytes)) log::warn("{} is not valid; using the default settings", configPath_);
        saveConfig();  // as the original, written back at every start
    }
    applyConfig();
}

void Stage::saveConfig() {
    if (configPath_.empty()) return;
    if (!writeFile(configPath_, config_.save())) log::warn("Could not write {}", configPath_);
}

void Stage::applyConfig() {
    if (sound_) sound_->setEnabled(config_.sound != 0);
    if (music_ && music_->enabled() != (config_.music != GameConfig::MusicOff)) {
        music_->setEnabled(config_.music != GameConfig::MusicOff);
    }
}

// The title object is created: back from a game (cursor on Extra Start after an Extra game,
// on Practice Start after practice), or at start-up.
bool Stage::openTitle(bool firstVisit, int cursor) {
    if (!vfs_ || !vfs_->hasFormat("PBG3")) return false;
    std::string err;
    if (!anm_.fileLoaded(kAnmFileAscii) && !anm_.loadAnm(*vfs_, kAnmFileAscii, "data/ascii.anm", kAnmOffsetAscii, &err)) {
        log::warn("ascii.anm not loaded ({}): the title has no text", err);
    }
    // text.anm: the Music Room's comments and the score viewer's names.
    if (!anm_.fileLoaded(kAnmFileText) && !anm_.loadAnm(*vfs_, kAnmFileText, "data/text.anm", kAnmOffsetText, &err)) {
        log::warn("text.anm not loaded ({})", err);
    }
    // What is left of the game (enemy art, result and staff-roll files) shares the title's
    // script numbers.
    for (const int slot : {kAnmFileEnemy, kAnmFileEnemy2, 11, 12, 44, 45, 46}) anm_.releaseAnm(slot);
    loadScores();  // the Extra lock and the practice stages come from score.dat
    applyConfig();
    if (cursor < 0) cursor = session_.difficulty == Extra ? 1 : 0;
    if (!title_.start(world_, *vfs_, textures_, scores_, config_, session_, cursor, firstVisit)) {
        title_.release(world_);
        log::warn("Title screen files missing: starting a game directly");
        return false;
    }
    inTitle_ = true;
    inResults_ = inEnding_ = false;
    quitRequested_ = false;
    state_.continueMenu = false;
    menus_.reset();
    clearInput_ = true;
    return true;
}

void Stage::tickTitle(const InputState& in, bool menuPress) {
    InputState t = in;
    if (menuPress) t.pause = true;  // Android Back is Esc (Cancel)
    if (music_) music_->tick();
    const Title::Result r = title_.update(world_, t);
    if (title_.takeConfigChanged()) saveConfig();
    applyConfig();
    if (sound_) sound_->flush();
    switch (r) {
        case Title::Result::StartGame:
            startFromTitle();
            break;
        case Title::Result::Quit:
            saveConfig();
            title_.release(world_);
            inTitle_ = false;
            quitRequested_ = true;
            break;
        case Title::Result::Failed:
            title_.release(world_);
            inTitle_ = false;
            ready_ = false;
            error_ = "THE SELECT SCREEN FILES (DATA/SELECT0N.ANM) ARE MISSING";
            break;
        case Title::Result::OpenScores:  // the score viewer; the title theme plays on
            title_.release(world_);
            inTitle_ = false;
            loadScores();
            if (vfs_ && results_.startTitleMode(world_, *vfs_, textures_, scores_)) {
                inResults_ = true;
                clearInput_ = true;
            } else if (!openTitle(false, 4)) {
                quitRequested_ = true;
            }
            break;
        case Title::Result::OpenMusicRoom:
            title_.release(world_);
            inTitle_ = false;
            if (vfs_ && musicRoom_.start(world_, *vfs_, textures_, gui_)) {
                inMusicRoom_ = true;
                clearInput_ = true;
            } else if (!openTitle(false, 5)) {
                quitRequested_ = true;
            }
            break;
        case Title::Result::None:
            break;
    }
}

// The Music Room and the score viewer run before any game (and without one loaded).
bool Stage::tickTitleScreens(const InputState& in, bool menuPress) {
    const bool scoreViewer = inResults_ && results_.titleMode();
    if (!inMusicRoom_ && !scoreViewer) return false;
    InputState t = in;
    if (menuPress) t.pause = true;  // Android Back is Esc (Cancel)
    if (music_) music_->tick();
    if (inMusicRoom_) {
        if (musicRoom_.update(world_, gui_, t)) {  // back at once; the track plays on until the title
            musicRoom_.release(world_);
            inMusicRoom_ = false;
            if (!openTitle(false, 5)) quitRequested_ = true;
        }
    } else if (results_.update(world_, gui_, t)) {
        writeScores();  // (always, as the original)
        inResults_ = false;
        if (!openTitle(false, 4)) quitRequested_ = true;
    }
    if (sound_) sound_->flush();
    return true;
}

void Stage::startFromTitle() {
    const Title::Choice c = title_.choice();
    title_.release(world_);
    inTitle_ = false;
    saveConfig();  // the remembered difficulty
    setShotType(c.shotType);
    newRun(c.stage, c.lives, c.bombs, c.practice);
    if (!load(c.stage, c.stage == 7 ? Extra : c.difficulty)) log::warn("Stage not started: {}", error_);
}

bool Stage::quickStart(int stageNumber, int difficulty, bool stageSelect) {
    stageNumber = std::clamp(stageNumber, 1, 7);
    const int d = stageNumber == 7 ? Extra : std::clamp(difficulty, 0, 3);
    session_.difficulty = d;
    session_.character = state_.shotType / 2;
    session_.shot = state_.shotType % 2;
    session_.practice = false;
    const bool extra = d == Extra;
    newRun(stageNumber, extra ? 2 : config_.lives, extra ? 3 : config_.bombs, false);
    if (stageSelect) {
        // The launcher's stage select: play on from the chosen stage, with the power a
        // practice run of that stage would have; nothing from this game is saved.
        stageSelect_ = true;
        state_.power = stageNumber <= 1 ? 0 : stageNumber == 2 ? 64 : GameState::kMaxPower;
    }
    return load(stageNumber, d);
}

// Everything a game starts from: the starting lives and bombs (kept for continues, the
// respawn bombs and the stage-clear penalty), and in practice the stage's power.
void Stage::newRun(int stageNumber, int lives, int bombs, bool practice) {
    const std::uint32_t mischief = state_.mischief;
    const int shotType = state_.shotType;
    state_ = GameState{};
    state_.mischief = mischief;
    state_.shotType = shotType;
    state_.livesRemaining = state_.startingLives = std::clamp(lives, 0, 4);
    state_.bombs = state_.startingBombs = std::clamp(bombs, 0, 3);
    state_.practice = practice;
    if (practice) state_.power = stageNumber <= 1 ? 0 : stageNumber == 2 ? 64 : GameState::kMaxPower;
    practiceRun_ = practice;
    stageSelect_ = false;
    runStartStage_ = 0;
    quitRequested_ = inResults_ = inEnding_ = endingMusic_ = false;
    world_.scores = &scores_;
}

// A practice run ends (its stage is over, or the player is): the best score for that
// stage, and score.dat is written either way (with the stage progress and spell history).
void Stage::practiceOver() {
    state_.continueMenu = false;
    state_.displayedScore = state_.score;
    if (recordable_) {
        std::uint32_t& best = scores_.practice(state_.shotType, std::clamp(stageNumber_ - 1, 0, 5), std::min(state_.difficulty, 3));
        if (state_.score > best) best = state_.score;
        saveScores();
    }
    leaveGame();
}

// Back to the title (or, without the title files, to the launcher).
void Stage::leaveGame() {
    if (music_) music_->stop();
    if (!openTitle(false)) quitRequested_ = true;
}

void Stage::render(SpriteSink& s, float alpha, const HudInfo& hud) {
    anm_.beginFrame();
    if (inTitle_) {
        title_.draw(world_, gui_, s, ui_.white);
        return;
    }
    if (inEnding_) {
        ending_.draw(world_, s, ui_.white);
        return;
    }
    if (inResults_) {
        results_.draw(world_, gui_, s);
        gui_.drawStrings(world_, s);
        return;
    }
    if (inMusicRoom_) {
        musicRoom_.draw(world_, gui_, s);
        return;
    }
    // The playfield is cleared to the fog colour (the background is drawn into it).
    const std::uint32_t clear = background_.loaded() ? argbToRgbaBytes(background_.fogColor() | 0xff000000u) : rgba(8, 6, 20);
    s.fillRect(ui_.white, kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight, clear);
    if (ready_) {
        background_.draw(world_, s, ui_.white, alpha, gui_.loadingHalted());
        player_->drawLow(world_, s, alpha, ui_.white);
        enemies_->draw(world_, s, alpha);
        player_->drawHits(world_, s, alpha);
        effects_.draw(world_, s, alpha);
        bullets_->draw(world_, s, alpha);
        if (showHitbox_ && (player_->state == Player::Alive || player_->state == Player::Invulnerable)) {
            // Practice: the ship's hitbox (the 2002 game never shows it).
            const Vec3f p = lerp(player_->prevPosition, player_->positionCenter, alpha);
            const float x = state_.playfieldX + p.x, y = state_.playfieldY + p.y;
            s.drawRegion(ui_.disc, x, y, 7.0f / static_cast<float>(ui_.disc.width), rgba(200, 20, 40, 230));
            s.drawRegion(ui_.disc, x, y, 3.5f / static_cast<float>(ui_.disc.width), rgba(255, 255, 255, 255));
        }
        gui_.drawPopups(world_, s);
        if (!gui_.ready()) renderBombName(s);
    }
    if (ready_ && gui_.ready()) {
        gui_.draw(world_, s, ui_.white, hud.fps);
        gui_.drawStrings(world_, s);
        renderDiagnostics(s);
        renderToast(s);
        menus_.draw(anm_, s, state_.cheat(MischiefContinues) ? 9 : 3 - state_.continuesUsed, captureReady_);
        return;
    }
    // Without the game's GUI files: the port's own frame and diagnostic HUD.
    const auto frame = rgba(46, 18, 40);
    s.fillRect(ui_.white, 0, 0, 640, kGameRegionY, frame);
    s.fillRect(ui_.white, 0, kGameRegionY + kGameRegionHeight, 640, 480 - kGameRegionY - kGameRegionHeight, frame);
    s.fillRect(ui_.white, 0, kGameRegionY, kGameRegionX, kGameRegionHeight, frame);
    s.fillRect(ui_.white, kGameRegionX + kGameRegionWidth, kGameRegionY, 640 - kGameRegionX - kGameRegionWidth,
               kGameRegionHeight, frame);
    renderHud(s, hud);
    if (!ready_) {
        renderMessage(s);
    } else if (menus_.frozen()) {  // without the game's GUI files there are no menu sprites
        const std::string_view t = state_.continueMenu ? "CONTINUE? SHOT = YES" : "PAUSED";
        drawText(s, ui_, kGameRegionX + (kGameRegionWidth - textWidth(t, 3.0f)) * 0.5f, 220, t, 3.0f, rgba(255, 255, 255));
    }
}

// The bomb's name on a dark bar at the bottom of the playfield, sliding in from the left
// when the bomb starts and fading when it ends. (The original draws it from the game's own
// text and front ANM scripts; this stands in until those GUI layers are ported.)
void Stage::renderBombName(SpriteSink& s) {
    const Bomb& bomb = player_->bomb();
    const float v = bomb.nameVisibility();
    if (v <= 0.0f) return;
    const std::string_view name = bomb.name();
    const float w = textWidth(name, 1.0f) + 16.0f;
    const float x = kGameRegionX - w + (w + 8.0f) * std::min(v * 2.0f, 1.0f);
    const float y = kGameRegionY + kGameRegionHeight - 30.0f;
    const auto a = static_cast<std::uint8_t>(255.0f * std::min(v * 1.5f, 1.0f));
    s.fillRect(ui_.white, x, y, w, 14.0f, rgba(20, 10, 40, static_cast<std::uint8_t>(a * 0.7f)));
    drawText(s, ui_, x + 8.0f, y + 3.0f, name, 1.0f, rgba(240, 240, 255, a));
}

// With the game's panel on screen, only the list of features the data used that are not
// ported yet, small, at the bottom of the side panel.
void Stage::renderDiagnostics(SpriteSink& s) {
    if (state_.mischief != 0 || stageSelect_ || toolsUsed_) {  // say so: these scores are not saved
        std::string t;
        if (state_.cheat(MischiefLives)) t += " LIVES";
        if (state_.cheat(MischiefBombs)) t += " BOMBS";
        if (state_.cheat(MischiefInvincible)) t += " INVINCIBLE";
        if (state_.cheat(MischiefFullPower)) t += " POWER";
        if (state_.cheat(MischiefContinues)) t += " CONT";
        if (speedPercent_ < 100) t = " SLOW " + std::to_string(speedPercent_) + "%" + t;
        if (toolsUsed_ && speedPercent_ >= 100) t = " STATE LOADED" + t;
        if (stageSelect_) t = (runStartStage_ == 7 ? std::string(" FROM THE EXTRA STAGE") : " FROM STAGE " + std::to_string(runStartStage_)) + t;
        s.fillRect(ui_.white, 420.0f, 352.0f, 216.0f, 23.0f, rgba(60, 0, 40, 170));
        const char* head = stageSelect_ ? "STAGE SELECT (SCORES NOT SAVED):"
                           : state_.mischief != 0 ? "MISCHIEF (SCORES NOT SAVED):"
                                                  : "PRACTICE (SCORES NOT SAVED):";
        drawText(s, ui_, 424.0f, 355.0f, head, 1.0f, rgba(255, 140, 200));
        drawText(s, ui_, 424.0f, 365.0f, t.substr(1, 34), 1.0f, rgba(255, 190, 220));
    }
    if (unported_.counts().empty()) return;
    const float x = 424.0f;
    float y = 380.0f;
    const auto n = static_cast<float>(std::min<std::size_t>(unported_.counts().size(), 6));
    s.fillRect(ui_.white, x - 4.0f, y - 4.0f, 216.0f, 16.0f + 9.0f * n, rgba(0, 0, 0, 170));
    drawText(s, ui_, x, y, "NOT PORTED YET:", 1.0f, rgba(255, 210, 120));
    y += 11.0f;
    int shown = 0;
    for (const auto& [name, count] : unported_.counts()) {
        if (++shown > 6) break;
        std::string t = name.substr(0, std::min<std::size_t>(name.find(" ("), name.size()));
        for (char& c : t) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        drawText(s, ui_, x + 4.0f, y, t.substr(0, 34), 1.0f, rgba(220, 190, 140));
        y += 9.0f;
    }
}

void Stage::renderHud(SpriteSink& s, const HudInfo& hud) {
    const auto label = rgba(255, 170, 200);
    const auto value = rgba(255, 255, 255);
    const float x = 432;
    float y = 24;
    auto line = [&](std::string_view name, const std::string& v) {
        drawText(s, ui_, x, y, name, 1.0f, label);
        drawText(s, ui_, x + 72, y, v, 1.0f, value);
        y += 12;
    };
    drawText(s, ui_, x, y, "TH06 PORT", 2.0f, value);
    y += 24;
    if (ready_) {
        drawText(s, ui_, x, y, std::format("STAGE {}  {}", stageNumber_, kDifficultyNames[state_.difficulty]), 1.0f,
                 rgba(170, 220, 255));
        y += 18;
        line("SCORE", std::format("{}", state_.displayedScore));
        line("PLAYER", std::format("{}", state_.livesRemaining));
        line("BOMB", std::format("{}", state_.bombs));
        line("POWER", state_.power >= GameState::kMaxPower ? std::string("MAX") : std::format("{}", state_.power));
        line("GRAZE", std::format("{}", state_.grazes));
        line("POINT", std::format("{}", state_.stagePointItems));
        line("RANK", std::format("{}", state_.rank));
        y += 6;
        line("FRAME", std::format("{}", enemies_->timelineTime()));
        line("ENEMIES", std::format("{}", enemies_->activeCount()));
        line("BULLETS", std::format("{} / {}", bullets_->activeCount(), BulletManager::kMaxBullets));
        line("LASERS", std::format("{}", bullets_->activeLaserCount()));
        line("ITEMS", std::format("{}", items_->activeCount()));
        line("EFFECTS", std::format("{}", effects_.activeCount()));
        y += 6;
        line("FPS", std::format("{:.1f}", hud.fps));
        line("DRAWS", std::format("{}", hud.drawCalls));
        if (enemies_->timelineFinished() && enemies_->activeCount() == 0) {
            y += 6;
            drawText(s, ui_, x, y, "TIMELINE COMPLETE", 1.0f, rgba(150, 255, 170));
            y += 12;
        }
    }
    // Features the game data used that are not ported yet.
    if (!unported_.counts().empty()) {
        y += 10;
        drawText(s, ui_, x, y, "NOT PORTED YET:", 1.0f, rgba(255, 210, 120));
        y += 12;
        for (const auto& [name, count] : unported_.counts()) {
            if (y > 452) break;
            std::string n = name.substr(0, std::min<std::size_t>(name.find(" ("), name.size()));
            for (char& c : n) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
            drawText(s, ui_, x + 6, y, n.substr(0, 31), 1.0f, rgba(220, 190, 140));
            y += 10;
        }
    }
}

void Stage::renderMessage(SpriteSink& s) {
    const auto warn = rgba(255, 210, 120);
    float y = 180;
    auto centred = [&](std::string_view t, float scale, std::uint32_t color) {
        drawText(s, ui_, kGameRegionX + (kGameRegionWidth - textWidth(t, scale)) * 0.5f, y, t, scale, color);
        y += 10 * scale + 6;
    };
    centred("CANNOT START STAGE", 2.0f, rgba(255, 255, 255));
    std::string e = error_;
    for (char& c : e) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    for (std::size_t at = 0; at < e.size() && at < 64 * 3; at += 60) centred(std::string_view(e).substr(at, 60), 1.0f, warn);
    y += 10;
    centred("CHOOSE THE FOLDER OF THE 2002 GAME", 1.0f, rgba(200, 200, 220));
    centred("(KOUMAKYO_ST.DAT / ...ST.DAT AND FRIENDS)", 1.0f, rgba(200, 200, 220));
}

}  // namespace th06
