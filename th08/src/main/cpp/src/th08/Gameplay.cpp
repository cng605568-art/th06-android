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

bool World::spellCardActive() const { return spellcard && spellcard->active(); }

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

// The face files (spec_msg_stage_end 2.2): the decor and the two player faces once a game,
// the stage's boss faces every stage (none in spell practice yet).
bool Gameplay::loadFaces(bool fresh, std::string* error) {
    AnmLibrary& lib = *ctx_.anm;
    if (fresh || !facesLoaded_) {
        static const char* const kHuman[4] = {"face_rm00.anm", "face_mr00.anm", "face_sk00.anm", "face_ym00.anm"};
        static const char* const kYoukai[4] = {"face_yk00.anm", "face_al00.anm", "face_rs00.anm", "face_yy00.anm"};
        const int team = state_.character < 4 ? state_.character : (state_.character - 4) / 2;
        if (!lib.load(*ctx_.vfs, *ctx_.textures, "face_cdbg.anm", Dialogue::kSlotDecor, error)) return false;
        if (!state_.spellPractice) {
            if (!lib.load(*ctx_.vfs, *ctx_.textures, kHuman[team], Dialogue::kSlotFaceHuman, error)) return false;
            if (!lib.load(*ctx_.vfs, *ctx_.textures, kYoukai[team], Dialogue::kSlotFaceYoukai, error)) return false;
        }
        facesLoaded_ = true;
    }
    if (!state_.spellPractice) {
        static const char* const kBoss[9] = {"face_st01.anm", "face_st02.anm", "face_st03.anm", "face_st04a.anm", "face_st04b.anm",
                                             "face_st05.anm", "face_st06.anm", "face_st06.anm", "face_st08m.anm"};
        if (!lib.load(*ctx_.vfs, *ctx_.textures, kBoss[state_.stage], Dialogue::kSlotFaceBoss, error)) return false;
        const char* second = state_.stage == 5 ? "face_st05b.anm" : state_.stage == 7 ? "face_st07.anm" : state_.stage == 8 ? "face_st08.anm" : nullptr;
        if (second && !lib.load(*ctx_.vfs, *ctx_.textures, second, Dialogue::kSlotFaceBoss2, error)) return false;
        bossFacesLoaded_ = true;
    }
    return true;
}

void Gameplay::releaseStageFiles() {
    if (!ctx_.anm) return;
    if (bossFacesLoaded_) {
        ctx_.anm->release(Dialogue::kSlotFaceBoss);
        ctx_.anm->release(Dialogue::kSlotFaceBoss2);
        bossFacesLoaded_ = false;
    }
    dialogue_.release();
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
        if (facesLoaded_) {
            for (int slot : {Dialogue::kSlotDecor, Dialogue::kSlotFaceHuman, Dialogue::kSlotFaceYoukai}) ctx_.anm->release(slot);
            facesLoaded_ = false;
        }
    }
    if (ctx_.music) ctx_.music->stop();
    running_ = false;
}

bool Gameplay::start(const Setup& setup, std::string* error) {
    end();
    setup_ = setup;
    outcome_ = Outcome::None;
    stageEnd_ = 0;
    pending_ = Pending::None;
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
    world_.msg = &dialogue_;
    world_.gui = &gui_;
    world_.spellcard = &spellcard_;
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
    // The 6B moon (spec_stage_gui 1.7): special effect 64 in slot 12 running the stage ANM's
    // scripts 11 and 12; labels 2-4 interrupt it while keeping its movement and colour.
    moon_ = nullptr;
    background_.moonStep = [this](std::int32_t label) {
        AnmLibrary& lib = *ctx_.anm;
        ZunRng& rng = *ctx_.rng;
        if (!moon_) {
            moon_ = &effects_->spawnSpecial(world_, 64, {}, 12, 1, 0xFFFFFFFFu);
            moon_->vm.start(lib, rng, Background::kSlotBackground, 11);
            return;
        }
        AnmVm& vm = moon_->vm;
        if (label == 1) {
            vm.start(lib, rng, Background::kSlotBackground, 11);
        } else if (label >= 2 && label <= 4) {
            const AnmVm keep = vm;
            if (label == 2) vm.start(lib, rng, Background::kSlotBackground, 12);
            vm.interrupt(label);
            vm.posStart = keep.posStart;
            vm.posEnd = keep.posEnd;
            vm.progress[AnmVm::Position] = keep.progress[AnmVm::Position];
            vm.duration[AnmVm::Position] = keep.duration[AnmVm::Position];
            vm.ease[AnmVm::Position] = keep.ease[AnmVm::Position];
            vm.color = keep.color;
        }
    };
    background_.drawMoon = [this](SpriteSink& sink, AnmDrawState& st) {
        if (moon_ && moon_->inUse) Effects::drawRing(sink, *moon_, st);
    };
    if (!gui_.stageStart(ctx_, state_, fresh, error)) return false;
    // The dialogue (not in spell practice) and the faces (spec_msg_stage_end 2).
    if (!loadFaces(fresh, error)) return false;
    if (!state_.spellPractice) {
        Dialogue::Context dc;
        dc.anm = &lib;
        dc.textures = ctx_.textures;
        dc.text = ctx_.text;
        dc.tables = ctx_.tables;
        dc.scores = ctx_.scores;
        // Without th08.exe's table there is no dialogue: the timeline's op 6 does nothing and its
        // op 7 does not wait.
        std::string why;
        if (!dialogue_.load(dc, *ctx_.vfs, state_.stage, state_.character, &why)) {
            if (ctx_.tables) {
                if (error) *error = why;
                return false;
            }
            log::warn("th08: {}", why);
        }
    }
    // The spell card object (spec_spellcard_hud 1.1-1.2): zeroed and bound every stage.
    if (fresh) {
        if (ctx_.scores) *cards_ = ctx_.scores->cards;
        else cards_->fill(CardRecord{});
    }
    Spellcard::Context sc;
    sc.anm = &lib;
    sc.textures = ctx_.textures;
    sc.text = ctx_.text;
    sc.cards = cards_.get();
    spellcard_.stageStart(world_, sc);
    dialogue_.onScriptZero = [this](int) {
        if (state_.stage >= 6) background_.setSpellBackgrounds(2, 2);
    };
    dialogue_.onMusic = [this](int track) {
        if (!ctx_.music) return;
        if (track < 0) {
            ctx_.music->stop();
            return;
        }
        if (ctx_.rng) gui_.musicCaption(*ctx_.rng, track);
        ctx_.music->playLoaded(track);
        if (!state_.practice) ctx_.notePlayed(background_.songPath(track));
    };
    dialogue_.onResults = [this]() { gui_.stageResults(world_, state_); };
    dialogue_.onStageEnd = [this]() { stageCleared(); };
    dialogue_.onMusicFade = [this]() {
        if (ctx_.music) ctx_.music->fadeOut(4.0f, g_timing.speed);
    };
    dialogue_.onWhiteFade = [this]() { screen_.fadeTo(442, 0x00FFFFFFu); };
    dialogue_.onSound = [this](int id) { world_.playSound(id); };
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
            state_.rollSpeed = 0;
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

// The next stage (spec 6.4): Stage 3 goes to 4B for Reimu's, Yukari's, Youmu's and Yuyuko's
// shot types and to 4A for the others; Stage 5 to 6B when the dialogue chose Final B.
bool Gameplay::nextStage(std::string* error) {
    const int c = state_.character;
    int next = state_.stage + 1;
    if (state_.stage == 2) {
        const bool b = c == 0 || c == 3 || c == 4 || c == 5 || c == 10 || c == 11;
        next = b ? 4 : 3;
    } else if (state_.stage == 3 || state_.stage == 4) {
        next = 5;
    } else if (state_.stage == 5) {
        next = (world_.mode & ModeFinalB) ? 7 : 6;
    }
    const std::uint32_t route = world_.mode & ModeFinalB;
    releaseStageFiles();
    state_.stage = next;
    // Next-stage set-up (spec 6.2 step 4): the GUI score catches up; the stage's misses and
    // bombs-used counters are cleared (two anti-tamper regenerations, 8 steps).
    state_.guiScore = state_.score;
    if (ctx_.rng) {
        antiTamper(*ctx_.rng);
        antiTamper(*ctx_.rng);
    }
    if (!setUpStage(false, error)) return false;
    world_.mode |= route;  // (the route survives the stage set-up)
    return true;
}

// Stage end (spec 6.3), first thing in the GameManager's pass.
bool Gameplay::stageEndStep(const MenuInput& in) {
    if (stageEnd_ == 0) return false;
    if (stageEnd_ == 2) {
        stageEnd_ = 3;
        pending_ = Pending::None;
        state_.guiScore = state_.score;
        if (state_.practice) {
            pending_ = Pending::Results;
            return true;
        }
        if (state_.stage <= 5) {
            if (state_.nightSteps >= 12) {  // dawn: the bad ending
                pending_ = Pending::BadEnding;
                return true;
            }
            std::string err;
            if (!nextStage(&err)) {
                log::warn("th08: the next stage could not start: {}", err);
                outcome_ = Outcome::Title;
                return true;
            }
            stageEnd_ = 1;  // the new stage waits for Shoot ("Press Shot Button")
        } else {
            pending_ = state_.difficulty >= 4 ? Pending::Results : Pending::Ending;
            return true;
        }
    }
    if (in.pressed(BtnShoot | BtnEnter) || state_.stage >= 6) {
        stageEnd_ = 0;
        const Pending p = pending_;
        pending_ = Pending::None;
        // (The result screen and the endings are not ported yet: the title.)
        if (p != Pending::None) outcome_ = Outcome::Title;
        return true;
    }
    return true;
}

void Gameplay::tick(const MenuInput& in) {
    if (!running_ || outcome_ != Outcome::None) return;
    if (stageEndStep(in)) return;
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
    state_.rollScore();  // (step 9)
    // The Supervisor (calc priority 0) counts the screen-effect suppression down.
    if (world_.screenSuppress != 0) --world_.screenSuppress;
    ZunRng& rng = *ctx_.rng;
    prevGameInput_ = gameInput_;
    gameInput_ = in.current() & (BtnShoot | BtnBomb | BtnFocus | BtnUp | BtnDown | BtnLeft | BtnRight | BtnSkip);
    world_.stageFrame = static_cast<std::int32_t>(stageFrame_);
    // Screen effects (3), Stage (8), Player (9), enemies (11, with the spell card 12),
    // effects (13), bullets (14), Gui (15).
    screen_.update(world_);
    background_.setYoukaiInFront(player_->youkaiInFront());
    background_.update(*ctx_.anm, rng, (world_.mode & ModeSpellFreeze) != 0);
    player_->update(world_, gameInput_);
    enemies_->update(world_);
    spellcard_.update(world_);  // 12
    effects_->update(world_);
    bullets_->update(world_);
    // Gui (15): the HUD update with the results' bonus, stage end and clock count-up, then the
    // dialogue (nothing while time is stopped).
    gui_.setNullity(spellcard_.active() && ((world_.mode >> 7) & 3u) == 1u);
    gui_.update(rng, state_, player_->position(), world_.timeStop, dialogue_.running());
    gui_.bossTimerSounds(world_);
    if (!world_.timeStop) {
        const int lives = state_.practice ? 8 : state_.difficulty >= 4 ? 2 : setup_.lives;
        if (gui_.resultsStep(state_, gameInput_, lives, stageEnd_ == 0)) stageCleared();
        dialogue_.run(world_, gameInput_, prevGameInput_);
    }
    // Out of lives: the continue menu (spec_flow 7.3).
    if (player_->continueRequested() && !menu_.active()) {
        if (ctx_.music) ctx_.music->pause();
        menu_.openContinue(world_);
    }
    ++stageFrame_;
}

void Gameplay::draw(SpriteSink& sink) {
    if (!running_) return;
    if (stageEnd_ == 1) {  // the next stage is loaded: "Press Shot Button"
        if (ctx_.ui) sink.fillRect(ctx_.ui->white, 0.0f, 0.0f, 640.0f, 480.0f, rgba(0, 0, 0));
        ascii_.add(384.0f, 440.0f, "Press Shot Button", 0xFFFFFFFFu);
        if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);
        return;
    }
    drawState_ = AnmDrawState{};
    // The shake moves everything up to the popups; the HUD stays put (spec_anm_text 4.2).
    drawState_.offsetX = screen_.offsetX();
    drawState_.offsetY = screen_.offsetY();
    background_.stageFinished = gui_.stagePictureFinished();
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
    spellcard_.draw(sink);                    // 15 (in the playfield viewport)
    sink.clearClip();
    dialogue_.draw(sink, ctx_.ui);            // (the Gui draws the MSG layer before the HUD)
    gui_.draw(sink, state_, ascii_);          // 17
    if (ctx_.anm) {
        ascii_.draw(sink, *ctx_.anm);  // 20
        gui_.drawMessages(sink, *ctx_.anm);
    }
    menu_.draw(sink);                          // (the AsciiManager's menus)
    screen_.draw(sink, background_.white);    // 21: flashes
}

}  // namespace touhou::th08
