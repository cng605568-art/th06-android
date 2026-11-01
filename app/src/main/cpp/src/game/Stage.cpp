#include "game/Stage.hpp"

#include <algorithm>
#include <format>
#include <span>

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
constexpr const char* kDifficultyNames[5] = {"EASY", "NORMAL", "HARD", "LUNATIC", "EXTRA"};
constexpr int kClearDelay = 180;

}  // namespace

Stage::Stage(TextureLibrary& textures, const UiArt& ui, const Vfs* vfs, SoundPlayer* sound)
    : sound_(sound),
      vfs_(vfs),
      ui_(ui),
      rng_(0),
      anm_(textures, rng_),
      bullets_(std::make_unique<BulletManager>()),
      enemies_(std::make_unique<EnemyManager>()),
      items_(std::make_unique<ItemManager>()),
      player_(std::make_unique<Player>()) {
    world_.anm = &anm_;
    world_.bullets = bullets_.get();
    world_.enemies = enemies_.get();
    world_.items = items_.get();
    world_.player = player_.get();
    world_.state = &state_;
    world_.rng = &rng_;
    world_.sound = sound;
    world_.unported = &unported_;
}

bool Stage::load(int stageNumber, int difficulty) {
    ready_ = false;
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
    state_.grazes = 0;
    state_.stagePointItems = 0;
    player_->reset(world_);
    if (!shots_.loaded) unported_.hit("shot types (needs 東方紅魔郷.exe 1.02h in the game folder)");
    enemies_->startStage(world_);
    clearTimer_ = 0;
    ready_ = true;
    error_.clear();
    log::info("Stage {} ({}) loaded", stageNumber_, kDifficultyNames[state_.difficulty]);
    return true;
}

void Stage::tick(const InputState& in) {
    if (in.pause && !previousPause_) paused_ = !paused_;
    previousPause_ = in.pause;
    if (!ready_ || paused_) return;

    ++state_.frame;
    rollScore(world_);
    player_->update(world_, in);
    enemies_->update(world_);
    bullets_->update(world_);
    if (sound_) sound_->flush();

    // The original ends a stage through its post-boss dialogue; without MSG ported, the
    // stage counts as cleared once the timeline has run out and every enemy is gone.
    if (enemies_->timelineFinished() && enemies_->activeCount() == 0) {
        if (++clearTimer_ >= kClearDelay && stageNumber_ < 6) {
            state_.score += stageClearBonus(state_, stageNumber_);
            load(stageNumber_ + 1, state_.difficulty);  // keeps score, power, lives and bombs
        }
    } else {
        clearTimer_ = 0;
    }
}

void Stage::render(SpriteSink& s, float alpha, const HudInfo& hud) {
    anm_.beginFrame();
    s.fillRect(ui_.white, kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight, rgba(8, 6, 20));
    if (ready_) {
        player_->drawLow(world_, s, alpha);
        enemies_->draw(world_, s, alpha);
        player_->drawHits(world_, s, alpha);
        bullets_->draw(world_, s, alpha);
    }
    // Frame (also clips sprites that overhang the playfield).
    const auto frame = rgba(46, 18, 40);
    s.fillRect(ui_.white, 0, 0, 640, kGameRegionY, frame);
    s.fillRect(ui_.white, 0, kGameRegionY + kGameRegionHeight, 640, 480 - kGameRegionY - kGameRegionHeight, frame);
    s.fillRect(ui_.white, 0, kGameRegionY, kGameRegionX, kGameRegionHeight, frame);
    s.fillRect(ui_.white, kGameRegionX + kGameRegionWidth, kGameRegionY, 640 - kGameRegionX - kGameRegionWidth,
               kGameRegionHeight, frame);
    renderHud(s, hud);
    if (!ready_) {
        renderMessage(s);
    } else if (paused_) {
        const std::string_view t = "PAUSED";
        drawText(s, ui_, kGameRegionX + (kGameRegionWidth - textWidth(t, 3.0f)) * 0.5f, 220, t, 3.0f, rgba(255, 255, 255));
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
