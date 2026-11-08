#pragma once

// Perfect Cherry Blossom as the shared host runs it (host/Game). For now a placeholder
// screen that reports the game files it found; the game itself is being ported (see the
// roadmap, Phase 3).

#include <cstdint>
#include <string>

#include "host/Game.hpp"

namespace touhou::th07 {

class Th07Game final : public Game {
  public:
    const GameInfo& info() const override;
    bool start(const GameContext& context) override;
    void tick(const InputState& input) override;
    void render(SpriteSink& sink, float alpha, const FrameInfo& frame) override;

    bool quitRequested() const override { return quit_; }
    bool paused() const override { return false; }
    bool showsPlayfield() const override { return false; }
    bool practiceSpeedApplies() const override { return false; }
    void pauseNow() override {}
    bool back() override { return false; }  // nothing to go back to yet: leave
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
    std::uint64_t frame_ = 0;
    bool quit_ = false;
};

}  // namespace touhou::th07
