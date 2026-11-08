#include "th06/Th06Game.hpp"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/Log.hpp"
#include "io/Vfs.hpp"
#if defined(__ANDROID__)
#include "platform/AndroidStorage.hpp"
#endif

namespace touhou::th06 {

Th06Game::~Th06Game() {
    stage_.reset();
    if (music_) music_->stop();
    music_.reset();
    sound_.reset();
}

const GameInfo& Th06Game::info() const {
    static const GameInfo kInfo{
        .id = "th06",
        .windowTitle = "TH06 Port",
        .prefOrg = "th06",
        .prefApp = "th06-android",
        .envPrefix = "TH06",
        .width = 640,
        .height = 480,
        .playX = kGameRegionX,
        .playY = kGameRegionY,
        .playW = kGameRegionWidth,
        .playH = kGameRegionHeight,
        .panelX = kGameRegionX + kGameRegionWidth,  // the panel's score rows, 224x224 from its top
        .panelY = kGameRegionY,
        .panelW = 224.0f,
        .panelH = 224.0f,
    };
    return kInfo;
}

bool Th06Game::start(const GameContext& ctx) {
    if (ctx.samples) {
        sound_ = std::make_unique<SoundPlayer>(ctx.samples);
        const int found = sound_->loadSamples(*ctx.vfs);
        log::info("Loaded {} of {} sound effects from the archives", found, SoundPlayer::kSamples);
    }
    stage_ = std::make_unique<Stage>(*ctx.textures, *ctx.ui, ctx.vfs, sound_.get(), ctx.text);
    if (ctx.music) {
        music_ = std::make_unique<MusicPlayer>(ctx.vfs, ctx.music);
        music_->setMissingMessage("background music (needs the bgm folder with th06_NN.wav)");
        stage_->setMusic(music_.get());
        if (ctx.vfs->looseFileCount() == 0) log::info("No bgm/*.wav in the game folder: only replaced songs play");
    }

    std::vector<std::string> folders;  // the game's text in another language
#if defined(__ANDROID__)
    {
        // GameActivity.getGameOptions(): shot type, difficulty, stage (7 = Extra), mischief
        // bits, quick start.
        const std::vector<int> v = androidIntArray("getGameOptions");
        if (v.size() >= 3) {
            shotType_ = v[0];
            startDifficulty_ = v[1];
            startStage_ = v[2];
            if (v.size() >= 4) mischief_ = static_cast<std::uint32_t>(v[3]);
            if (v.size() >= 5) quickStart_ = v[4] != 0;
        }
        if (startStage_ == 7) startDifficulty_ = Extra;
    }
    folders = readAndroidGameTextStack();
#else
    for (int i = 1; i < ctx.argc; ++i) {
        const std::string a = ctx.argv[i] ? ctx.argv[i] : "";
        const char* next = i + 1 < ctx.argc ? ctx.argv[i + 1] : nullptr;
        if (a == "--stage" && next) startStage_ = std::atoi(ctx.argv[++i]), quickStart_ = true;
        else if (a == "--difficulty" && next) startDifficulty_ = std::atoi(ctx.argv[++i]), quickStart_ = true;
        else if (a == "--shot" && next) shotType_ = std::atoi(ctx.argv[++i]), quickStart_ = true;
        else if (a == "--quick") quickStart_ = true;
        else if (a == "--translation" && next) translation_ = ctx.argv[++i];  // patch folders, comma-separated
        else if (a == "--mischief" && next) mischief_ = static_cast<std::uint32_t>(std::atoi(ctx.argv[++i]));
    }
    std::string list = translation_;
    if (list.empty()) {
        if (const char* env = std::getenv("TH06_TRANSLATION")) list = env;
    }
    for (std::size_t at = 0; at < list.size();) {
        const std::size_t comma = list.find(',', at);
        const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!item.empty()) folders.push_back(item);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
#endif

    stage_->seedRandom(ctx.seed);
    stage_->setTranslation(std::move(folders));
    stage_->setShotType(shotType_);
    stage_->setMischief(mischief_);
    stage_->setAutoFire(ctx.controls.autoFire);
    stage_->setPracticeTools(ctx.controls.speedPercent, ctx.controls.showHitbox, ctx.controls.saveStates);
    if (!ctx.storageDir.empty()) {
        stage_->setScorePath(ctx.storageDir + "score.dat");
        stage_->setConfigPath(ctx.storageDir + "th06.cfg");
        // The launcher's replaced music: <files>/music/th06_NN.wav (+ .pos loop points).
        if (music_) music_->setOverrideDir(ctx.storageDir + "music/");
        stage_->setReplayDir(ctx.storageDir + "replay/");  // th6_01..15.rpy, as the PC game's replay folder
    }
    // The game's title screen, unless the launcher asked for a quick start (or the title
    // files are missing).
    if (quickStart_ || !stage_->openTitle(true)) {
        if (!stage_->quickStart(startStage_, startDifficulty_, quickStart_)) log::warn("Stage not started: {}", stage_->error());
    }
    return true;
}

void Th06Game::render(SpriteSink& sink, float alpha, const FrameInfo& frame) {
    stage_->render(sink, alpha, HudInfo{frame.fps, frame.drawCalls});
}

bool Th06Game::showsPlayfield() const {
    return stage_->ready() && !stage_->inMenus() && !stage_->inResults() && !stage_->inEnding();
}

// Back is the Menu button: it opens the pause menu and resumes from it (Quit is in the
// menu); on the title screens it is Cancel. Without a running stage it leaves.
bool Th06Game::back() {
    if (!stage_->ready() && !stage_->inMenus()) return false;
    stage_->pressMenu();
    return true;
}

// A short vibration when the player is hit (after the deathbomb window) or bombs.
int Th06Game::takeVibration() {
    if (!stage_->ready() || stage_->inMenus()) return 0;
    const GameState& gs = stage_->state();
    const bool missed = gs.deaths > lastDeaths_, bombed = gs.bombsUsed > lastBombs_;
    lastDeaths_ = gs.deaths;
    lastBombs_ = gs.bombsUsed;
    if (!(missed || bombed)) return 0;
    return missed ? 80 : 35;
}

void Th06Game::logStatus(const FrameInfo& frame) const {
    if (stage_->inMenus()) {
        log::info("fps {:.1f} | ticks {} | title screens", frame.fps, frame.ticks);
        return;
    }
    if (!stage_->ready()) {
        log::info("fps {:.1f} | ticks {} | stage not running: {}", frame.fps, frame.ticks, stage_->error());
        return;
    }
    Stage& st = *stage_;
    log::info("fps {:.1f} | ticks {} | stage {} frame {} | enemies {} | bullets {} (peak {}, slots full {}) | "
              "ECL instructions {} | draw calls {}",
              frame.fps, frame.ticks, st.stageNumber(), st.enemies().timelineTime(), st.enemies().activeCount(),
              st.bullets().activeCount(), st.bullets().peakCount(), st.bullets().spawnFailures(),
              st.enemies().instructionsExecuted(), frame.drawCalls);
}

void Th06Game::logSummary() const {
    if (!stage_->ready()) return;
    const GameState& gs = stage_->state();
    log::info("score {} | grazes {} | misses {} | rank {}", gs.score, gs.grazes, gs.deaths, gs.rank);
    for (const auto& [name, count] : stage_->unported().counts()) log::info("  not ported: {} x{}", name, count);
}

void Th06Game::devCommand(DevCommand command) {
    switch (command) {
        case DevCommand::Menu: stage_->pressMenu(); break;
        case DevCommand::GameOver: stage_->endGameForTesting(); break;
        case DevCommand::Clear: stage_->clearGameForTesting(); break;
    }
}

}  // namespace touhou::th06
