#pragma once

// Embodiment of Scarlet Devil as the shared host runs it (host/Game): the stage with its
// title screens, menus, results and ending, its sound effects and music, the launcher's
// game options and the files kept in the app's storage (score.dat, th06.cfg, replays,
// replaced music).

#include <cstdint>
#include <memory>
#include <string>

#include "audio/MusicPlayer.hpp"
#include "host/Game.hpp"
#include "th06/SoundPlayer.hpp"
#include "th06/Stage.hpp"

namespace touhou::th06 {

class Th06Game final : public Game {
  public:
    ~Th06Game() override;

    const GameInfo& info() const override;
    bool start(const GameContext& context) override;
    void tick(const InputState& input) override { stage_->tick(input); }
    void render(SpriteSink& sink, float alpha, const FrameInfo& frame) override;

    bool quitRequested() const override { return stage_->quitRequested(); }
    bool paused() const override { return stage_->paused(); }
    bool showsPlayfield() const override;
    bool practiceSpeedApplies() const override { return showsPlayfield() && !stage_->menus().frozen(); }
    void pauseNow() override { stage_->pauseNow(); }
    bool back() override;
    int takeVibration() override;

    bool takeCaptureRequest() override { return stage_->takeCaptureRequest(); }
    int captureTexture() const override { return stage_->captureTexture(); }
    void setCaptureReady(bool ready) override { stage_->setCaptureReady(ready); }

    void logStatus(const FrameInfo& frame) const override;
    void logSummary() const override;
    void devCommand(DevCommand command) override;

  private:
    // The launcher's choices (desktop: --shot, --difficulty, --stage, --quick, --mischief,
    // --translation).
    int shotType_ = 0;
    int startStage_ = 1;
    int startDifficulty_ = 1;
    std::uint32_t mischief_ = 0;
    bool quickStart_ = false;
    std::string translation_;  // desktop: patch folders, comma-separated

    std::uint32_t lastDeaths_ = 0, lastBombs_ = 0;  // for the vibration on a miss / bomb

    std::unique_ptr<SoundPlayer> sound_;
    std::unique_ptr<MusicPlayer> music_;
    std::unique_ptr<Stage> stage_;  // destroyed first: it holds the two above
};

}  // namespace touhou::th06
