#include "th08/Gameplay.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bgm.hpp"
#include "th08/GameConfig.hpp"
#include "th08/SoundPlayer.hpp"

namespace touhou::th08 {

namespace {

// The stage effect files (table 0x4C7480 by stage index).
const char* effectAnm(int stage) {
    static const char* const kNames[kStageCount] = {"eff01.anm", "eff02.anm", "eff03.anm", "eff04a.anm", "eff04b.anm",
                                                    "eff05.anm", "eff06.anm", "eff07.anm", "eff08.anm"};
    return kNames[std::clamp(stage, 0, kStageCount - 1)];
}

// The boss music table names its files without the folder.
std::string bossMusicPath(const ExeTables::BossMusic& m) { return m.file.find('/') == std::string::npos ? "bgm/" + m.file : m.file; }

// Spell practice's boss face (spec_spell_practice 3.3).
const char* spellFace(int stage, int card) {
    switch (stage) {
        case 0: return "face_st01sp.anm";
        case 1: return "face_st02sp.anm";
        case 2: return "face_st03sp.anm";
        case 3:
            switch (card) {
                case 216: return "face_sksp.anm";
                case 217: return "face_ymsp.anm";
                case 218: return "face_alsp.anm";
                case 219: return "face_rssp.anm";
                case 220: return "face_yysp.anm";
                case 221: return "face_yksp.anm";
                default: return "face_st04asp.anm";
            }
        case 4: return "face_st04bsp.anm";
        case 5: return card == 212 ? "face_st05msp.anm" : "face_st05sp.anm";
        case 6: return "face_st06sp.anm";
        case 7: return card >= 147 && card <= 150 ? "face_st06sp.anm" : "face_st07sp.anm";
        default: return (card >= 191 && card <= 193) || card == 213 ? "face_st08msp.anm" : "face_st08sp.anm";
    }
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
    g.timeOrbsNet += n;
    g.timePoints += n;
    g.timeTotal += n;
    if (rng) antiTamper(*rng);
    if (n > 0) g.pointValue += ((n + (g.timeTotal & 1)) / 2) * 10;
}

Gameplay::~Gameplay() { end(); }

bool Gameplay::takeCaptureRequest() {
    if (captureOwner_ == CaptureOwner::None) {
        if (menu_.takeCaptureRequest()) captureOwner_ = CaptureOwner::Menu;
        else if (gui_.takeCaptureRequest()) captureOwner_ = CaptureOwner::Gui;
    }
    return captureOwner_ != CaptureOwner::None;
}

void Gameplay::captureRect(int& x, int& y, int& w, int& h) const {
    x = y = w = h = 0;
    if (captureOwner_ == CaptureOwner::Menu) menu_.captureRect(x, y, w, h);
    else if (captureOwner_ == CaptureOwner::Gui) gui_.captureRect(x, y, w, h);
}

void Gameplay::setCaptureReady(bool ready) {
    if (captureOwner_ == CaptureOwner::Menu) menu_.setCaptureReady(ready);
    else if (captureOwner_ == CaptureOwner::Gui) gui_.setCaptureReady(ready);
    captureOwner_ = CaptureOwner::None;
}

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
    if (state_.spellPractice) {
        if (!lib.load(*ctx_.vfs, *ctx_.textures, spellFace(state_.stage, state_.card), Dialogue::kSlotFaceBoss, error)) return false;
        bossFacesLoaded_ = true;
    } else {
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
    if (ctx_.music && !keepMusic_) ctx_.music->stop();
    running_ = false;
}

const ExeTables::BossMusic* Gameplay::bossMusic() const {
    if (!ctx_.tables || !state_.spellPractice) return nullptr;
    for (const ExeTables::BossMusic& m : ctx_.tables->bossMusic) {
        if (m.lastCard >= state_.card) return &m;
    }
    return nullptr;
}

bool Gameplay::start(const Setup& setup, std::string* error) {
    end();
    setup_ = setup;
    replay_ = setup.replay;
    slowCounter_ = 0;
    replayFps_ = 60;
    replayFpsFlag_ = false;
    fpsText_.clear();
    fpsTextTimer_ = 0;
    demoVm_ = AnmVm{};
    demoVmOn_ = false;
    demoCounter_ = 0;
    slowLag_ = 0;
    slowSkip_ = false;
    if (replay_) {  // (character, difficulty and card from the file; the practice flag clear)
        const ReplaySummary& sm = replay_->summary;
        setup_.character = std::clamp(sm.character(), 0, 11);
        setup_.difficulty = std::clamp(sm.difficulty(), 0, 4);
        setup_.practice = false;
        setup_.spellPractice = sm.card() >= 0;
        setup_.card = sm.card();
        setup_.lives = sm.config()[0x1C];
        if (setup_.stage < 0 || !replay_->hasStage(setup_.stage)) setup_.stage = replay_->firstStage();
    }
    const Setup& s0 = setup_;
    // A new game starts a new recording; a retry records the stage again under the same
    // summary (spec_replays 2.3).
    if (replay_ || !retrying_) {
        record_ = ReplayFile{};
        recordValid_ = false;
    }
    retrying_ = false;
    outcome_ = Outcome::None;
    stageEnd_ = 0;
    pending_ = Pending::None;
    // Fresh set-up (spec 6.2 step 3).
    state_ = GameState{};
    state_.stage = std::clamp(s0.stage, 0, kStageCount - 1);
    state_.character = std::clamp(s0.character, 0, 11);
    state_.difficulty = std::clamp(s0.difficulty, 0, 4);
    state_.practice = !replay_ && (s0.practice || s0.spellPractice);  // (clear in every playback)
    state_.slowMode = !replay_ && !state_.practice && state_.difficulty < 4 && ctx_.config && ctx_.config->slowMode != 0;
    state_.spellPractice = s0.spellPractice;
    state_.card = s0.spellPractice ? std::clamp(s0.card, 0, 221) : -1;
    state_.nightSteps = state_.stage == 8 ? 6 : 0;  // (keyed on the stage index, spec_spell_practice 8.5)
    state_.lives = state_.practice ? 8.0f : state_.difficulty >= 4 ? 2.0f : static_cast<float>(s0.lives);
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
    // Step 3.8: the result screen's write-through (score.dat written, two game-RNG draws), then
    // the records this game reads: the hi-score, the cards (all tagged), the practice count.
    if (ctx_.saveScores) ctx_.saveScores(true);
    if (ScoreData* r = ctx_.records) {
        r->cardsTagged = true;
        for (CardRecord& c : r->cards) c.present = true;
        const auto slot = static_cast<std::size_t>(state_.stage * 5 + state_.difficulty);
        PracticeRecord& pr = r->practice[static_cast<std::size_t>(state_.character)];
        if (state_.practice && slot < pr.plays.size()) {
            state_.hiScore = pr.best[slot];
            ++pr.plays[slot];
            pr.hasData = true;
        } else {
            state_.hiScore = r->bestScore(state_.difficulty, state_.character);
        }
        // Step 9: the statistics (not in spell practice or a replay).
        if (!state_.spellPractice && !replay_) r->notePlay(state_.difficulty, state_.character, s0.restart, state_.practice);
    }
    if (!setUpStage(true, error)) {
        end();
        return false;
    }
    return true;
}

// ---- recording (spec_replays 2) --------------------------------------------------------------

void Gameplay::recordStageStart() {
    const int s = state_.stage;
    if (!recordValid_) {
        ReplaySummary& sm = record_.summary;
        sm.bytes.fill(0);
        sm.bytes[0x01] = 'd';
        sm.setCharacter(state_.character);
        sm.setDifficulty(state_.difficulty);
        sm.bytes[0x46] = 0x00;
        sm.bytes[0x47] = 0x01;
        sm.setExe(840704, static_cast<std::int32_t>(2724749753u), "0100d");
        sm.setPractice(state_.practice);
        sm.setCard(state_.spellPractice ? state_.card : -1);
        // The route byte (MSG 9.3): 2 with a 6B clear on E-L (no continue) or a solo character,
        // 1 with a 6A clear, else 0.
        int route = state_.character > 3 ? 2 : 0;
        if (ctx_.records && route == 0) {
            const ClearRecord& r = ctx_.records->clears[static_cast<std::size_t>(state_.character)];
            for (int d = 0; d < 4; ++d) {
                if (r.noContinue[static_cast<std::size_t>(d)] & 0x80) route = 2;
                else if ((r.noContinue[static_cast<std::size_t>(d)] & 0x40) && route < 1) route = 1;
            }
        }
        sm.setRouteByte(route);
        std::memcpy(sm.bytes.data() + 0x0A, "NO N", 4);  // (a placeholder until saved)
        // The game's configuration copy: the practice / Extra lives, slow mode forced off.
        std::vector<std::uint8_t> cfg = ctx_.config ? ctx_.config->save() : std::vector<std::uint8_t>(60, 0);
        cfg.resize(60, 0);
        cfg[0x1C] = static_cast<std::uint8_t>(state_.difficulty >= 4 ? 2 : state_.practice ? 8 : setup_.lives);
        if (state_.difficulty >= 4 || state_.practice) cfg[0x25] = 0;
        sm.setConfig(cfg);
        recordValid_ = true;
    } else {
        for (int t = s - 1; t >= 0; --t) {  // the score carried out of the last recorded stage
            if (!record_.hasStage(t)) continue;
            record_.stages[static_cast<std::size_t>(t)].carriedScore = state_.score;
            break;
        }
    }
    ReplayStage& b = record_.stages[static_cast<std::size_t>(s)];
    b = ReplayStage{};
    b.present = true;
    b.pointItems = state_.pointItems;
    b.graze = state_.graze;
    b.extends = state_.extends;
    b.nextExtend = state_.nextExtend;
    b.pointValue = state_.pointValue;
    b.gauge = state_.gauge;
    b.seed = stageSeed_;
    b.power = static_cast<std::uint8_t>(state_.power);
    b.lives = static_cast<std::uint8_t>(state_.lives);
    b.bombs = static_cast<std::uint8_t>(state_.bombs);
    b.rank = static_cast<std::uint8_t>(state_.rank);
    b.cardsCaptured = static_cast<std::uint8_t>(state_.cardsCaptured);
    b.nightSteps = static_cast<std::int8_t>(state_.nightSteps);
    b.entries.assign(1, 0);
    recordFrames_ = 0;
    resultsEntries_ = 0;
}

// One recorded pass (the end of a pass that ran the game): the raw input word, an FPS sample
// every 30 entries; nothing in slow mode or more than 3 passes after the stage results.
void Gameplay::recordPass(std::uint16_t raw) {
    if (!recordValid_ || !record_.hasStage(state_.stage)) return;
    if (record_.summary.slowModeRecorded()) return;
    if (world_.mode & ModeResults) {
        if (resultsEntries_ >= 3) return;
        ++resultsEntries_;
    }
    ReplayStage& b = record_.stages[static_cast<std::size_t>(state_.stage)];
    if (b.entries.size() >= kReplayMaxEntries) return;
    b.entries.push_back(raw);
    if (recordFrames_ % 30 == 0) {
        const std::size_t i = recordFrames_ / 30;
        b.fps.resize(i + 2);
        b.fps[i] = 60;
        b.fps[i + 1] = 60;
    }
    ++recordFrames_;
}

bool Gameplay::runAgain() const {
    if (!replay_ || !running_ || menu_.active() || stageEnd_ != 0 || outcome_ != Outcome::None) return false;
    if (world_.mode & ModeResults) return false;
    if (dialogue_.running() && dialogue_.skippable() && replayFrame_ % 3 != 2) return true;
    return setup_.playbackMode == 2 && !enemies_->bossPresent() && replayFrame_ % 5 != 4;
}

void Gameplay::closeRecording() {
    if (recordValid_ && record_.hasStage(state_.stage)) record_.stages[static_cast<std::size_t>(state_.stage)].entries.push_back(0);
}

// spec_replays 3.2 step 4 and 6: the stage header's state, the seed last; the score carried
// from the previous recorded stage.
void Gameplay::restoreReplayStage() {
    if (!replay_ || !replay_->hasStage(state_.stage) || !ctx_.rng) return;
    const ReplayStage& h = replay_->stages[static_cast<std::size_t>(state_.stage)];
    state_.pointItems = h.pointItems;
    state_.rank = h.rank;
    state_.lives = static_cast<float>(h.lives);
    state_.bombs = static_cast<float>(h.bombs);
    state_.power = static_cast<float>(h.power);
    state_.graze = h.graze;
    state_.pointValue = h.pointValue;
    state_.cardsCaptured = h.cardsCaptured;
    state_.extends = h.extends;
    state_.nextExtend = h.nextExtend;
    state_.gauge = h.gauge;  // (the display copy is not updated [QUIRK])
    state_.nightSteps = h.nightSteps;
    ctx_.rng->seed(h.seed);
    for (int t = state_.stage - 1; t >= 0; --t) {
        if (!replay_->hasStage(t)) continue;
        state_.score = state_.guiScore = replay_->stages[static_cast<std::size_t>(t)].carriedScore;
        break;
    }
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
    if (state_.spellPractice) state_.power = state_.card <= 1 ? 30.0f : state_.card <= 12 ? 80.0f : 128.0f;
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
    world_.english = ctx_.english;
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
    gui_.setBackground(&background_);
    world_.mode = (state_.practice ? ModePractice : 0u) | (state_.spellPractice ? ModeSpellPractice : 0u) | (replay_ ? ModeReplay : 0u);
    // The player (registered before the stage: its shot files every stage, its ANM once).
    if (!player_->load(*ctx_.vfs, *ctx_.textures, lib, state_.character, fresh, error)) return false;
    // Steps 7-8 (spec_replays 3.2): a replay restores the stage's recorded state and its RNG
    // seed; then the seed is recorded. Every draw of the stage managers comes after it.
    replayFrame_ = 0;
    fpsIndex_ = 0;
    rawPending_ = 0;
    gameInput_ = prevGameInput_ = 0;  // (game input cleared at every stage start, spec 1.1)
    if (replay_) restoreReplayStage();
    stageSeed_ = rng.currentSeed();
    if (!replay_) recordStageStart();
    // Step 9: Stage, the bullet manager's etama.anm, the effect manager's file, the Gui.
    background_.white = ctx_.ui ? &ctx_.ui->white : nullptr;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, lib, rng, state_.stage, state_.spellPractice, error)) return false;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "etama.anm", kSlotEtama, error)) return false;
    etamaLoaded_ = true;
    if (!enemies_->stageStart(world_, *ctx_.vfs, *ctx_.textures, state_.stage, state_.spellPractice, error)) return false;
    {
        std::string eff = effectAnm(state_.stage);
        if (state_.spellPractice && state_.card >= 216 && ctx_.tables && !ctx_.tables->lastWordEffects[static_cast<std::size_t>(state_.card - 216)].empty()) {
            eff = ctx_.tables->lastWordEffects[static_cast<std::size_t>(state_.card - 216)];
        }
        if (!lib.load(*ctx_.vfs, *ctx_.textures, eff, kSlotEffects, error)) return false;
    }
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
    if (ctx_.rng) gui_.hudStageStart(*ctx_.rng, state_, fresh);  // (AM init, after the Player)
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
    Spellcard::Context sc;
    sc.anm = &lib;
    sc.textures = ctx_.textures;
    sc.text = ctx_.text;
    sc.cards = ctx_.records ? &ctx_.records->cards : cards_.get();
    spellcard_.stageStart(world_, sc);
    dialogue_.onScriptZero = [this](int) {
        if (state_.stage >= 6) background_.setSpellBackgrounds(2, 2);
        // The second boss's name (spec_stage_gui 3.6): Stage 5, 6B and Extra.
        if (state_.stage == 5) gui_.copyBossName(22);
        else if (state_.stage == 7) gui_.copyBossName(24);
        else if (state_.stage == 8) gui_.copyBossName(25);
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
    musicStart_ = 1;
    if (const ExeTables::BossMusic* m = bossMusic()) {
        // Spell practice (spec_spell_practice 3.4): the card's boss music in slot 0, kept
        // playing over a retry unless the record says otherwise.
        if (keepMusic_) {
            if (ctx_.music) ctx_.music->resume();
            musicStart_ = 2;
        } else if (ctx_.music) {
            ctx_.music->load(0, bossMusicPath(*m));
        }
        // The spell-background base of 6A / 6B / Extra (spec 3.2).
        const int c = state_.card;
        if (state_.stage >= 6 && !(c >= 119 && c <= 122) && !(c >= 147 && c <= 150) && !(c >= 191 && c <= 193) && c != 213) {
            background_.setSpellBackgrounds(2, 2);
        }
    } else if (ctx_.music) {
        for (int k = 0; k < 3; ++k) {
            const std::string& song = background_.songPath(k);
            if (!song.empty() && song[0] != ' ') ctx_.music->load(k, song);
        }
    }
    keepMusic_ = false;
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
        case R::Title:  // (the write-through keeps this run's records, spec_result_ending 1.5)
            if (ctx_.saveScores) ctx_.saveScores(true);
            outcome_ = Outcome::Title;
            break;
        case R::Results:
            outcome_ = Outcome::Results;
            break;
        case R::BadEnding:
            state_.finished = false;
            outcome_ = Outcome::Ending;
            break;
        case R::Restart: {  // the main game from Stage 1, fresh
            Setup s = setup_;
            s.stage = 0;
            s.restart = true;
            std::string err;
            if (!start(s, &err)) outcome_ = Outcome::Title;
            break;
        }
        case R::Retry: {  // the same stage again, under the old frame's tiles
            if (const ExeTables::BossMusic* m = bossMusic()) keepMusic_ = m->restart == 0;
            retrying_ = true;
            std::string err;
            if (!start(setup_, &err)) outcome_ = Outcome::Title;
            else if (ctx_.rng) gui_.startTransition(*ctx_.rng);
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
            if (ctx_.records) ctx_.records->noteContinue(state_.difficulty, state_.character);
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
        if (replay_) {  // playback: the next recorded stage, else the end (spec_replays 3.5)
            state_.guiScore = state_.score;
            const int next = state_.stage <= 5 ? replay_->nextStage(state_.stage) : -1;
            if (next < 0) {
                outcome_ = Outcome::ReplayEnd;
                return true;
            }
            std::string err;
            const std::uint32_t route = world_.mode & ModeFinalB;
            releaseStageFiles();
            state_.stage = next;
            if (!setUpStage(false, &err)) {
                log::warn("th08: the replay's next stage could not start: {}", err);
                outcome_ = Outcome::ReplayEnd;
                return true;
            }
            world_.mode |= route;
            stageEnd_ = 0;  // ("press Shot" is auto-accepted in a replay)
            return true;
        }
        // The stage's bit in the clear records (no-continue only while no continue was used).
        if (ctx_.records) ctx_.records->markClear(state_.character, state_.difficulty, static_cast<std::uint16_t>(1u << state_.stage), state_.continues == 0);
        // Port: written to score.dat at once (the original waits for the result screen or the
        // program's exit, which a phone may never reach); the port's RNG, not the game's.
        if (ctx_.saveScores) ctx_.saveScores(false);
        state_.guiScore = state_.score;
        if (state_.practice) {
            pending_ = Pending::Results;
            return true;
        }
        if (state_.stage <= 5) {
            if (state_.nightSteps >= 12) {  // dawn: the bad ending
                state_.finished = false;
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
        } else if (state_.difficulty >= 4) {
            // Extra: 0x8000 into the character's no-continue Extra word, but into record 12's
            // reached word [QUIRK]; the Extra clear count (difficulty row only).
            if (ScoreData* r = ctx_.records; r && state_.difficulty == 4) {
                ClearRecord& mine = r->clears[static_cast<std::size_t>(state_.character)];
                mine.noContinue[4] = static_cast<std::uint16_t>(mine.noContinue[4] | 0x8000);
                r->clears[12].reached[4] = static_cast<std::uint16_t>(r->clears[12].reached[4] | 0x8000);
                r->plstAdd(4, ScoreData::Clears);
            }
            state_.finished = true;
            pending_ = Pending::Results;
            return true;
        } else {
            state_.finished = true;
            pending_ = Pending::Ending;
            return true;
        }
    }
    if (in.pressed(BtnShoot | BtnEnter) || state_.stage >= 6) {
        stageEnd_ = 0;
        const Pending p = pending_;
        pending_ = Pending::None;
        if (p == Pending::Results) outcome_ = Outcome::Results;
        else if (p == Pending::Ending || p == Pending::BadEnding) outcome_ = Outcome::Ending;
        return true;
    }
    return true;
}

void Gameplay::tick(const MenuInput& in) {
    if (!running_ || outcome_ != Outcome::None) return;
    ++state_.runFrames;  // (the GameManager's frame counter: loading, pauses and dialogue too)
    if (stageEndStep(in)) return;
    // The AsciiManager's HUD VMs tick every frame, also while paused; in a demo its marker VM
    // starts on its first update (a start runs one tick, then the update's tick).
    if (ctx_.rng) {
        gui_.hudTick(*ctx_.rng);
        if (setup_.demo && ctx_.anm) {
            if (!demoVmOn_) {
                demoVm_.startFresh(*ctx_.anm, *ctx_.rng, kSlotAscii, 11);
                demoVmOn_ = true;
            }
            demoVm_.tick(*ctx_.rng);
        }
    }
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
    // The pause check (step 4; never in a demo).
    if ((in.pressed(BtnMenu) || backPressed) && !setup_.demo && !slowSkip_) {
        if (ctx_.music) ctx_.music->pause();
        menu_.openPause(world_);
        ++state_.pauses;
        return;
    }
    // GameManager (priority 4): the music starts when play begins (spec 6.6 step 3).
    if (musicStart_ != 0) {
        // (the GUI start hook 0x438046)
        if (state_.spellPractice) gui_.bossNameForCard(state_.stage, state_.card);
        else gui_.bossNameForStage(state_.stage);
        const ExeTables::BossMusic* m = bossMusic();
        if (musicStart_ == 1) {
            if (ctx_.music) ctx_.music->playLoaded(0);
            if (m) ctx_.notePlayed(bossMusicPath(*m));  // (practice sets the Music Room byte too)
            else if (!state_.practice) ctx_.notePlayed(background_.songPath(0));
        }
        if (m && (musicStart_ == 1 || m->restart != 0) && ctx_.rng) gui_.musicCaption(*ctx_.rng, m->caption);
        musicStart_ = 0;
    }
    // Step 6: the recorded slowdown (playback mode 1, no dialogue): passes are skipped by the
    // replayed FPS sample (a skipped pass reads no entry).
    if (replay_ && setup_.playbackMode == 1 && !dialogue_.running()) {
        const std::uint32_t c = ++slowCounter_;
        const int f = replayFps_;
        const bool skip = f < 20 ? c % 3 != 0 : f < 30 ? c % 2 != 0 : f < 40 ? c % 3 == 0 : f < 50 ? c % 6 == 0 : false;
        if (skip) return;
    }
    // Step 7: the attract demo's clock (spec_replays 5.2): a 120-frame playfield fade and a 3 s
    // music fade at the fade frame; at the end frame back to the title, before the input feed.
    if (setup_.demo) {
        static constexpr int kFadeFrame[4] = {6000, 4800, 4920, 6900};
        const int fade = kFadeFrame[std::clamp(setup_.demoIndex, 0, 3)];
        ++demoCounter_;
        if (demoCounter_ == fade) {
            screen_.playfieldFadeTo(120, 0x000000u);
            if (ctx_.music) ctx_.music->fadeOut(3.0f);
        }
        if (demoCounter_ >= fade + 120) {
            outcome_ = Outcome::Title;
            return;
        }
    }
    state_.rollScore();  // (step 9)
    // Step 10: slow mode skips one pass in 3, 4 or 5 with 320, 224 or 128 bullets or more.
    slowSkip_ = false;
    if (state_.slowMode) {
        ++slowLag_;
        const int n = bullets_->liveBullets();
        if ((n >= 320 && slowLag_ % 3 == 0) || (n >= 224 && n < 320 && slowLag_ % 4 == 0) || (n >= 128 && n < 224 && slowLag_ % 5 == 0)) {
            slowSkip_ = true;
            return;
        }
        if (n < 128) slowLag_ = 0;
    }
    // The Supervisor (calc priority 0) counts the screen-effect suppression down.
    if (world_.screenSuppress != 0) --world_.screenSuppress;
    ZunRng& rng = *ctx_.rng;
    constexpr std::uint16_t kGameBits = BtnShoot | BtnBomb | BtnFocus | BtnUp | BtnDown | BtnLeft | BtnRight | BtnSkip;
    if (replay_) {
        // The playback feed (priority 6, spec_replays 3.3): entry k is frame k's input; frozen
        // once the stage results are shown; past the block's end, 0.
        if (!(world_.mode & ModeResults)) {
            const auto& e = replay_->stages[static_cast<std::size_t>(state_.stage)].entries;
            prevGameInput_ = gameInput_;
            gameInput_ = replayFrame_ < e.size() ? static_cast<std::uint16_t>(e[replayFrame_] & kGameBits) : 0;
            // The FPS sample, one early: frame 30j reads byte j + 1 (past the block: kept).
            if (replayFrame_ % 30 == 0) {
                const auto& fps = replay_->stages[static_cast<std::size_t>(state_.stage)].fps;
                if (fpsIndex_ + 1 < fps.size()) {
                    replayFps_ = fps[fpsIndex_ + 1] & 0x7F;
                    replayFpsFlag_ = (fps[fpsIndex_ + 1] & 0x80) != 0;
                }
                ++fpsIndex_;
            }
            ++replayFrame_;
        }
    } else {
        // Live play reads the raw input of the previous pass (the recorder sets the game input
        // at priority 17, after every gameplay object: a one-pass delay [QUIRK]).
        prevGameInput_ = gameInput_;
        gameInput_ = static_cast<std::uint16_t>((inputDelay_ ? rawPending_ : in.current()) & kGameBits);
    }
    world_.stageFrame = static_cast<std::int32_t>(stageFrame_);
    // Screen effects (3), Stage (8), Player (9), enemies (11, with the spell card 12),
    // effects (13), bullets (14), Gui (15).
    screen_.update(world_);
    background_.setYoukaiInFront(player_->youkaiInFront());
    background_.update(*ctx_.anm, rng, (world_.mode & ModeSpellFreeze) != 0);
    player_->update(world_, gameInput_);
    gui_.hudPlayerZone(player_->position());  // (inside the Player update, after its VM tick)
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
    if ((player_->continueRequested() || world_.continueRequested) && !menu_.active()) {
        world_.continueRequested = false;
        if (replay_) {  // a game over ends a playback at once (spec_replays 3.5)
            state_.guiScore = state_.score;
            outcome_ = Outcome::ReplayEnd;
            return;
        }
        if (ctx_.music) ctx_.music->pause();
        menu_.openContinue(world_);
    }
    // The recorder (priority 17): the game input of the next pass is this pass's raw input.
    if (!replay_) {
        rawPending_ = in.current();
        recordPass(in.current());
    }
    ++stageFrame_;
}

void Gameplay::draw(SpriteSink& sink) {
    if (!running_) return;
    if (stageEnd_ == 1) {
        // The next stage is loaded: "Press Shot Button" at half size over the last frame, its
        // alpha pulsing 255 -> 127 -> 255 over 60 frames (spec_spellcard_hud 11.3).
        const int phase = 5 + static_cast<int>(pressShotFrames_ % 60);
        const int alpha = phase < 35 ? 255 - (phase - 5) * 128 / 30 : 255 - (65 - phase) * 128 / 30;
        ++pressShotFrames_;
        ascii_.add(288.0f, 454.0f, "Press Shot Button", (static_cast<std::uint32_t>(alpha) << 24) | 0xFFFFFFu, 0.5f);
        if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);
        return;
    }
    pressShotFrames_ = 0;
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
    gui_.drawBlindness(sink, player_->position(), drawState_.offsetX, drawState_.offsetY);  // 14, after the popups
    drawState_.offsetX = drawState_.offsetY = 0.0f;
    sink.clearClip();
    gui_.drawGauge(sink, state_);             // 14 (the AsciiManager's popup pass)
    sink.setClip(kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH);
    spellcard_.draw(sink);                    // 15 (in the playfield viewport)
    sink.clearClip();
    dialogue_.draw(sink, ctx_.ui);            // (the Gui draws the MSG layer before the HUD)
    gui_.draw(sink, state_, ascii_);          // 17
    if (ctx_.anm) {
        ascii_.draw(sink, *ctx_.anm);  // 20
        gui_.drawBossMarkers(sink, player_->position());
        gui_.drawFamiliarReadout(sink, player_->position(), menu_.active(), (world_.mode & ModeSpellFreeze) != 0);
        gui_.drawMessages(sink, *ctx_.anm);
    }
    menu_.draw(sink);                          // (the AsciiManager's menus)
    if (demoVmOn_) drawVm(sink, demoVm_, AnmDrawState{});  // the "Demonstration" logo, last
    // The replayed FPS (spec_replay_menu 4.2): "%2d" at the play area's bottom-right corner,
    // pale yellow (red for a flagged sample), refreshed about every 500 ms while no menu is up.
    if (replay_ && !menu_.active() && ctx_.anm) {
        if (fpsTextTimer_ <= 0) {
            fpsText_ = std::format("{:2d}", replayFps_);
            fpsTextTimer_ = 30;
        }
        --fpsTextTimer_;
        ascii_.add(384.0f, 448.0f, fpsText_, replayFpsFlag_ ? 0xFFFF4040u : 0xFFFFFFD0u);
        ascii_.draw(sink, *ctx_.anm);
    }
    screen_.draw(sink, background_.white);    // 21: flashes
}

}  // namespace touhou::th08
