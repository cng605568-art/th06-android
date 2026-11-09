#include "th07/Gameplay.hpp"

#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "io/Vfs.hpp"
#include "th07/Bgm.hpp"
#include "th07/ExeTables.hpp"
#include "th07/SoundPlayer.hpp"

namespace touhou::th07 {

Gameplay::Gameplay() : player_(std::make_unique<Player>()) {}

bool Gameplay::takeCaptureRequest() {
    if (captureOwner_ == CaptureOwner::None) {
        if (menu_.takeCaptureRequest()) captureOwner_ = CaptureOwner::Menu;
        else if (gui_ && gui_->takeCaptureRequest()) captureOwner_ = CaptureOwner::Gui;
    }
    return captureOwner_ != CaptureOwner::None;
}

void Gameplay::captureRect(int& x, int& y, int& w, int& h) const {
    x = y = w = h = 0;
    if (captureOwner_ == CaptureOwner::Menu) menu_.captureRect(x, y, w, h);
    else if (captureOwner_ == CaptureOwner::Gui && gui_) gui_->captureRect(x, y, w, h);
}

void Gameplay::setCaptureReady(bool ready) {
    switch (captureOwner_) {
        case CaptureOwner::Menu: menu_.setCaptureReady(ready); break;
        case CaptureOwner::Gui:
            if (gui_) gui_->setCaptureReady(ready);
            break;
        case CaptureOwner::None:  // GL resources lost: every snapshot is gone
            menu_.setCaptureReady(false);
            if (gui_) gui_->setCaptureReady(false);
            break;
    }
    captureOwner_ = CaptureOwner::None;
}
Gameplay::~Gameplay() = default;

bool Gameplay::start(const Resources& res, const Setup& setup, std::string* error) {
    end();
    res_ = res;
    setup_ = setup;
    state_ = GameState{};
    state_.setDifficulty(setup.difficulty);
    state_.shotIndex = setup.shotIndex;
    state_.practice = setup.practice;
    state_.lifeCount = setup.lifeCount;
    state_.slowMode = setup.slowMode;
    state_.stage = setup.stage - 1;
    playback_ = setup.playback;
    recorder_ = playback_ ? nullptr : setup.recorder;
    playbackMode_ = playback_ ? std::clamp(setup.playbackMode, 0, 2) : 0;
    demo_ = playback_ && setup.demo;
    demoIndex_ = std::clamp(setup.demoIndex, 0, 2);
    demoFrame_ = 0;
    state_.replay = playback_ != nullptr;
    state_.replayStages = playback_ ? playback_->stageMask() : 0;
    if (playback_) state_.practice = false;  // (practice replays play as ordinary games)
    world_ = World{};
    world_.state = &state_;
    world_.rng = res.rng;
    world_.anm = res.anm;
    world_.tables = res.tables;
    world_.english = res.english;
    world_.sound = res.sound;
    world_.player = player_.get();
    world_.effects = res.tables ? &effects_ : nullptr;
    world_.background = &background_;
    world_.shakes = &screenFx_;
    world_.bullets = bullets_.get();
    world_.bulletManager = bullets_.get();
    world_.items = bullets_.get();
    world_.enemies = enemies_.get();
    world_.gui = gui_.get();
    world_.popups = gui_.get();
    world_.onMusic = [this](int track) {
        if (res_.music) res_.music->playLoaded(track);
    };
    world_.onMusicFade = [this](float seconds) {
        if (res_.music) res_.music->fadeOut(seconds);
    };
    world_.onDialogueStart = [this]() { enemies_->removeAll(); };
    world_.onSpellDeclared = [this](int card, const std::string& name) {
        if (hooks_.spellDeclared && !state_.replay) hooks_.spellDeclared(card, name);
    };
    world_.onSpellCaptured = [this](int card, std::uint32_t bonus) {
        if (hooks_.spellCaptured && !state_.replay) hooks_.spellCaptured(card, bonus);
    };
    enemies_->setMusic(res.music);
    outcome_ = Outcome::None;
    // (The object is reused for every game: nothing of the last one carries over.)
    prevRaw_ = 0;
    gameInput_ = prevGameInput_ = 0;
    wasPaused_ = slowSkip_ = false;
    slowLag_ = replayLag_ = 0;
    pauseRequested_ = false;
    menu_ = InGameMenu{};
    gui_->setDemo(demo_);
    return setUpStage(true, error);
}

bool Gameplay::nextStage(std::string* error) {
    if (!running_) return false;
    // (The original cuts the game manager between stages: its teardown closes the block.)
    if (recorder_) recorder_->closeBlock();
    releaseStageFiles();
    return setUpStage(false, error);
}

bool Gameplay::loadEffectFiles(std::string* error) {
    struct File {
        const char* name;
        int slot, offset;
    };
    std::vector<File> files;
    switch (state_.stage) {
        case 1: files = {{"data/eff01.anm", 17, 0x2DC}}; break;
        case 2: files = {{"data/eff02.anm", 17, 0x2DC}}; break;
        case 3: files = {{"data/eff03.anm", 17, 0x2DC}}; break;
        case 4: files = {{"data/eff04.anm", 17, 0x2DC}, {"data/eff04b.anm", 18, 0x2DD}}; break;
        case 5: files = {{"data/eff05.anm", 17, 0x2DC}}; break;
        case 6: files = {{"data/eff05.anm", 17, 0x2DC}, {"data/eff06.anm", 19, 0x2DE}}; break;
        case 7: files = {{"data/eff02.anm", 17, 0x2DC}, {"data/eff07.anm", 18, 0x2DD}}; break;
        default: files = {{"data/eff07.anm", 17, 0x2DC}, {"data/eff08.anm", 19, 0x2DE}}; break;
    }
    for (const File& f : files) {
        if (!res_.anm->load(*res_.vfs, *res_.textures, f.name, f.slot, f.offset, error)) return false;
    }
    return true;
}

void Gameplay::releaseStageFiles() {
    background_.release(*res_.anm);
    enemies_->release(*res_.anm);
    gui_->stageEnd(world_);
    for (int slot : {17, 18, 19, 20}) res_.anm->release(slot);
}

bool Gameplay::setUpStage(bool fresh, std::string* error) {
    GameState& g = state_;
    ZunRng& rng = *res_.rng;
    const World& w = world_;
    g_timing.speed = 1.0f;  // the force-integer flag is never reset
    if (fresh) {
        rng.u32InRange(65535);  // a dummy allocation size
        // The anti-tamper canaries: the cherry base first, then 29 more values.
        g.cherryBase = static_cast<std::int32_t>(rng.u32InRange(100000)) + 6543;
        for (int i = 0; i < 15; ++i) rng.u32InRange(100000);
        for (int i = 0; i < 9; ++i) rng.f32ZeroToOne();
        for (int i = 0; i < 5; ++i) rng.u32InRange(100000);
        config_ = setup_.config;  // the configuration copy
        g.fullPowerChain = 0;
        if (g.difficulty >= Extra) g.lifeCount = 2;
        else if (g.practice) g.lifeCount = 8;
        if (!player_->load(*res_.vfs, *res_.textures, *res_.anm, g.shotIndex, true, error)) return false;
        if (!g.replay) {
            w.setLives(static_cast<float>(g.lifeCount));
            w.setBombs(static_cast<float>(static_cast<int>(player_->header().initialBombs)));
        }
        w.setPower(0.0f);
        g.score = g.guiScore = g.rollSpeed = 0;
        g.hiScore = 100000;
        g.continues = 0;
        g.totalGraze = 0;
        g.extendPoints = 0;
        g.extendsGranted = 0;
        g.nextExtend = g.difficulty >= Extra ? 200 : 50;
        if (g.difficulty >= Extra) g.slowMode = false;
        // Step 7: score.dat (rewritten, then the hi-score and records read).
        if (hooks_.readScores) hooks_.readScores(g);
        w.setMisses(0.0f);
        w.setBombsUsed(0.0f);
        g.spellsCaptured = 0;
        static constexpr std::int32_t kMax[6] = {200000, 200000, 250000, 300000, 400000, 400000};
        g.cherryMax = kMax[g.difficulty];
        g.cherry = g.difficulty == Extra ? 200000 : g.difficulty == Phantasm ? 300000 : 0;
        g.cherryPlus = 0;
        const int s = g.stage + 1;
        if (g.practice && s >= 2) {
            g.cherryMax += 50000 * (s - 2);
            g.cherry = g.cherryMax;
        }
        // Step 8: a play is counted (live play, slow mode off).
        if (!g.replay && !g.slowMode && hooks_.countPlay) hooks_.countPlay(g, setup_.restart);
        if (g.replay) g.slowMode = false;
        config_.lives = static_cast<std::uint8_t>(g.lifeCount);
        config_.slowMode = g.slowMode ? 1 : 0;
    } else {
        g.guiScore = g.score;
        g.rollSpeed = 0;
        if (!player_->load(*res_.vfs, *res_.textures, *res_.anm, g.shotIndex, false, error)) return false;
    }
    g.subRank = 0;
    g.stagePointItems = 0;
    g.stageGraze = 0;
    ++g.stage;
    if (!g.replay && hooks_.stageEntered) hooks_.stageEntered(g);
    if (g.practice && g.stage >= 2) w.setPower(128.0f);
    // Step 11: playback restores the stage's block (it sets the RNG seed). The shadow values
    // are then regenerated with the seed saved and restored around it: no net effect.
    if (playback_) restoreReplayStage();
    // Step 13: the stage seed.
    stageSeed_ = rng.currentSeed();

    if (!etamaLoaded_) {
        if (!res_.anm->load(*res_.vfs, *res_.textures, "data/etama.anm", 11, 0x200, error)) return false;
        etamaLoaded_ = true;
    }
    if (!background_.load(*res_.vfs, *res_.textures, *res_.anm, rng, g.stage, error)) return false;
    background_.onWeatherShift = [this](const Vec3f& d) {  // a camera jump moves the weather with it
        for (int i = 0; i < Effects::kNormal; ++i) {
            Effect& e = effects_.normal(i);
            if (e.inUse && (e.id == 20 || e.id == 31)) e.base += d;
        }
    };
    background_.drawWeather = [this](SpriteSink& sink, AnmDrawState& state) { effects_.drawStageLayer(sink, state, background_); };
    background_.white = res_.ui ? &res_.ui->white : nullptr;
    bullets_->stageStart(w);
    // The enemy manager (stgNenm.anm, two RNG draws), then the stage's ECL.
    if (!enemies_->stageStart(w, *res_.vfs, *res_.textures, g.stage, error)) return false;
    effects_.reset();
    if (!loadEffectFiles(error)) return false;
    screenFx_.clear();
    if (hooks_.playTimeRebase) hooks_.playTimeRebase();
    {
        Gui::Resources gr;
        gr.vfs = res_.vfs;
        gr.textures = res_.textures;
        gr.text = res_.text;
        gr.ui = res_.ui;
        if (!gui_->stageStart(w, gr, fresh, error, fresh && setup_.restart)) return false;
    }
    player_->stageStart(w);
    // Step 15: live play records into a block of the recording (the first stage creates it).
    if (recorder_) {
        if (fresh) recorder_->create(g.shotIndex, g.difficulty, config_, setup_.exe);
        startRecordingBlock();
    }

    if (res_.music) {
        res_.music->load(0, background_.songPath(0));
        res_.music->load(1, background_.songPath(1));
        if (g.stage == 6) {
            res_.music->stop();
            res_.music->load(2, "bgm/th07_13b.mid");
        } else {
            res_.music->playLoaded(0);
        }
    }
    g.bombActive = false;
    g.timeStop = false;
    g.score = 0;  // the first score roll restores it from the HUD score
    g.stageFrame = 0;
    running_ = true;
    log::info("th07: stage {} ({} {}, seed {:04x})", g.stage, g.difficulty, g.shotIndex, stageSeed_);
    return true;
}

void Gameplay::restoreReplayStage() {
    GameState& g = state_;
    const int index = g.stage >= 7 ? 6 : std::max(0, g.stage - 1);
    g.setDifficulty(g.difficulty);  // the rank is initialised, then set from the block
    th07::restoreReplayStage(world_, *playback_, index, config_);
    feed_.start(playback_->hasStage(index) ? &playback_->stages[static_cast<std::size_t>(index)] : nullptr);
    gameInput_ = prevGameInput_ = 0;
    replayLag_ = 0;
}

void Gameplay::startRecordingBlock() {
    const GameState& g = state_;
    ReplayRecorder::StageStart v;
    v.seed = stageSeed_;
    v.score = g.score;
    v.extendPoints = static_cast<std::int32_t>(g.extendPoints);
    v.cherry = g.cherry;
    v.cherryMax = g.cherryMax;
    v.cherryPlus = g.cherryPlus;
    v.totalGraze = static_cast<std::int32_t>(g.totalGraze);
    v.extendsGranted = g.extendsGranted;
    v.nextExtend = g.nextExtend;
    v.power = g.powerI();
    v.lives = g.livesI();
    v.bombs = g.bombsI();
    v.rank = g.rank;
    v.chain = g.fullPowerChain;
    v.spells = g.spellsCaptured;
    recorder_->startBlock(g.stage >= 7 ? 6 : g.stage - 1, v);
    gameInput_ = prevGameInput_ = 0;
}

void Gameplay::end() {
    if (!running_) return;
    releaseStageFiles();
    player_->releaseAnm(*res_.anm);
    gui_->gameEnd(world_);
    if (etamaLoaded_) res_.anm->release(11);
    etamaLoaded_ = false;
    if (res_.music) res_.music->stop();
    // Teardown step 3: the current block gets its terminator.
    if (recorder_) recorder_->closeBlock();
    running_ = false;
}

bool Gameplay::tick(std::uint16_t raw) {
    if (!running_) return false;
    GameState& g = state_;
    g.dialogue = gui_->dialogueCurrent();
    const std::uint16_t pressed = static_cast<std::uint16_t>(raw & ~prevRaw_);
    // "Any button" as a mask: the word is non-zero and differs from the last pass's.
    const bool anyPressed = raw != 0 && raw != prevRaw_;
    struct Done {  // the previous raw input is kept on every way out of the pass
        Gameplay& self;
        std::uint16_t raw;
        ~Done() { self.prevRaw_ = raw; }
    } done{*this, raw};
    // The text/menu manager (priority 1): a pause or continue menu runs instead of the
    // popups, and the game manager then stops the pass while it is up. A menu's last update
    // clears its flag, so the game manager proceeds in that pass (spec 2.2): also when it
    // leaves the game (its state request is acted on at the next pass), and when the continue
    // menu exits at once (practice, replays: one more frame is simulated and recorded).
    const bool menuWasUp = menu_.active();
    if (menuWasUp) {
        const InGameMenu::Request r = menu_.update(world_, pressed);
        switch (r) {
            case InGameMenu::Request::Resume:
                if (res_.music && !(g.stage == 6 && gui_->frame() < 300)) res_.music->resume();
                if (hooks_.playTimeRebase) hooks_.playTimeRebase();
                break;
            case InGameMenu::Request::Continue:
                applyContinue();
                if (res_.music) res_.music->resume();
                if (hooks_.playTimeRebase) hooks_.playTimeRebase();
                break;
            case InGameMenu::Request::Title:
                if (hooks_.playTimeRebase) hooks_.playTimeRebase();
                outcome_ = Outcome::Title;
                break;
            case InGameMenu::Request::Results:
                // A replay's game over ends the playback (spec 2.4.2).
                if (playback_) {
                    outcome_ = Outcome::ReplayEnd;
                    break;
                }
                g.guiScore = g.score;
                if (hooks_.playTimeRebase) hooks_.playTimeRebase();
                outcome_ = Outcome::Results;
                break;
            case InGameMenu::Request::Restart:
                if (hooks_.playTimeRebase) hooks_.playTimeRebase();
                outcome_ = Outcome::Restart;
                break;
            case InGameMenu::Request::None: return false;
        }
    } else if (outcome_ != Outcome::None) {
        return false;  // waiting for the supervisor
    } else {
        gui_->asciiUpdate(world_);  // the popups (no menu up)
    }

    // The game manager (priority 2). 1: the pause check (not in the demo, nor in the pass
    // after a slow-mode skip).
    if (!menu_.active() && !demo_ && !slowSkip_ && ((pressed & BtnMenu) || pauseRequested_)) {
        pauseRequested_ = false;
        menu_.openPause(world_);
        wasPaused_ = true;
        if (res_.music && !(g.stage == 6 && gui_->frame() < 300)) res_.music->pause();
        if (hooks_.playTimeAdd) hooks_.playTimeAdd();
        return false;
    }
    // 3: playback mode 1 reproduces the recorded slowdown by stopping passes (no input is
    // consumed), outside dialogue.
    if (playback_ && playbackMode_ == 1 && !g.dialogue) {
        ++replayLag_;
        const int fps = feed_.fps();
        const bool stop = fps < 20   ? replayLag_ % 3 != 0
                          : fps < 30 ? replayLag_ % 2 != 0
                          : fps < 40 ? replayLag_ % 3 == 0
                          : fps < 50 ? replayLag_ % 6 == 0
                                     : false;
        if (stop) return false;
    }
    // 4: the attract demo: any press, or its end frame, returns to the title; a fade to
    // black and a music fade 120 frames before the end.
    if (demo_) {
        if (anyPressed || pauseRequested_) {
            pauseRequested_ = false;
            outcome_ = Outcome::Title;
            return false;
        }
        static constexpr std::int32_t kFade[3] = {8100, 7020, 4620}, kEnd[3] = {8220, 7140, 4740};
        ++demoFrame_;
        if (demoFrame_ == kFade[demoIndex_]) {
            screenFx_.fade(2, 120, 0x000000u);
            if (res_.music) res_.music->fadeOut(3.0f);
        }
        if (demoFrame_ >= kEnd[demoIndex_]) {
            outcome_ = Outcome::Title;
            return false;
        }
    }
    // 7: a menu up (the continue menu opened last pass) stops the pass.
    if (menu_.active()) return false;
    // 8: the score roll; 9: slow mode (the configuration copy) skips passes with many bullets.
    g.rollScore();
    slowSkip_ = false;
    if (g.slowMode) {
        ++slowLag_;
        const int n = bullets_->liveBullets();
        if ((n >= 320 && slowLag_ % 3 == 0) || (n >= 224 && n < 320 && slowLag_ % 4 == 0) || (n >= 128 && n < 224 && slowLag_ % 5 == 0)) {
            slowSkip_ = true;
            return false;
        }
        if (n < 128) slowLag_ = 0;
    }
    // 10: the stage frame counter.
    ++g.stageFrame;
    // Replay playback's feed (priority 5): this frame's game input is the next entry.
    if (playback_ && !g.slowMode) {
        prevGameInput_ = gameInput_;
        gameInput_ = feed_.next();
    }
    // The recorder's reset hook (priority 6): the event flags, and "paused" after a pause.
    g.eventFlags = 0;
    if (recorder_ && wasPaused_) g.eventFlags |= kReplayEventPaused;
    wasPaused_ = false;
    // Stage background (7), player (8), enemies (10), effects (11), bullets and items (12),
    // GUI (13), screen effects (15): the player, bombs and dialogue read the game input.
    const std::uint16_t input = gameInput_;
    background_.update(*res_.anm, *res_.rng);
    player_->update(world_, input);
    if (player_->continueMenuRequested()) {
        player_->clearContinueRequest();
        menu_.openContinue(world_, setup_.maxContinues);
        if (res_.music) res_.music->pause();
        if (hooks_.playTimeAdd) hooks_.playTimeAdd();
    }
    enemies_->update(world_);
    if (g.stageFrame == 30 && testDialogue_ >= 0) gui_->startDialogue(testDialogue_);
    if (g.stageFrame == 30 && testSpell_ >= 0) gui_->spellCardStart(testSpell_, -1, "test");
    if (testPatterns_ && g.stageFrame % 60 == 30) {
        const int k = static_cast<int>(g.stageFrame / 60) % 11;
        Shooter s;
        s.type = static_cast<std::int16_t>(k);
        s.color = static_cast<std::int16_t>(g.stageFrame / 60 % 8);
        s.pos = {192.0f, 120.0f, 0.0f};
        s.count1 = 24;
        s.count2 = 2;
        s.speed1 = 2.0f;
        s.speed2 = 1.2f;
        s.aimMode = 2;
        s.flags = 0x4;
        bullets_->fire(world_, s);
        for (int i = 0; i < 4; ++i) {
            bullets_->spawn({96.0f + 64.0f * static_cast<float>(i), 80.0f, 0.0f}, (k + i) % 10, ItemField::Normal);
        }
    }
    effects_.update(world_);
    bullets_->update(world_);
    gui_->update(world_, input, prevGameInput_);
    switch (gui_->endRequest()) {
        case Gui::EndRequest::Results: outcome_ = Outcome::Results; break;
        case Gui::EndRequest::Ending: outcome_ = Outcome::Ending; break;
        case Gui::EndRequest::ReplayEnd: outcome_ = Outcome::ReplayEnd; break;
        default: break;
    }
    drawState_.offsetX = drawState_.offsetY = 0.0f;
    screenFx_.update(*res_.rng, g, drawState_.offsetX, drawState_.offsetY);
    // The recorder's record hook (priority 16), after all gameplay: the game input is latched
    // (even in slow mode), then the raw input and the event flags are written. Its "menu up"
    // test reads the game manager's step 6 flag, so the frame in which the player opens the
    // continue menu is still recorded (ZUN's demo files show it).
    if (!playback_) {
        prevGameInput_ = gameInput_;
        gameInput_ = raw;
        if (recorder_ && !g.slowMode) recorder_->write(raw, static_cast<std::uint16_t>(g.eventFlags));
    }
    if (playback_ && onPlaybackFrame) onPlaybackFrame(g.stage, feed_.frame(), feed_.upcoming(), g.eventFlags);
    // The playback's fast-forward hook (priority 17).
    return playback_ && outcome_ == Outcome::None && !menu_.active() && fastForward();
}

// Spec 7.8.6: dialogue (skippable) runs three frames per displayed frame in every playback
// mode; mode 2 runs five outside boss fights.
bool Gameplay::fastForward() const {
    const std::int32_t f = feed_.frame();
    if (gui_->dialogueCurrent() && gui_->dialogueSkippable() && f % 3 != 2) return true;
    if (playbackMode_ == 2 && !state_.boss.present && f % 5 != 4) return true;
    return false;
}

void Gameplay::draw(SpriteSink& sink) {
    if (!running_) return;
    background_.stageFinished = gui_->stageFinished();
    background_.drawHigh(sink, drawState_);
    background_.drawLow(sink, drawState_);
    enemies_->drawLow(sink, drawState_);
    player_->drawLow(sink, drawState_);
    enemies_->drawHigh(sink, drawState_);
    player_->drawHigh(sink, drawState_);
    effects_.draw(sink, drawState_);
    bullets_->draw(sink, drawState_);
    gui_->drawPopups(sink);
    gui_->draw(sink, drawState_);
    gui_->drawText(sink);
    menu_.draw(sink);
    gui_->drawDemoMarker(sink);
    if (res_.ui) screenFx_.draw(sink, res_.ui->white);
}

void Gameplay::applyContinue() {
    GameState& g = state_;
    const World& w = world_;
    ++g.continues;
    g.score = g.guiScore = static_cast<std::uint32_t>(g.continues);
    g.rollSpeed = 0;
    w.setLives(static_cast<float>(g.lifeCount));
    w.setBombs(static_cast<float>(static_cast<int>(player_->header().initialBombs)));
    g.stageGraze = g.stagePointItems = g.extendPoints = 0;
    w.setPower(0.0f);
    g.extendsGranted = 0;
    g.nextExtend = 50;
    g.cherry = 0;
    if (hooks_.continued) hooks_.continued(g);
}

bool Gameplay::stageAdvance(std::string* error) {
    if (!running_ || !gui_->nextStageRequested()) return true;
    gui_->clearNextStageRequest();
    return nextStage(error);
}

}  // namespace touhou::th07
