#pragma once

// Imperishable Night as the host runs it: the Supervisor's scenes as they are ported from the
// clean-room specs (the main menu, the Music Room, the game, the endings and the result screen). A status screen says what is missing when the
// player's files are not there (th08.dat 1.00d, th08.exe 1.00d, thbgm.dat).

#include <cstdint>
#include <memory>

#include "core/ZunMath.hpp"
#include "gfx/TextRenderer.hpp"
#include "host/Game.hpp"
#include "th08/Anm.hpp"
#include "th08/Bgm.hpp"
#include "th08/Ending.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Flow.hpp"
#include "th08/GameConfig.hpp"
#include "th08/GameFiles.hpp"
#include "th08/Gameplay.hpp"
#include "th08/MainMenu.hpp"
#include "th08/MusicRoom.hpp"
#include "th08/ResultScreen.hpp"
#include "th08/ScoreFile.hpp"
#include "th08/SoundPlayer.hpp"

namespace touhou::th08 {

class Th08Game final : public Game {
  public:
    ~Th08Game() override;
    const GameInfo& info() const override;
    bool start(const GameContext& context) override;
    void tick(const InputState& input) override;
    void render(SpriteSink& sink, float alpha, const FrameInfo& frame) override;

    bool quitRequested() const override { return quit_; }
    bool paused() const override { return false; }
    bool showsPlayfield() const override { return gameplay_ != nullptr; }
    bool practiceSpeedApplies() const override { return false; }
    void pauseNow() override {}
    bool back() override;
    int takeVibration() override { return 0; }

    bool takeCaptureRequest() override;
    int captureTexture() const override;
    void setCaptureReady(bool ready) override;
    CaptureRect captureRect() const override;
    bool keepsPreviousFrame() const override;

    void logStatus(const FrameInfo& frame) const override;
    void logSummary() const override {}
    void devCommand(DevCommand) override {}

    Scene scene() const { return active_; }

  private:
    void saveConfig() const;
    // `gameRng`: the two random header bytes come from the game's RNG (the original's writes);
    // the port's own extra writes (a song heard) draw nothing from it.
    void saveScores(bool gameRng = false);
    bool anyScene() const { return menu_ || musicRoom_ || gameplay_ || ending_ || result_; }
    void refreshUnlocks();
    void applyScene();

    const UiArt* ui_ = nullptr;
    FolderReport folder_;
    std::string storage_;
    GameConfig config_;
    ScoreData scores_;
    // The spell card history as last read or written: in slow mode a run's card history is
    // dropped before every write (spec_result_ending 1.3 steps 7-8, 1.5, 3.1).
    std::array<CardRecord, kCardCount> savedCards_{};
    ExeTables tables_;
    bool tablesLoaded_ = false;
    std::unique_ptr<SoundPlayer> sound_;
    std::unique_ptr<BgmPlayer> music_;
    std::size_t songs_ = 0;
    AnmLibrary anm_;
    ZunRng rng_;
    std::unique_ptr<FallbackTextRenderer> fallbackText_;
    FlowContext flow_;
    MenuInput input_;
    std::unique_ptr<MainMenu> menu_;
    std::unique_ptr<MusicRoom> musicRoom_;
    std::unique_ptr<Gameplay> gameplay_;
    std::unique_ptr<Ending> ending_;
    std::unique_ptr<ResultScreen> result_;
    GameState lastGame_;  // the run as it ended (for the ending and the result screen)
    std::vector<MusicTrack> musicTracks_;  // musiccmt.txt: a played song's Music Room index
    Scene active_ = Scene::MainMenu, requested_ = Scene::MainMenu;
    bool quit_ = false;
    std::uint16_t injected_ = 0;  // Android Back as Menu for one pass
    std::uint64_t ticks_ = 0;
};

}  // namespace touhou::th08
