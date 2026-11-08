#pragma once

// Perfect Cherry Blossom as the shared host runs it (host/Game). For now a status screen:
// whether th07.dat (1.00b) and the music were found, with the title song playing from
// thbgm.dat. The game itself is being ported system by system (roadmap, Phase 3).

#include <cstdint>
#include <memory>
#include <string>

#include "host/Game.hpp"
#include "th07/Bgm.hpp"

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
    bool archive_ = false;  // th07.dat with th07_0100b.ver
    std::size_t songs_ = 0;
    bool thbgm_ = false, thbgmBad_ = false;
    std::unique_ptr<BgmPlayer> music_;
    bool titleSongRequested_ = false;
    std::uint64_t frame_ = 0;
    bool quit_ = false;
};

}  // namespace touhou::th07
