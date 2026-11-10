#include "th08/Gameplay.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bgm.hpp"

namespace touhou::th08 {

namespace {

// The stage effect files (table 0x4C7480 by stage index).
const char* effectAnm(int stage) {
    static const char* const kNames[kStageCount] = {"eff01.anm", "eff02.anm", "eff03.anm", "eff04a.anm", "eff04b.anm",
                                                    "eff05.anm", "eff06.anm", "eff07.anm", "eff08.anm"};
    return kNames[std::clamp(stage, 0, kStageCount - 1)];
}

}  // namespace

Gameplay::~Gameplay() { end(); }

void Gameplay::releaseStageFiles() {
    if (!ctx_.anm) return;
    background_.release(*ctx_.anm);
    if (effectsLoaded_) ctx_.anm->release(kSlotEffects);
    effectsLoaded_ = false;
}

void Gameplay::end() {
    if (!running_) return;
    releaseStageFiles();
    if (ctx_.anm) gui_.release(*ctx_.anm);
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
    // Step 9: Stage, then (the bullet and enemy managers to come) the effect manager's file,
    // then the Gui.
    background_.white = ctx_.ui ? &ctx_.ui->white : nullptr;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, lib, rng, state_.stage, state_.spellPractice, error)) return false;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, effectAnm(state_.stage), kSlotEffects, error)) return false;
    effectsLoaded_ = true;
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

void Gameplay::tick(const MenuInput& in) {
    if (!running_ || outcome_ != Outcome::None) return;
    // (The pause menu is not ported yet: Menu goes back to the title.)
    if (in.pressed(BtnMenu)) {
        outcome_ = Outcome::Title;
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
    // Stage (8) ... Gui (15).
    background_.update(*ctx_.anm, rng);
    gui_.update(rng, state_, Vec3f{192.0f, 384.0f, 0.0f});
    ++stageFrame_;
}

void Gameplay::draw(SpriteSink& sink) {
    if (!running_) return;
    drawState_ = AnmDrawState{};
    background_.stageFinished = false;
    background_.drawHigh(sink, drawState_);  // 6
    background_.drawLow(sink, drawState_);   // 7
    gui_.draw(sink, state_, ascii_);          // 17
    if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);  // 20
}

}  // namespace touhou::th08
