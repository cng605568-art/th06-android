#include "th08/Gameplay.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bgm.hpp"
#include "th08/SoundPlayer.hpp"

namespace touhou::th08 {

namespace {

// The stage effect files (table 0x4C7480 by stage index).
const char* effectAnm(int stage) {
    static const char* const kNames[kStageCount] = {"eff01.anm", "eff02.anm", "eff03.anm", "eff04a.anm", "eff04b.anm",
                                                    "eff05.anm", "eff06.anm", "eff07.anm", "eff08.anm"};
    return kNames[std::clamp(stage, 0, kStageCount - 1)];
}

}  // namespace

void World::playSound(int id) const {
    if (sound && id >= 0) sound->playSound(id);
}

void World::addTimePoints(std::int32_t n) const {
    GameState& g = *state;
    if (n < 0 && g.timePoints < -n) {
        g.timePoints = 0;
        return;
    }
    g.timePoints += n;
    g.timeTotal += n;
    if (rng) antiTamper(*rng);
    if (n > 0) g.pointValue += ((n + (g.timeTotal & 1)) / 2) * 10;
}

Gameplay::~Gameplay() { end(); }

void Gameplay::releaseStageFiles() {
    if (!ctx_.anm) return;
    background_.release(*ctx_.anm);
    if (effectsLoaded_) ctx_.anm->release(kSlotEffects);
    if (etamaLoaded_) ctx_.anm->release(kSlotEtama);
    enemies_->release(*ctx_.anm);
    effectsLoaded_ = etamaLoaded_ = false;
}

void Gameplay::end() {
    if (!running_) return;
    releaseStageFiles();
    if (ctx_.anm) {
        gui_.release(*ctx_.anm);
        player_->releaseAnm(*ctx_.anm);
    }
    if (ctx_.music) ctx_.music->stop();
    running_ = false;
}

bool Gameplay::start(const Setup& setup, std::string* error) {
    end();
    setup_ = setup;
    outcome_ = Outcome::None;
    // Fresh set-up (spec 6.2 step 3).
    state_ = GameState{};
    state_.stage = std::clamp(setup.stage, 0, kStageCount - 1);
    state_.character = std::clamp(setup.character, 0, 11);
    state_.difficulty = std::clamp(setup.difficulty, 0, 4);
    state_.practice = setup.practice || setup.spellPractice;
    state_.spellPractice = setup.spellPractice;
    state_.nightSteps = state_.difficulty >= 4 ? 6 : 0;
    state_.lives = state_.difficulty >= 4 ? 2.0f : state_.practice ? 8.0f : static_cast<float>(setup.lives);
    state_.bombs = 3.0f;
    state_.hiScore = 100000;
    static constexpr std::int32_t kRankStart[5] = {10, 10, 8, 8, 16};
    static constexpr std::int32_t kPointValue[5] = {60000, 100000, 200000, 300000, 300000};
    if (ctx_.tables) {
        const ExeTables::Rank& r = ctx_.tables->rank[static_cast<std::size_t>(state_.difficulty)];
        state_.rank = r.start, state_.minRank = r.min, state_.maxRank = r.max;
    } else {
        state_.rank = kRankStart[state_.difficulty];
    }
    state_.pointValue = kPointValue[state_.difficulty];
    if (!setUpStage(true, error)) {
        end();
        return false;
    }
    return true;
}

bool Gameplay::setUpStage(bool fresh, std::string* error) {
    if (!ctx_.vfs || !ctx_.textures || !ctx_.anm || !ctx_.rng) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx_.anm;
    ZunRng& rng = *ctx_.rng;
    running_ = true;
    stageFrame_ = 0;
    // Practice power (step 6): Stage 1 -> 0, Stage 2 -> 112, later -> 128.
    if (state_.practice && !state_.spellPractice) state_.power = state_.stage == 0 ? 0.0f : state_.stage == 1 ? 112.0f : 128.0f;
    // Per-stage counters: time points and their quota (spec_player 12.1), subrank, the
    // gauge frame counters, the next extend threshold.
    state_.timePoints = 0;
    state_.timeTotal = 0;
    state_.timeQuota = 0;
    if (ctx_.tables && !state_.spellPractice) {
        const std::size_t q = static_cast<std::size_t>(state_.stage * 4 + std::min(state_.difficulty, 4));
        if (q < ctx_.tables->timeQuota.size()) state_.timeQuota = ctx_.tables->timeQuota[q];
    }
    state_.subrank = 0;
    state_.stageFrames = state_.stageHuman80 = state_.stageYoukai80 = 0;
    state_.stageGraze = state_.stagePointItems = 0;
    if (ctx_.tables) {
        const std::int32_t k = state_.extends;
        state_.nextExtend = state_.difficulty >= 4 ? (k < 3 ? ctx_.tables->extendExtra[static_cast<std::size_t>(k)] : 99999)
                                                   : (k < 6 ? ctx_.tables->extendMain[static_cast<std::size_t>(k)] : 9999 + 500 * (k - 5));
    }
    screen_.reset();
    world_ = World{};
    world_.state = &state_;
    world_.rng = &rng;
    world_.anm = &lib;
    world_.tables = ctx_.tables;
    world_.sound = ctx_.sound;
    world_.effects = effects_.get();
    world_.player = player_.get();
    world_.background = &background_;
    world_.bullets = bullets_.get();
    world_.enemies = enemies_.get();
    world_.screen = &screen_;
    world_.mode = (state_.practice ? ModePractice : 0u) | (state_.spellPractice ? ModeSpellPractice : 0u);
    // The player (registered before the stage: its shot files every stage, its ANM once).
    if (!player_->load(*ctx_.vfs, *ctx_.textures, lib, state_.character, fresh, error)) return false;
    // Step 9: Stage, the bullet manager's etama.anm, the effect manager's file, the Gui.
    background_.white = ctx_.ui ? &ctx_.ui->white : nullptr;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, lib, rng, state_.stage, state_.spellPractice, error)) return false;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "etama.anm", kSlotEtama, error)) return false;
    etamaLoaded_ = true;
    if (!enemies_->stageStart(world_, *ctx_.vfs, *ctx_.textures, state_.stage, state_.spellPractice, error)) return false;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, effectAnm(state_.stage), kSlotEffects, error)) return false;
    effectsLoaded_ = true;
    effects_->reset();
    bullets_->stageStart(world_);
    player_->stageStart(world_);
    background_.spawnGlow = [this](const Vec3f& point) {
        Effect& e = effects_->spawn(world_, 62, point, 1, 0x20FFFFFFu);
        if (!effects_->isSink(e)) e.layer = 4;
    };
    background_.drawEffectsB = [this](SpriteSink& sink, AnmDrawState& st) { effects_->drawListB(sink, st, background_); };
    background_.onWeatherShift = [this](const Vec3f& delta) { effects_->shiftWeather(delta); };
    if (!gui_.stageStart(ctx_, state_, fresh, error)) return false;
    // Step 11: the stage's songs into slots 0-2 (blank names skipped); step 13: start them
    // when play begins.
    if (ctx_.music) {
        for (int k = 0; k < 3; ++k) {
            const std::string& song = background_.songPath(k);
            if (!song.empty() && song[0] != ' ') ctx_.music->load(k, song);
        }
    }
    musicStart_ = 1;
    log::info("th08: stage index {} for character {} on difficulty {}{}", state_.stage, state_.character, state_.difficulty,
              state_.practice ? " (practice)" : "");
    return true;
}

void Gameplay::menuDone(InGameMenu::Request request) {
    using R = InGameMenu::Request;
    switch (request) {
        case R::Resume:
            if (ctx_.music) ctx_.music->resume();
            break;
        case R::Title:
        case R::Results:    // (the result screen is not ported yet)
        case R::BadEnding:  // (nor the endings)
            outcome_ = Outcome::Title;
            break;
        case R::Restart: {  // the main game from Stage 1, fresh
            Setup s = setup_;
            s.stage = 0;
            std::string err;
            if (!start(s, &err)) outcome_ = Outcome::Title;
            break;
        }
        case R::Retry: {  // the same stage again
            std::string err;
            if (!start(setup_, &err)) outcome_ = Outcome::Title;
            break;
        }
        case R::Continue: {
            // Spec_flow 7.3, pass 90: the score becomes the continue count; lives, bombs,
            // power and the extend counters start again.
            ++state_.continues;
            state_.score = static_cast<std::uint32_t>(state_.continues);
            state_.guiScore = state_.score;
            state_.lives = static_cast<float>(setup_.lives);
            state_.bombs = static_cast<float>(static_cast<int>(player_->header().bombsPerLife));
            state_.power = 0.0f;
            state_.pointItems = 0;
            state_.extends = 0;
            state_.nextExtend = 100;
            player_->continueGame();
            if (ctx_.music) ctx_.music->resume();
            break;
        }
        case R::None: break;
    }
}

void Gameplay::tick(const MenuInput& in) {
    if (!running_ || outcome_ != Outcome::None) return;
    // An in-game menu stops the simulation (spec_flow 6.6 step 8).
    const bool backPressed = pauseRequested_;
    pauseRequested_ = false;
    if (menu_.active()) {
        std::uint16_t pressed = backPressed && menu_.pauseMenu() ? BtnMenu : 0;
        for (std::uint32_t b = 1; b <= 0x8000u; b <<= 1) {
            if (in.pressed(static_cast<std::uint16_t>(b))) pressed |= static_cast<std::uint16_t>(b);
        }
        menuDone(menu_.update(world_, pressed));
        return;
    }
    // The pause check (step 4).
    if (in.pressed(BtnMenu) || backPressed) {
        if (ctx_.music) ctx_.music->pause();
        menu_.openPause(world_);
        return;
    }
    // GameManager (priority 4): the music starts when play begins (spec 6.6 step 3).
    if (musicStart_ != 0) {
        if (musicStart_ == 1) {
            if (ctx_.music) ctx_.music->playLoaded(0);
            if (!state_.practice) ctx_.notePlayed(background_.songPath(0));
        }
        musicStart_ = 0;
    }
    ZunRng& rng = *ctx_.rng;
    gameInput_ = in.current() & (BtnShoot | BtnBomb | BtnFocus | BtnUp | BtnDown | BtnLeft | BtnRight);
    world_.stageFrame = static_cast<std::int32_t>(stageFrame_);
    // Screen effects (3), Stage (8), Player (9), enemies (11, with the spell card 12),
    // effects (13), bullets (14), Gui (15).
    screen_.update(world_);
    background_.setYoukaiInFront(player_->youkaiInFront());
    background_.update(*ctx_.anm, rng, (world_.mode & ModeSpellFreeze) != 0);
    world_.spellCardActive = enemies_->spellCard().active;
    player_->update(world_, gameInput_);
    enemies_->update(world_);
    world_.spellCardActive = enemies_->spellCard().active;
    effects_->update(world_);
    bullets_->update(world_);
    gui_.update(rng, state_, player_->position(), world_.timeStop);
    state_.guiScore = std::max(state_.guiScore, state_.score * 10);
    // Out of lives: the continue menu (spec_flow 7.3).
    if (player_->continueRequested() && !menu_.active()) {
        if (ctx_.music) ctx_.music->pause();
        menu_.openContinue(world_);
    }
    ++stageFrame_;
}

void Gameplay::draw(SpriteSink& sink) {
    if (!running_) return;
    drawState_ = AnmDrawState{};
    // The shake moves everything up to the popups; the HUD stays put (spec_anm_text 4.2).
    drawState_.offsetX = screen_.offsetX();
    drawState_.offsetY = screen_.offsetY();
    background_.stageFinished = false;
    background_.drawHigh(sink, drawState_);  // 6
    background_.drawLow(sink, drawState_);   // 7 (with effect list B)
    sink.setClip(kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH);
    enemies_->drawLayers(sink, drawState_, 0, 1);  // 8
    player_->drawShots(sink, drawState_);     // 9
    player_->drawPlayer(sink, drawState_);    // 10
    enemies_->drawLayers(sink, drawState_, 2, 3);  // 11
    effects_->draw(sink, drawState_, &background_);  // 12
    bullets_->draw(sink, drawState_);         // 13: lasers, items, bullets
    effects_->drawListD(sink, drawState_);     // (inside the bullet draw)
    drawState_.offsetX = drawState_.offsetY = 0.0f;
    sink.clearClip();
    gui_.draw(sink, state_, ascii_);          // 17
    if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);  // 20
    menu_.draw(sink);                          // (the AsciiManager's menus)
    screen_.draw(sink, background_.white);    // 21: flashes
}

}  // namespace touhou::th08
