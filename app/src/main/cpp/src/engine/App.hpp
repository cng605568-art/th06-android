#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <SDL.h>

#include "audio/AudioMixer.hpp"
#include "audio/SoundPlayer.hpp"
#include "core/FixedTimestep.hpp"
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
    void renderFrame(float alpha);
    void developerFrameHooks();
    void logStats() const;

    // Desktop developer options (ignored on Android):
    //   --size WxH        window size, e.g. --size 2400x1080 to check phone layouts
    //   --frames N        quit after N rendered frames
    //   --screenshot F    write the last frame to F (binary PPM)
    //   --stage N         start at stage N (1..7, 7 = Extra)
    //   --difficulty D    0 Easy, 1 Normal, 2 Hard, 3 Lunatic, 4 Extra
    //   --demo-touch      inject finger events (hold D-pad right + Shoot) through the
    //                     touch overlay -> virtual controller -> game input path
    int devFrames_ = 0;
    std::string devScreenshot_;
    bool devTouch_ = false;
    int startStage_ = 1;
    int startDifficulty_ = 1;
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
    AudioMixer audio_;
    std::unique_ptr<SoundPlayer> sound_;
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
