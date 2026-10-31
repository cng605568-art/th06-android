#include "engine/App.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/Log.hpp"
#include "game/GameArt.hpp"
#include "platform/DataMount.hpp"

namespace th06 {
namespace {

constexpr double kTickSeconds = 1.0 / 60.0;
constexpr int kMaxTicksPerFrame = 4;        // beyond this the game slows down (like the original)
constexpr double kVsyncSnapSeconds = 0.0005;
constexpr int kVirtualWidth = 640;
constexpr int kVirtualHeight = 480;

[[maybe_unused]] bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

}  // namespace

App::~App() { shutdown(); }

int App::run(int argc, char** argv) {
    if (!init(argc, argv)) {
        shutdown();
        return 1;
    }
    loop();
    shutdown();
    return 0;
}

bool App::init(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        log::error("SDL_Init: {}", SDL_GetError());
        return false;
    }
    sdlReady_ = true;
    SDL_version v;
    SDL_GetVersion(&v);
    log::info("SDL {}.{}.{}", v.major, v.minor, v.patch);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#if !defined(__ANDROID__)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--frames" && next) devFrames_ = std::atoi(argv[++i]);
        else if (a == "--screenshot" && next) devScreenshot_ = argv[++i];
        else if (a == "--demo-touch") devTouch_ = true;
        else if (a == "--size" && next) std::sscanf(argv[++i], "%dx%d", &windowWidth_, &windowHeight_);
    }
#endif

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__ANDROID__)
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    window_ = SDL_CreateWindow("TH06 Port", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               windowWidth_, windowHeight_, flags);
    if (!window_) {
        log::error("SDL_CreateWindow: {}", SDL_GetError());
        return false;
    }
    context_ = SDL_GL_CreateContext(window_);
    if (!context_) {
        log::error("OpenGL ES 3.0 context: {}", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, context_);
    vsync_ = SDL_GL_SetSwapInterval(1) == 0;
    SDL_DisplayMode mode;
    if (SDL_GetWindowDisplayMode(window_, &mode) == 0 && mode.refresh_rate > 0) refreshHz_ = mode.refresh_rate;
    log::info("GL: {} / {} | vsync {} | display {} Hz",
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)), vsync_ ? "on" : "off", refreshHz_);

    vfs_ = mountGameData(argc, argv);
    GameArt art = buildGameArt(vfs_.get(), textures_);
    ui_ = art.ui;

    std::string err;
    if (audio_.open(&err)) {
        loadSounds();
    } else {
        log::warn("Audio disabled: {}", err);
    }

    game_ = std::make_unique<Game>(std::move(art), audio_.isOpen() ? &audio_ : nullptr);

    if (!createGpu(&err)) {
        log::error("GPU setup failed: {}", err);
        return false;
    }

    if (!touch_.attach(&err)) log::warn("Touch controls unavailable: {}", err);
#if defined(__ANDROID__)
    touch_.setVisible(true);
#else
    touch_.setVisible(devTouch_ || hasFlag(argc, argv, "--touch") || SDL_GetNumTouchDevices() > 0);
#endif
    relayout();
    return true;
}

void App::loadSounds() {
    if (!vfs_ || vfs_->fileCount() == 0) return;
    int loaded = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Sfx::Count); ++i) {
        auto bytes = vfs_->read(kSfxFiles[i]);
        if (!bytes) continue;
        std::string err;
        if (audio_.load(static_cast<Sfx>(i), *bytes, &err)) {
            ++loaded;
        } else {
            log::warn("{}: {}", kSfxFiles[i], err);
        }
    }
    log::info("Loaded {} of {} sound effects from the archives", loaded, static_cast<int>(Sfx::Count));
}

bool App::createGpu(std::string* error) {
    if (!batch_.create(error)) return false;
    if (!target_.create(kVirtualWidth, kVirtualHeight, error)) return false;
    gpu_.sync(textures_);
    return true;
}

void App::releaseGpu(bool contextLost) {
    if (contextLost) {
        gpu_.abandon();
        batch_.abandon();
        target_.abandon();
    } else {
        gpu_.destroy();
        batch_.destroy();
        target_.destroy();
    }
}

void App::shutdown() {
    game_.reset();
    touch_.detach();
    input_.closeAll();
    audio_.close();
    if (context_) {
        releaseGpu(false);
        SDL_GL_DeleteContext(context_);
        context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdlReady_) {
        SDL_Quit();
        sdlReady_ = false;
    }
}

void App::relayout() {
    SDL_GL_GetDrawableSize(window_, &drawWidth_, &drawHeight_);
    if (drawWidth_ <= 0 || drawHeight_ <= 0) return;
    const float scale = std::min(static_cast<float>(drawWidth_) / kVirtualWidth,
                                 static_cast<float>(drawHeight_) / kVirtualHeight);
    gameW_ = kVirtualWidth * scale;
    gameH_ = kVirtualHeight * scale;
    gameX_ = std::floor((static_cast<float>(drawWidth_) - gameW_) * 0.5f);
    gameY_ = std::floor((static_cast<float>(drawHeight_) - gameH_) * 0.5f);
    touch_.layout(drawWidth_, drawHeight_, gameX_, gameY_, gameW_, gameH_);
    log::info("Screen {}x{}, game area {:.0f}x{:.0f} at ({:.0f},{:.0f})", drawWidth_, drawHeight_,
              gameW_, gameH_, gameX_, gameY_);
}

void App::handleEvent(const SDL_Event& e) {
    if (input_.handleEvent(e, touch_.joystickId())) touch_.setVisible(false);

    switch (e.type) {
        case SDL_QUIT:
        case SDL_APP_TERMINATING:
            running_ = false;
            break;
        case SDL_APP_WILLENTERBACKGROUND:
            if (game_) game_->setPaused(true);
            audio_.setPaused(true);
            touch_.releaseAll();
            break;
        case SDL_APP_DIDENTERFOREGROUND:
            audio_.setPaused(false);
            resetClock_ = true;
            break;
        case SDL_APP_LOWMEMORY:
            log::warn("System reports low memory");
            break;
        case SDL_RENDER_DEVICE_RESET: {
            // SDL had to create a fresh EGL context after resume: every GL object is gone.
            log::warn("GL context was recreated; rebuilding GPU resources");
            releaseGpu(true);
            std::string err;
            if (!createGpu(&err)) {
                log::error("GPU rebuild failed: {}", err);
                running_ = false;
            }
            break;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
                relayout();
            }
            break;
        case SDL_KEYDOWN:
            // Android Back: first press pauses, a second press while paused quits.
            if (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK && !e.key.repeat && game_) {
                if (game_->paused()) {
                    running_ = false;
                } else {
                    game_->setPaused(true);
                }
            }
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            touch_.handleEvent(e, drawWidth_, drawHeight_);
            break;
        default:
            break;
    }
}

void App::loop() {
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 fpsStart = last;
    Uint64 logStart = last;
    int fpsFrames = 0;
    double accumulator = 0;

    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        if (!running_) break;

        const Uint64 now = SDL_GetPerformanceCounter();
        double dt = static_cast<double>(now - last) / freq;
        last = now;
        if (resetClock_) {
            dt = kTickSeconds;
            accumulator = 0;
            resetClock_ = false;
        }
        // Vsync jitter: frame times within 0.5 ms of a whole number of display refreshes
        // are treated as exact, so a 60 Hz display yields exactly one tick per frame.
        if (vsync_ && refreshHz_ > 0) {
            const double frame = 1.0 / refreshHz_;
            const double k = std::round(dt / frame);
            if (k >= 1 && std::fabs(dt - k * frame) < kVsyncSnapSeconds) dt = k * frame;
        }
        accumulator += std::min(dt, 0.1);

        int ticks = 0;
        while (accumulator >= kTickSeconds - 0.0002) {
            if (ticks == kMaxTicksPerFrame) {
                accumulator = 0;
                break;
            }
            game_->tick(input_.sample());
            accumulator -= kTickSeconds;
            ++ticks;
        }

        gpu_.sync(textures_);
        renderFrame();
        developerFrameHooks();
        SDL_GL_SwapWindow(window_);

        if (!vsync_) {
            const double spent = static_cast<double>(SDL_GetPerformanceCounter() - now) / freq;
            const double wait = kTickSeconds - accumulator - spent;
            if (wait > 0.002) SDL_Delay(static_cast<Uint32>(wait * 1000.0) - 1);
        }

        ++fpsFrames;
        if (now - fpsStart >= static_cast<Uint64>(freq / 2)) {
            fps_ = static_cast<float>(fpsFrames / (static_cast<double>(now - fpsStart) / freq));
            fpsFrames = 0;
            fpsStart = now;
        }
        if (now - logStart >= static_cast<Uint64>(freq * 10)) {
            const auto& s = game_->stats();
            log::info("fps {:.1f} | ticks {} | bullets {} (peak {}, dropped {}) | draw calls {}", fps_,
                      s.ticks, s.bullets, s.peakBullets, s.dropped, lastDrawCalls_);
            logStart = now;
        }
    }
}

void App::developerFrameHooks() {
    ++frameCount_;
    if (devTouch_ && frameCount_ == 2) {
        // Two fingers: one pushing the D-pad right, one holding Shoot.
        const SDL_GameControllerButton targets[2] = {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_A};
        for (int i = 0; i < 2; ++i) {
            float x = 0, y = 0;
            if (!touch_.locate(targets[i], x, y)) continue;
            SDL_Event e{};
            e.type = SDL_FINGERDOWN;
            e.tfinger.fingerId = 100 + i;
            e.tfinger.x = x / static_cast<float>(drawWidth_);
            e.tfinger.y = y / static_cast<float>(drawHeight_);
            e.tfinger.pressure = 1.0f;
            SDL_PushEvent(&e);
        }
    }
    if (devFrames_ <= 0 || frameCount_ < static_cast<std::uint64_t>(devFrames_)) return;
    if (!devScreenshot_.empty()) {
        std::vector<std::uint8_t> px(static_cast<std::size_t>(drawWidth_) * drawHeight_ * 4);
        glReadPixels(0, 0, drawWidth_, drawHeight_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (std::FILE* f = std::fopen(devScreenshot_.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", drawWidth_, drawHeight_);
            for (int y = drawHeight_ - 1; y >= 0; --y) {
                for (int x = 0; x < drawWidth_; ++x) {
                    std::fwrite(&px[(static_cast<std::size_t>(y) * drawWidth_ + x) * 4], 1, 3, f);
                }
            }
            std::fclose(f);
            log::info("Screenshot written to {}", devScreenshot_);
        }
    }
    const auto& s = game_->stats();
    log::info("Exit after {} frames: ticks {} bullets {} peak {} grazes {} hits {} draw calls {}", frameCount_,
              s.ticks, s.bullets, s.peakBullets, s.grazes, s.hits, lastDrawCalls_);
    running_ = false;
}

void App::renderFrame() {
    // 1) The game at its native 640x480.
    target_.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    batch_.begin(kVirtualWidth, kVirtualHeight, &gpu_);
    game_->render(batch_, HudInfo{fps_, lastDrawCalls_});
    batch_.end();

    // 2) Letterboxed onto the screen, then the touch controls at full resolution.
    RenderTarget::bindScreen();
    glViewport(0, 0, drawWidth_, drawHeight_);
    glClearColor(0.02f, 0.02f, 0.03f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    batch_.begin(static_cast<float>(drawWidth_), static_cast<float>(drawHeight_), &gpu_);
    SpriteInstance blit;
    blit.x = gameX_ + gameW_ * 0.5f;
    blit.y = gameY_ + gameH_ * 0.5f;
    blit.w = gameW_;
    blit.h = gameH_;
    blit.u0 = 0;
    blit.v0 = 1;  // GL framebuffers are stored bottom-up
    blit.u1 = 1;
    blit.v1 = 0;
    batch_.drawTexture(target_.texture(), Blend::Alpha, blit);
    touch_.draw(batch_, ui_);
    batch_.end();
    lastDrawCalls_ = batch_.takeDrawCalls();
}

}  // namespace th06
