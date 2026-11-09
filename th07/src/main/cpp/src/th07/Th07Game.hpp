#pragma once

// Perfect Cherry Blossom as the shared host runs it (host/Game): the boot (th07.dat 1.00b,
// th07.exe's tables, music and sound effects), then the supervisor (title, select screens,
// games, results, ending, Music Room). A status screen says what is missing when the files
// cannot run the game.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gfx/TextRenderer.hpp"
#include "host/Game.hpp"
#include "th07/Anm.hpp"
#include "th07/Background.hpp"
#include "th07/Bgm.hpp"
#include "th07/English.hpp"
#include "th07/ExeTables.hpp"
#include "th07/Gameplay.hpp"
#include "th07/SoundPlayer.hpp"
#include "th07/Supervisor.hpp"

namespace touhou::th07 {

class Th07Game final : public Game {
  public:
    ~Th07Game() override;
    const GameInfo& info() const override;
    bool start(const GameContext& context) override;
    void tick(const InputState& input) override;
    void render(SpriteSink& sink, float alpha, const FrameInfo& frame) override;

    bool quitRequested() const override { return quit_; }
    bool paused() const override { return flow_ && flow_->paused(); }
    bool showsPlayfield() const override { return flow_ && flow_->inGame(); }
    bool practiceSpeedApplies() const override { return false; }
    void pauseNow() override {
        if (flow_) flow_->pauseNow();
    }
    // In a game: the pause menu (and Menu inside it); in the menus: Return. False leaves.
    bool back() override { return flow_ ? flow_->back() : false; }
    int takeVibration() override { return 0; }

    // The pause and continue menus' playfield snapshot, into capture.anm's texture.
    bool takeCaptureRequest() override;
    int captureTexture() const override;
    void setCaptureReady(bool ready) override;
    CaptureRect captureRect() const override;

    void logStatus(const FrameInfo& frame) const override;
    void logSummary() const override;
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
    std::vector<std::uint8_t> verFile_;
    ExeIdentity exeId_;
    std::unique_ptr<FallbackTextRenderer> fallbackText_;
    bool autoFire_ = false;  // the launcher's "Shot is held during play"
    bool holdFocus_ = false;  // desktop --hold-focus
    std::vector<AnmVm> previewVms_;
    int previewBackground_ = -1;  // texture id
    // --stage-preview N: stage N's background running on its own.
    Background background_;
    bool titleSongRequested_ = false;
    std::uint64_t frame_ = 0;
    bool quit_ = false;
    // Desktop --replay-check: each played-back frame's event flags against the recorded ones
    // (the first mismatch of each stage is logged); --replay-log N: the state every N frames.
    struct ReplayCheck {
        bool on = false;
        int logEvery = 0;
        int stage = 0;
        std::int32_t frames = 0, mismatches = 0;
        std::int32_t firstMismatch = -1;  // in the current stage
        std::uint32_t lastScore = 0;
        int lastLives = 0, deaths = 0;
        std::vector<std::string> summary;
        std::shared_ptr<const ReplayFile> file;
    };
    mutable ReplayCheck check_;
    void installReplayCheck();
    void closeReplayCheckStage() const;
    // The English text mode (launcher option; desktop --english or --thcrap DIR): the
    // player's thcrap English patch, applied through the Vfs's read filter.
    void setUpEnglish(const GameContext& ctx);
    std::unique_ptr<English> english_;
    Vfs* vfs_ = nullptr;
    // The supervisor (declared last: it goes first, while the ANM library, music and sound
    // it uses still exist). Desktop --play STAGE [--shot N] [--difficulty D] [--practice]
    // and the launcher's "skip the title screen" start a game straight away.
    std::unique_ptr<Supervisor> flow_;
};

}  // namespace touhou::th07
