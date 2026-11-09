#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "audio/AudioMixer.hpp"
#include "core/FixedTimestep.hpp"
#include "gfx/GpuTextures.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/RenderTarget.hpp"
#include "gfx/SpriteBatch.hpp"
#include "gfx/TextRenderer.hpp"
#include "gfx/TextureLibrary.hpp"
#include "host/DisplayOptions.hpp"
#include "host/Game.hpp"
#include "input/InputMapper.hpp"
#include "input/VirtualController.hpp"
#include "io/Vfs.hpp"

namespace touhou {

// The shared host. Owns the window, GL context, audio and input devices and runs one game
// (host/Game) in the dual loop: the simulation at a fixed 60.0 Hz, and rendering once per
// display refresh with the positions interpolated between ticks. The game's picture is
// drawn at its own size into an offscreen target, then letterboxed onto the screen (or, in
// the portrait layout during play, its playfield across the width and the score panel
// below), with the touch controls on top at full resolution.
class App {
  public:
    explicit App(std::unique_ptr<Game> game) : game_(std::move(game)) {}
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
    // Copies the playfield of the last rendered frame into the game's capture texture
    // (stretched to fill it, top row first), for the menus to draw over.
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
    void haptics();
    FrameInfo frameInfo() const { return FrameInfo{fps_, lastDrawCalls_, tickCount_}; }

    // Desktop developer options (ignored on Android; a game reads its own, e.g. th06's
    // --stage, --difficulty, --shot, --quick):
    //   --size WxH        window size, e.g. --size 2400x1080 to check phone layouts
    //   --frames N        quit after N rendered frames
    //   --ticks N         quit once N game ticks have run
    //   --screenshot F    write the last frame to F (binary PPM)
    //   --seed N          the game RNG's first seed (default: the clock)
    //   --lockstep        one game tick per rendered frame (reproducible runs)
    //   --keys T:K,...    press button K (up, down, left, right, shoot, bomb, pause, quit,
    //                     save, load) once game tick T is reached, briefly (in tick order)
    //   --demo-touch      inject finger events (hold D-pad right + Shoot) through the
    //                     touch overlay -> virtual controller -> game input path
    //   --demo-bomb N, --demo-pause N, --demo-gameover N, --demo-clear N
    //   --portrait --sharp --pixel --scanlines   display settings
    //   --dpad --autofire --left-handed --slow P --hitbox --savestates   control settings
    int devFrames_ = 0;
    int devTicks_ = 0;
    int devSeed_ = -1;
    bool devLockstep_ = false;
    std::string devScreenshot_;
    bool devTouch_ = false;
    int devBombFrame_ = -1;
    int devPauseFrame_ = -1;
    int devGameOverFrame_ = -1;
    int devClearFrame_ = -1;
    struct DevKey {
        std::uint64_t tick;
        std::string key;
    };
    std::vector<DevKey> devKeys_;  // in order
    std::size_t devKeyNext_ = 0;
    bool devKeyHeld_ = false;

    std::unique_ptr<TextRenderer> text_;  // platform text (null: built-in fallback); outlives the game
    std::unique_ptr<Game> game_;          // reset first in shutdown(): it uses the members below
    ControlOptions controls_;  // the launcher's touch settings (desktop: --dpad, --autofire, ...)
    DisplayOptions display_;   // the launcher's display settings (desktop: --portrait, --sharp, --pixel, --scanlines)
    // Portrait play view: the playfield and the panel's score rows on screen.
    float playX_ = 0, playY_ = 0, playW_ = 0, playH_ = 0;
    float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;
    bool lastPlayView_ = false;
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
    InputMapper input_;
    VirtualController touch_;
    UiArt ui_;

    int drawWidth_ = 0;
    int drawHeight_ = 0;
    float gameX_ = 0, gameY_ = 0, gameW_ = 0, gameH_ = 0;

    FixedTimestep clock_;
    bool running_ = true;
    bool vsync_ = false;
    bool resetClock_ = true;
    bool background_ = false;  // the app is in the background: no ticks, no drawing
    int refreshHz_ = 60;
    float fps_ = 0;
    std::uint32_t lastDrawCalls_ = 0;
    int glChecks_ = 180;  // frames with GL error checks after each step (start-up diagnostics)
};

}  // namespace touhou
