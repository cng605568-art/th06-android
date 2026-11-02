#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "audio/AudioMixer.hpp"
#include "audio/SoundPlayer.hpp"
#include "core/FixedTimestep.hpp"
#include "engine/DisplayOptions.hpp"
#include "game/Stage.hpp"
#include "gfx/GpuTextures.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/RenderTarget.hpp"
#include "gfx/SpriteBatch.hpp"
#include "gfx/TextureLibrary.hpp"
#include "input/InputMapper.hpp"
#include "input/VirtualController.hpp"
#include "io/Vfs.hpp"

namespace th06 {

// Owns the window, GL context, audio and input devices and the running stage, and runs
// the dual loop: the simulation (player, ECL, bullets, collision) at a fixed 60.0 Hz, and
// rendering once per display refresh with the positions interpolated between ticks.
class App {
  public:
    ~App();
    int run(int argc, char** argv);

  private:
    bool init(int argc, char** argv);
    void shutdown();
    void loop();
    void handleEvent(const SDL_Event& event);
    void relayout();
    bool createGpu(std::string* error);
    void releaseGpu(bool contextLost);
    // Copies the playfield of the last rendered frame into capture.anm's texture (stretched
    // to fill it, top row first), for the menus to draw over.
    bool capturePlayfield();
    void renderFrame(float alpha);
    // Portrait in play: the playfield fills the width and the score panel sits below it.
    bool playView() const;
    void layoutTouch();
    void blitRegion(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);
    void drawScanlines(float x, float y, float w, float h, int rows);
    void developerFrameHooks();
    void parseDevKeys(const std::string& spec);
    void applyDevKeys(InputState& in);
    void logStats() const;

    // Desktop developer options (ignored on Android):
    //   --size WxH        window size, e.g. --size 2400x1080 to check phone layouts
    //   --frames N        quit after N rendered frames
    //   --screenshot F    write the last frame to F (binary PPM)
    //   --stage N         start at stage N (1..7, 7 = Extra), skipping the title
    //   --difficulty D    0 Easy, 1 Normal, 2 Hard, 3 Lunatic, 4 Extra (skips the title)
    //   --shot S          0 Reimu A, 1 Reimu B, 2 Marisa A, 3 Marisa B (skips the title)
    //   --quick           skip the title with the defaults
    //   --keys T:K,...    press button K (up, down, left, right, shoot, bomb, pause, quit)
    //                     once game tick T is reached, briefly (keys in tick order)
    // On Android the quick start and these three come from the launcher.
    //   --demo-touch      inject finger events (hold D-pad right + Shoot) through the
    //                     touch overlay -> virtual controller -> game input path
    int devFrames_ = 0;
    int devTicks_ = 0;  // --ticks N: screenshot/exit once N game ticks have run
    std::string devScreenshot_;
    bool devTouch_ = false;
    int devBombFrame_ = -1;  // --demo-bomb N: tap the touch BOMB button on frame N
    int devPauseFrame_ = -1;  // --demo-pause N: presses Menu on frame N
    std::uint32_t mischief_ = 0;  // launcher mischief options (desktop: --mischief bits)
    int devGameOverFrame_ = -1;  // --demo-gameover N: ends the game on frame N (result screen)
    int devClearFrame_ = -1;     // --demo-clear N: clears the game on frame N (ending)
    std::unique_ptr<TextRenderer> text_;  // platform text (null: built-in fallback); outlives stage_
    bool quickStart_ = false;
    ControlOptions controls_;  // the launcher's touch settings (desktop: --dpad, --autofire)
    DisplayOptions display_;   // the launcher's display settings (desktop: --portrait, --sharp, --pixel, --scanlines)
    // Portrait play view: the playfield and the panel's score rows on screen.
    float playX_ = 0, playY_ = 0, playW_ = 0, playH_ = 0;
    float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;
    bool lastPlayView_ = false;
    std::uint32_t lastDeaths_ = 0, lastBombs_ = 0;  // for the vibration on a miss / bomb
    void haptics();
    struct DevKey {
        std::uint64_t tick;
        std::string key;
    };
    std::vector<DevKey> devKeys_;
    std::string translation_;  // --translation (desktop)  // in order
    std::size_t devKeyNext_ = 0;
    bool devKeyHeld_ = false;
    int startStage_ = 1;
    int startDifficulty_ = 1;
    int shotType_ = 0;
    int windowWidth_ = 1280;
    int windowHeight_ = 960;
    std::uint64_t frameCount_ = 0;
    std::uint64_t tickCount_ = 0;

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    bool sdlReady_ = false;

    std::unique_ptr<Vfs> vfs_;
    TextureLibrary textures_;
    GpuTextures gpu_;
    SpriteBatch batch_;
    RenderTarget target_;
    GLuint captureFbo_ = 0;
    AudioMixer audio_;
    std::unique_ptr<SoundPlayer> sound_;
    std::unique_ptr<MusicPlayer> music_;
    InputMapper input_;
    VirtualController touch_;
    UiArt ui_;
    std::unique_ptr<Stage> stage_;

    int drawWidth_ = 0;
    int drawHeight_ = 0;
    float gameX_ = 0, gameY_ = 0, gameW_ = 0, gameH_ = 0;

    FixedTimestep clock_;
    bool running_ = true;
    bool vsync_ = false;
    bool resetClock_ = true;
    int refreshHz_ = 60;
    float fps_ = 0;
    std::uint32_t lastDrawCalls_ = 0;
};

}  // namespace th06
