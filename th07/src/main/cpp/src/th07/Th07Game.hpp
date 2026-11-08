#pragma once

// Perfect Cherry Blossom as the shared host runs it (host/Game). For now a status screen:
// whether th07.dat (1.00b) and the music were found, with the title song playing from
// thbgm.dat. The game itself is being ported system by system (roadmap, Phase 3).

#include <cstdint>
#include <memory>
#include <string>

#include "host/Game.hpp"
#include "th07/Anm.hpp"
#include "th07/Background.hpp"
#include "th07/Bgm.hpp"
#include "th07/ExeTables.hpp"
#include "th07/Gameplay.hpp"
#include "th07/SoundPlayer.hpp"

namespace touhou::th07 {

class Th07Game final : public Game {
  public:
    const GameInfo& info() const override;
    bool start(const GameContext& context) override;
    void tick(const InputState& input) override;
    void render(SpriteSink& sink, float alpha, const FrameInfo& frame) override;

    bool quitRequested() const override { return quit_; }
    bool paused() const override { return game_ && game_->paused(); }
    bool showsPlayfield() const override { return game_ != nullptr; }
    bool practiceSpeedApplies() const override { return false; }
    void pauseNow() override {
        if (game_ && !game_->paused()) game_->requestPause();
    }
    bool back() override {  // in a game: the pause menu; otherwise leave
        if (!game_) return false;
        if (!game_->paused()) game_->requestPause();
        return true;
    }
    int takeVibration() override { return 0; }

    bool takeCaptureRequest() override { return false; }
    int captureTexture() const override { return -1; }
    void setCaptureReady(bool) override {}

    void logStatus(const FrameInfo& frame) const override;
    void logSummary() const override {}
    void devCommand(DevCommand) override {}

  private:
    const UiArt* ui_ = nullptr;
    std::size_t files_ = 0;
    bool archive_ = false;  // th07.dat with th07_0100b.ver
    std::size_t songs_ = 0;
    bool thbgm_ = false, thbgmBad_ = false;
    std::unique_ptr<BgmPlayer> music_;
    bool exe_ = false;  // th07.exe 1.00b found (its tables loaded)
    ExeTables tables_;
    std::unique_ptr<SoundPlayer> sound_;
    int soundsLoaded_ = 0;
    bool lastShoot_ = false;

    // Desktop developer option --anm-preview FILE [--anm-bg PICTURE]: every script of an ANM
    // file running side by side over a background, to check the ANM system on real data.
    void startPreview(const GameContext& ctx, const std::string& file, const std::string& background);
    AnmLibrary anm_;
    ZunRng rng_;
    // Desktop developer option --play STAGE [--shot N] [--difficulty D] [--practice]: straight
    // into a stage (until the menus are ported).
    std::unique_ptr<Gameplay> game_;
    bool autoFire_ = false;  // the launcher's "Shot is held during play"
    bool holdFocus_ = false;  // desktop --hold-focus
    std::vector<AnmVm> previewVms_;
    int previewBackground_ = -1;  // texture id
    // --stage-preview N: stage N's background running on its own.
    Background background_;
    bool titleSongRequested_ = false;
    std::uint64_t frame_ = 0;
    bool quit_ = false;
};

}  // namespace touhou::th07
