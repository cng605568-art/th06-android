#include "th07/Gameplay.hpp"

#include <format>

#include "core/Log.hpp"
#include "io/Vfs.hpp"
#include "th07/Bgm.hpp"
#include "th07/ExeTables.hpp"
#include "th07/SoundPlayer.hpp"

namespace touhou::th07 {

void ScreenShake::update(ZunRng& rng, std::uint32_t stageFrame, float& offsetX, float& offsetY) {
    if (stageFrame <= 1) {
        jobs_.clear();
        return;
    }
    for (std::size_t i = 0; i < jobs_.size();) {
        Job& j = jobs_[i];
        if (++j.timer >= j.duration) {
            jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const float o = j.from + (j.to - j.from) * static_cast<float>(j.timer) / static_cast<float>(j.duration);
        const auto pick = [&](std::uint32_t r) { return r == 0 ? 0.0f : r == 1 ? o : -o; };
        offsetX = pick(rng.u32InRange(3));
        offsetY = pick(rng.u32InRange(3));
        ++i;
    }
}

Gameplay::Gameplay() : player_(std::make_unique<Player>()) {}
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
    world_ = World{};
    world_.state = &state_;
    world_.rng = res.rng;
    world_.anm = res.anm;
    world_.tables = res.tables;
    world_.sound = res.sound;
    world_.player = player_.get();
    world_.effects = res.tables ? &effects_ : nullptr;
    world_.background = &background_;
    world_.shakes = &shake_;
    return setUpStage(true, error);
}

bool Gameplay::nextStage(std::string* error) {
    if (!running_) return false;
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
    for (int slot : {17, 18, 19, 20}) res_.anm->release(slot);
}

bool Gameplay::setUpStage(bool fresh, std::string* error) {
    GameState& g = state_;
    ZunRng& rng = *res_.rng;
    const World& w = world_;
    if (fresh) {
        rng.u32InRange(65535);  // a dummy allocation size
        // The anti-tamper canaries: the cherry base first, then 29 more values.
        g.cherryBase = static_cast<std::int32_t>(rng.u32InRange(100000)) + 6543;
        for (int i = 0; i < 15; ++i) rng.u32InRange(100000);
        for (int i = 0; i < 9; ++i) rng.f32ZeroToOne();
        for (int i = 0; i < 5; ++i) rng.u32InRange(100000);
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
        if (g.replay) g.slowMode = false;
    } else {
        g.guiScore = g.score;
        g.rollSpeed = 0;
        if (!player_->load(*res_.vfs, *res_.textures, *res_.anm, g.shotIndex, false, error)) return false;
    }
    g.subRank = 0;
    g.stagePointItems = 0;
    g.stageGraze = 0;
    ++g.stage;
    if (g.practice && g.stage >= 2) w.setPower(128.0f);
    stageSeed_ = rng.currentSeed();

    if (!etamaLoaded_) {
        if (!res_.anm->load(*res_.vfs, *res_.textures, "data/etama.anm", 11, 0x200, error)) return false;
        etamaLoaded_ = true;
    }
    if (!background_.load(*res_.vfs, *res_.textures, *res_.anm, rng, g.stage, error)) return false;
    background_.onWeatherShift = nullptr;
    effects_.reset();
    if (!loadEffectFiles(error)) return false;
    shake_.clear();
    player_->stageStart(w);

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

void Gameplay::end() {
    if (!running_) return;
    releaseStageFiles();
    player_->releaseAnm(*res_.anm);
    if (etamaLoaded_) res_.anm->release(11);
    etamaLoaded_ = false;
    if (res_.music) res_.music->stop();
    running_ = false;
}

void Gameplay::tick(std::uint16_t input) {
    if (!running_) return;
    GameState& g = state_;
    g.eventFlags = 0;
    // Game manager (priority 2).
    g.rollScore();
    ++g.stageFrame;
    // Stage background (7), player (8), enemies (10), effects (11), bullets and items (12),
    // GUI (13), screen effects (15).
    background_.update(*res_.anm, *res_.rng);
    player_->update(world_, input);
    effects_.update(world_);
    drawState_.offsetX = drawState_.offsetY = 0.0f;
    shake_.update(*res_.rng, g.stageFrame, drawState_.offsetX, drawState_.offsetY);
}

void Gameplay::draw(SpriteSink& sink) {
    if (!running_) return;
    background_.drawHigh(sink, drawState_);
    background_.drawLow(sink, drawState_);
    player_->drawLow(sink, drawState_);
    player_->drawHigh(sink, drawState_);
    effects_.draw(sink, drawState_);
}

}  // namespace touhou::th07
