#pragma once

// Imperishable Night as the host runs it. For now a status screen: what the player's game
// folder holds (th08.dat, th08.exe 1.00d, thbgm.dat). The game's own screens replace it as
// the systems are ported from the clean-room specs.

#include <cstdint>

#include "host/Game.hpp"
#include "th08/GameFiles.hpp"

namespace touhou::th08 {

class Th08Game final : public Game {
  public:
    const GameInfo& info() const override;
    bool start(const GameContext& context) override;
    void tick(const InputState& input) override;
    void render(SpriteSink& sink, float alpha, const FrameInfo& frame) override;

    bool quitRequested() const override { return false; }
    bool paused() const override { return false; }
    bool showsPlayfield() const override { return false; }
    bool practiceSpeedApplies() const override { return false; }
    void pauseNow() override {}
    bool back() override { return false; }
    int takeVibration() override { return 0; }

    bool takeCaptureRequest() override { return false; }
    int captureTexture() const override { return -1; }
    void setCaptureReady(bool) override {}

    void logStatus(const FrameInfo& frame) const override;
    void logSummary() const override {}
    void devCommand(DevCommand) override {}

  private:
    const UiArt* ui_ = nullptr;
    FolderReport folder_;
    std::uint64_t ticks_ = 0;
};

}  // namespace touhou::th08
