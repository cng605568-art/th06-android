#pragma once

// The line between the shared host and one game. The host (host/App) owns the window, the
// GL context, the audio device, input and the main loop, and runs the game it was given:
// a fixed 60 Hz tick for the simulation, and rendering once per display refresh with the
// picture interpolated between ticks. Everything a game decides (its menus, rules, files,
// options and saves) stays behind this interface. No SDL or GL here: a game's code builds
// and tests without a window.

#include <cstdint>
#include <string>

#include "input/ControlOptions.hpp"
#include "input/InputState.hpp"

namespace touhou {

class MusicBackend;
class SampleBackend;
class SpriteSink;
class TextRenderer;
class TextureLibrary;
class Vfs;
struct UiArt;

// What the host needs to know before a game starts.
struct GameInfo {
    const char* id;           // "th06": the log's name for the game
    const char* windowTitle;  // the desktop window
    // The app's own storage (SDL_GetPrefPath): on desktop <data>/<prefOrg>/<prefApp>/; on
    // Android the app's files folder whatever these say.
    const char* prefOrg;
    const char* prefApp;
    // Desktop: the environment variables that name the game folder (<envPrefix>_DATA) and
    // prefer the New Classic archives (<envPrefix>_PREFER_NC).
    const char* envPrefix;
    // The game's picture and, inside it, the playfield and the score panel the portrait
    // layout shows on their own during play.
    int width = 640, height = 480;
    float playX = 32, playY = 16, playW = 384, playH = 448;
    float panelX = 416, panelY = 16, panelW = 224, panelH = 224;
};

// What the host has ready when the game starts.
struct GameContext {
    int argc = 0;              // desktop command line (game options); unused on Android
    char** argv = nullptr;
    Vfs* vfs = nullptr;        // the player's own game files
    TextureLibrary* textures = nullptr;
    const UiArt* ui = nullptr;
    SampleBackend* samples = nullptr;  // null without an audio device
    MusicBackend* music = nullptr;     // null without an audio device
    TextRenderer* text = nullptr;      // the platform's text (null: the built-in fallback)
    ControlOptions controls;           // the launcher's touch and practice settings
    std::string storageDir;            // the app's storage, ending in '/' (empty: none)
    std::uint16_t seed = 0;            // the game RNG's first seed (the clock, or --seed)
};

// Numbers the host measures, for a game's debug HUD and logs.
struct FrameInfo {
    float fps = 0;
    std::uint32_t drawCalls = 0;
    std::uint64_t ticks = 0;
};

class Game {
  public:
    virtual ~Game() = default;
    virtual const GameInfo& info() const = 0;

    // Once, after the game files, the texture library and audio are ready (before the GL
    // resources: textures are uploaded when first drawn). False: the game cannot run.
    virtual bool start(const GameContext& context) = 0;
    // One 1/60 s step of the game.
    virtual void tick(const InputState& input) = 0;
    // The whole picture (info().width x height), `alpha` of the way from the last tick to
    // the next.
    virtual void render(SpriteSink& sink, float alpha, const FrameInfo& frame) = 0;

    // The game asked to leave (back to the launcher).
    virtual bool quitRequested() const = 0;
    // Nothing moves (a pause menu): the picture is drawn without interpolation.
    virtual bool paused() const = 0;
    // The playfield is on screen in play: the portrait layout shows it on its own.
    virtual bool showsPlayfield() const = 0;
    // The launcher's practice speed (slow motion) applies now.
    virtual bool practiceSpeedApplies() const = 0;
    // The app went to the background: pause the game now.
    virtual void pauseNow() = 0;
    // Android Back (a pause menu's Menu button). False: the game does not use it and the
    // host leaves.
    virtual bool back() = 0;
    // A vibration the game asks for since the last call, in milliseconds (0: none). The
    // host checks the player's setting.
    virtual int takeVibration() = 0;

    // A copy of the playfield for menus drawn over it: when the game asks, the host draws
    // the last frame's playfield into the game's texture `captureTexture()` and reports
    // whether that worked (and false again when GL resources are lost).
    virtual bool takeCaptureRequest() = 0;
    virtual int captureTexture() const = 0;
    virtual void setCaptureReady(bool ready) = 0;
    // Where in that texture the playfield goes, in texture pixels from the top-left corner;
    // the rest of the texture is left alone. A zero width means the whole texture.
    struct CaptureRect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    virtual CaptureRect captureRect() const { return {}; }

    // A status line for the log every few seconds, and a summary when a desktop test run
    // ends.
    virtual void logStatus(const FrameInfo& frame) const = 0;
    virtual void logSummary() const = 0;

    // Desktop test hooks (--demo-pause, --demo-gameover, --demo-clear).
    enum class DevCommand { Menu, GameOver, Clear };
    virtual void devCommand(DevCommand command) = 0;
};

}  // namespace touhou
