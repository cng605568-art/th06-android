#include "host/App.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.hpp"
#include "gfx/Gl.hpp"
#include "platform/CrashHandler.hpp"
#include "platform/DataMount.hpp"
#include "platform/Platform.hpp"
#include "gfx/RenderTarget.hpp"
#if defined(TOUHOU_IOS)
#include <SDL_syswm.h>
#endif

namespace touhou {
namespace {

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
    if (!game_) return false;
    const GameInfo& info = game_->info();
    log::setTag(info.id);
#if defined(TOUHOU_PHONE)
    display_ = readPlatformDisplayOptions();
#else
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--portrait") display_.portrait = true;
        else if (a == "--sharp") display_.scaling = DisplayOptions::Sharp;
        else if (a == "--pixel") display_.scaling = DisplayOptions::PixelPerfect;
        else if (a == "--scanlines") display_.scanlines = true;
    }
#endif
    // The session log and the crash report, kept in the app's storage for bug reports (the
    // launcher's "Save a log").
    std::string storage;  // the app's storage, ending in '/'
#if defined(TOUHOU_IOS)
    storage = iosStorageDir();  // Documents/Saves/: visible in the Files app
#else
    if (char* pref = SDL_GetPrefPath(info.prefOrg, info.prefApp)) {
        storage = pref;
        SDL_free(pref);
    }
#endif
    if (!storage.empty()) {
        const std::string logs = storage + "logs";
        std::error_code ec;
        std::filesystem::create_directories(logs, ec);
        if (log::openFile(logs + "/session.log", logs + "/previous.log")) installCrashHandler(logs + "/crash.txt", info.id);
    }
    SDL_SetHint(SDL_HINT_ORIENTATIONS, display_.portrait ? "Portrait PortraitUpsideDown" : "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        log::error("SDL_Init: {}", SDL_GetError());
        return false;
    }
    sdlReady_ = true;
    // Without a sound device the game still runs (silently).
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) log::warn("No audio: {}", SDL_GetError());
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
        else if (a == "--ticks" && next) devTicks_ = std::atoi(argv[++i]);
        else if (a == "--screenshot" && next) devScreenshot_ = argv[++i];
        else if (a == "--demo-touch") devTouch_ = true;
        else if (a == "--demo-bomb" && next) devBombFrame_ = std::atoi(argv[++i]);
        else if (a == "--demo-pause" && next) devPauseFrame_ = std::atoi(argv[++i]);
        else if (a == "--demo-gameover" && next) devGameOverFrame_ = std::atoi(argv[++i]);
        else if (a == "--demo-clear" && next) devClearFrame_ = std::atoi(argv[++i]);
        else if (a == "--dpad") controls_.drag = false;
        else if (a == "--autofire") controls_.autoFire = true;
        else if (a == "--left-handed") controls_.leftHanded = true;
        else if (a == "--slow" && next) controls_.speedPercent = std::atoi(argv[++i]);
        else if (a == "--hitbox") controls_.showHitbox = true;
        else if (a == "--savestates") controls_.saveStates = true;
        else if (a == "--keys" && next) parseDevKeys(argv[++i]);
        else if (a == "--size" && next) std::sscanf(argv[++i], "%dx%d", &windowWidth_, &windowHeight_);
        else if (a == "--seed" && next) devSeed_ = std::atoi(argv[++i]);
        else if (a == "--lockstep") devLockstep_ = true;
    }
#endif

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(TOUHOU_PHONE)
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
#if defined(TOUHOU_IOS)
    flags |= SDL_WINDOW_BORDERLESS;  // no status bar
#endif
    window_ = SDL_CreateWindow(info.windowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
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
#if defined(TOUHOU_IOS)
    RenderTarget::setScreenFramebuffer(iosScreenFramebuffer(window_));  // SDL's, not 0
#endif
    vsync_ = SDL_GL_SetSwapInterval(1) == 0;
    SDL_DisplayMode mode;
    if (SDL_GetWindowDisplayMode(window_, &mode) == 0 && mode.refresh_rate > 0) refreshHz_ = mode.refresh_rate;
    log::info("GL: {} / {} | vsync {} | display {} Hz",
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)), vsync_ ? "on" : "off", refreshHz_);

    clock_.setDisplay(refreshHz_, vsync_);

    vfs_ = mountGameData(argc, argv, info.envPrefix);
    ui_ = registerUiArt(textures_);

    std::string err;
    if (!audio_.open(&err)) log::warn("Audio disabled: {}", err);

#if defined(TOUHOU_PHONE)
    text_ = makePlatformTextRenderer();
    controls_ = readPlatformControlOptions();
#endif
    {
        GameContext ctx;
        ctx.argc = argc;
        ctx.argv = argv;
        ctx.vfs = vfs_.get();
        ctx.textures = &textures_;
        ctx.ui = &ui_;
        ctx.samples = audio_.isOpen() ? &audio_ : nullptr;
        ctx.music = audio_.isOpen() ? &audio_ : nullptr;
        ctx.text = text_.get();
        ctx.controls = controls_;
        ctx.storageDir = storage;
        // The game RNG starts from the clock, as the originals' do (desktop: --seed N fixes it).
        ctx.seed = devSeed_ >= 0 ? static_cast<std::uint16_t>(devSeed_)
                                 : static_cast<std::uint16_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  std::chrono::system_clock::now().time_since_epoch())
                                                                  .count());
        if (!game_->start(ctx)) {
            log::error("{} did not start", info.id);
            return false;
        }
    }

    if (!createGpu(&err)) {
        log::error("GPU setup failed: {}", err);
        return false;
    }

    if (!touch_.attach(&err)) log::warn("Touch controls unavailable: {}", err);
    touch_.setOptions(controls_);
#if defined(TOUHOU_PHONE)
    touch_.setVisible(true);
#else
    touch_.setVisible(devTouch_ || hasFlag(argc, argv, "--touch") || SDL_GetNumTouchDevices() > 0);
#endif
    relayout();
    return true;
}

bool App::createGpu(std::string* error) {
    if (!batch_.create(error)) return false;
    if (!target_.create(game_->info().width, game_->info().height, error)) return false;
    gpu_.sync(textures_);
    return true;
}

void App::releaseGpu(bool contextLost) {
    if (game_) game_->setCaptureReady(false);  // the snapshot lived in a GL texture
    if (captureFbo_ && !contextLost) glDeleteFramebuffers(1, &captureFbo_);
    captureFbo_ = 0;
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
    game_.reset();  // before the mixer (the game's sounds and music play through it) and the files
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
    const GameInfo& info = game_->info();
    const float w = static_cast<float>(drawWidth_), h = static_cast<float>(drawHeight_);
    auto snap = [&](float scale) {  // pixel-perfect: whole multiples once there is room
        return display_.scaling == DisplayOptions::PixelPerfect && scale >= 1.0f ? std::floor(scale) : scale;
    };
    // The whole picture (landscape, and every screen but the playfield in portrait).
    const bool upright = h > w;
    const float scale = snap(std::min(w / info.width, h / info.height));
    gameW_ = info.width * scale;
    gameH_ = info.height * scale;
    gameX_ = std::floor((w - gameW_) * 0.5f);
    gameY_ = std::floor(upright ? h * 0.06f : (h - gameH_) * 0.5f);
    // Portrait play: the playfield across the width (a strip kept clear at the top for the
    // camera cut-out), the panel's score rows (th06: 224x224 from its top) under its left half.
    const float top = std::floor(h * 0.035f);
    const float ps = snap(std::min(w / info.playW, (h * 0.62f) / info.playH));
    playW_ = info.playW * ps;
    playH_ = info.playH * ps;
    playX_ = std::floor((w - playW_) * 0.5f);
    playY_ = top;
    const float below = h - (playY_ + playH_);
    const float panelScale = std::max(0.5f, std::min(ps * 0.75f, below * 0.45f / info.panelH));
    panelW_ = info.panelW * panelScale;
    panelH_ = info.panelH * panelScale;
    panelX_ = playX_ + std::floor(w * 0.02f);
    panelY_ = playY_ + playH_ + std::floor(below * 0.03f);
    lastPlayView_ = playView();
    layoutTouch();
    log::info("Screen {}x{}, game area {:.0f}x{:.0f} at ({:.0f},{:.0f}){}", drawWidth_, drawHeight_, gameW_, gameH_, gameX_,
              gameY_, display_.portrait ? " (portrait layout)" : "");
}

bool App::playView() const {
    if (!display_.portrait || drawHeight_ <= drawWidth_ || !game_) return false;
    return game_->showsPlayfield();
}

// The touch controls follow the picture: around the playfield in the portrait play view (with
// the drag scaled to playfield pixels), around the whole picture otherwise.
void App::layoutTouch() {
    if (lastPlayView_) {
        touch_.layout(drawWidth_, drawHeight_, playX_, playY_, playW_, playH_, game_->info().playW / playW_);
    } else {
        touch_.layout(drawWidth_, drawHeight_, gameX_, gameY_, gameW_, gameH_);
    }
}

void App::handleEvent(const SDL_Event& e) {
    if (input_.handleEvent(e, touch_.joystickId())) touch_.setVisible(false);

    switch (e.type) {
        case SDL_QUIT:
        case SDL_APP_TERMINATING:
            running_ = false;
            break;
        case SDL_APP_WILLENTERBACKGROUND:
            if (game_) game_->pauseNow();
            audio_.setPaused(true);
            touch_.releaseAll();
            background_ = true;  // iOS ends an app that draws while in the background
            break;
        case SDL_APP_DIDENTERFOREGROUND:
            audio_.setPaused(false);
            resetClock_ = true;
            background_ = false;
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
            // Android Back: the game's Menu button (th06: it opens the pause menu and resumes
            // from it, Quit is in the menu). When the game does not use it, it leaves.
            if (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK && !e.key.repeat && game_) {
                if (!game_->back()) running_ = false;
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
    using Clock = FixedTimestep::Clock;
    auto fpsStart = Clock::now();
    auto logStart = fpsStart;
    int fpsFrames = 0;

    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        if (!running_) break;
        if (background_) {  // (Android blocks in SDL instead; iOS keeps calling)
            SDL_Delay(50);
            continue;
        }

        const auto now = Clock::now();
        if (resetClock_) {
            clock_.reset(now);  // after start-up or resume: no catch-up burst
            resetClock_ = false;
        }

        // The simulation, in whole 1/60 s ticks. Input is sampled once per rendered frame and
        // applies to every tick it runs. Practice slow motion applies only where the game says
        // (th06: in play, not on the title, in menus or on the result screen).
        clock_.setSpeed(game_->practiceSpeedApplies() ? controls_.speedPercent : 100);
        const int ticks = devLockstep_ ? 1 : clock_.advance(now);
        if (ticks > 0) {
            InputState in = input_.sample();
            touch_.fillDrag(in);
            applyDevKeys(in);
            for (int i = 0; i < ticks && !game_->quitRequested(); ++i) {
                game_->tick(in);
                ++tickCount_;
            }
        }
        haptics();
        if (game_->quitRequested()) {  // e.g. Quit in the pause menu: back to the launcher
            running_ = false;
            break;
        }

        // Rendering: once per display refresh, interpolated to the present instant.
        gpu_.sync(textures_);
        if (game_->takeCaptureRequest()) game_->setCaptureReady(capturePlayfield());
        renderFrame(game_->paused() || devLockstep_ ? 1.0f : clock_.alpha());
        developerFrameHooks();
        SDL_GL_SwapWindow(window_);

        if (!vsync_) {
            // No vsync: sleep until the next tick is due rather than spinning.
            const auto wait = clock_.untilNextTick() - (Clock::now() - now);
            if (wait > std::chrono::milliseconds(2)) {
                std::this_thread::sleep_for(wait - std::chrono::milliseconds(1));
            }
        }

        ++fpsFrames;
        if (now - fpsStart >= std::chrono::milliseconds(500)) {
            fps_ = static_cast<float>(fpsFrames / std::chrono::duration<double>(now - fpsStart).count());
            fpsFrames = 0;
            fpsStart = now;
        }
        if (now - logStart >= std::chrono::seconds(10)) {
            game_->logStatus(frameInfo());
            logStart = now;
        }
    }
}

// A short vibration when the game asks for one (th06: a miss or a bomb), if the player
// keeps vibration on.
void App::haptics() {
    const int ms = game_->takeVibration();
    if (ms <= 0 || !controls_.vibrate) return;
#if defined(TOUHOU_PHONE)
    platformVibrate(ms);
#else
    log::info("(vibrate {} ms)", ms);
#endif
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
    if (devBombFrame_ >= 0 && (frameCount_ == static_cast<std::uint64_t>(devBombFrame_) ||
                               frameCount_ == static_cast<std::uint64_t>(devBombFrame_) + 30)) {
        float x = 0, y = 0;
        if (touch_.locate(SDL_CONTROLLER_BUTTON_B, x, y)) {
            SDL_Event e{};
            e.type = frameCount_ == static_cast<std::uint64_t>(devBombFrame_) ? SDL_FINGERDOWN : SDL_FINGERUP;
            e.tfinger.fingerId = 102;
            e.tfinger.x = x / static_cast<float>(drawWidth_);
            e.tfinger.y = y / static_cast<float>(drawHeight_);
            e.tfinger.pressure = 1.0f;
            SDL_PushEvent(&e);
        }
    }
    if (devPauseFrame_ >= 0 && frameCount_ == static_cast<std::uint64_t>(devPauseFrame_)) {
        game_->devCommand(Game::DevCommand::Menu);
    }
    if (devGameOverFrame_ >= 0 && frameCount_ == static_cast<std::uint64_t>(devGameOverFrame_)) {
        game_->devCommand(Game::DevCommand::GameOver);
    }
    if (devClearFrame_ >= 0 && frameCount_ == static_cast<std::uint64_t>(devClearFrame_)) {
        game_->devCommand(Game::DevCommand::Clear);
    }
    const bool byFrames = devFrames_ > 0 && frameCount_ >= static_cast<std::uint64_t>(devFrames_);
    const bool byTicks = devTicks_ > 0 && tickCount_ >= static_cast<std::uint64_t>(devTicks_);
    if (!byFrames && !byTicks) return;
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
    log::info("Exit after {} frames", frameCount_);
    game_->logStatus(frameInfo());
    game_->logSummary();
    running_ = false;
}

// --keys "30:shoot,60:down,90:bomb+focus,...": presses buttons once game tick N is reached, for one
// input sample (one or more ticks), then releases everything for one sample before the
// next key, so two presses never merge into one.
void App::parseDevKeys(const std::string& spec) {
    std::size_t at = 0;
    while (at < spec.size()) {
        std::size_t end = spec.find(',', at);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(at, end - at);
        const std::size_t colon = item.find(':');
        if (colon != std::string::npos) {
            devKeys_.push_back({static_cast<std::uint64_t>(std::atoll(item.c_str())), item.substr(colon + 1)});
        }
        at = end + 1;
    }
}

void App::applyDevKeys(InputState& in) {
    if (devKeyHeld_) {  // the release sample
        devKeyHeld_ = false;
        ++devKeyNext_;
        return;
    }
    if (devKeyNext_ >= devKeys_.size() || tickCount_ < devKeys_[devKeyNext_].tick) return;
    const std::string& keys = devKeys_[devKeyNext_].key;
    devKeyHeld_ = true;
    // "a+b" presses several buttons in the same sample.
    std::size_t at = 0;
    while (at <= keys.size()) {
        std::size_t end = keys.find('+', at);
        if (end == std::string::npos) end = keys.size();
        const std::string key = keys.substr(at, end - at);
        if (key == "up") in.up = true;
        else if (key == "down") in.down = true;
        else if (key == "left") in.left = true;
        else if (key == "right") in.right = true;
        else if (key == "shoot") in.shoot = true;
        else if (key == "bomb") in.bomb = true;
        else if (key == "focus") in.focus = true;
        else if (key == "pause") in.pause = true;
        else if (key == "quit") in.quit = true;
        else if (key == "save") in.saveState = true;
        else if (key == "load") in.loadState = true;
        at = end + 1;
    }
}

bool App::capturePlayfield() {
    const int id = game_->captureTexture();
    if (id < 0 || frameCount_ == 0) return false;  // no capture.anm, or nothing rendered yet
    const auto tid = static_cast<std::uint16_t>(id);
    const GLuint tex = gpu_.get(tid);
    const int texW = textures_.width(tid), texH = textures_.height(tid);
    // The destination rectangle, clipped to the texture. Texture row y is framebuffer row y
    // here (see below), so the rectangle needs no flip.
    Game::CaptureRect r = game_->captureRect();
    if (r.w <= 0 || r.h <= 0) r = {0, 0, texW, texH};
    r.w = std::min(r.w, texW - r.x);
    r.h = std::min(r.h, texH - r.y);
    if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0) return false;
    const int w = r.w, h = r.h;
    if (!captureFbo_) glGenFramebuffers(1, &captureFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, captureFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(r.x, r.y, w, h);
        glEnable(GL_SCISSOR_TEST);
        glScissor(r.x, r.y, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        batch_.begin(static_cast<float>(w), static_cast<float>(h), &gpu_);
        // The game target still holds the last frame, bottom-up. Drawn into the texture's
        // framebuffer the quad's bottom edge becomes texture row 0, so it samples the
        // playfield's top row there.
        SpriteInstance s;
        s.x = static_cast<float>(w) * 0.5f;
        s.y = static_cast<float>(h) * 0.5f;
        s.w = static_cast<float>(w);
        s.h = static_cast<float>(h);
        const GameInfo& info = game_->info();
        const float vw = static_cast<float>(info.width), vh = static_cast<float>(info.height);
        s.u0 = info.playX / vw;
        s.u1 = (info.playX + info.playW) / vw;
        s.v0 = (vh - info.playY - info.playH) / vh;
        s.v1 = (vh - info.playY) / vh;
        batch_.drawTexture(target_.texture(), Blend::Alpha, s);
        batch_.end();
    } else {
        log::warn("Playfield capture: framebuffer incomplete");
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    RenderTarget::bindScreen();
    gl::check("capturePlayfield");
    return ok;
}

// Draws part of the game's picture (sx..: picture pixels) at dx.. on the screen.
void App::blitRegion(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh) {
    const float vw = static_cast<float>(game_->info().width), vh = static_cast<float>(game_->info().height);
    SpriteInstance blit;
    blit.x = dx + dw * 0.5f;
    blit.y = dy + dh * 0.5f;
    blit.w = dw;
    blit.h = dh;
    blit.u0 = sx / vw;
    blit.u1 = (sx + sw) / vw;
    blit.v0 = 1.0f - sy / vh;  // GL framebuffers are stored bottom-up
    blit.v1 = 1.0f - (sy + sh) / vh;
    batch_.drawTexture(target_.texture(), Blend::Alpha, blit);
}

// A soft CRT look: the lower part of every picture row darkened.
void App::drawScanlines(float x, float y, float w, float h, int rows) {
    const float row = h / static_cast<float>(rows);
    if (row < 1.5f) return;  // too small to show
    for (int r = 0; r < rows; ++r) {
        batch_.fillRect(ui_.white, x, y + row * (static_cast<float>(r) + 0.55f), w, row * 0.45f, rgba(0, 0, 0, 70));
    }
}

void App::renderFrame(float alpha) {
    const GameInfo& info = game_->info();
    // 1) The game at its native size (th06: 640x480).
    target_.bind();
    glClearColor(0, 0, 0, 1);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(game_->keepsPreviousFrame() ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    batch_.begin(static_cast<float>(info.width), static_cast<float>(info.height), &gpu_);
    game_->render(batch_, alpha, frameInfo());
    batch_.end();

    // 2) Onto the screen (letterboxed, or the portrait play view), sharp or smooth, with the
    //    optional scanlines, then the touch controls at full resolution.
    const bool play = playView();
    if (play != lastPlayView_) {
        lastPlayView_ = play;
        layoutTouch();
    }
    RenderTarget::bindScreen();
    glViewport(0, 0, drawWidth_, drawHeight_);
    glClearColor(0.02f, 0.02f, 0.03f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLint filter = display_.scaling == DisplayOptions::Smooth ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D, target_.texture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    batch_.begin(static_cast<float>(drawWidth_), static_cast<float>(drawHeight_), &gpu_);
    if (play) {
        blitRegion(info.playX, info.playY, info.playW, info.playH, playX_, playY_, playW_, playH_);
        blitRegion(info.panelX, info.panelY, info.panelW, info.panelH, panelX_, panelY_, panelW_, panelH_);
        if (display_.scanlines) {
            drawScanlines(playX_, playY_, playW_, playH_, static_cast<int>(info.playH));
            drawScanlines(panelX_, panelY_, panelW_, panelH_, static_cast<int>(info.panelH));
        }
    } else {
        blitRegion(0, 0, static_cast<float>(info.width), static_cast<float>(info.height), gameX_, gameY_, gameW_, gameH_);
        if (display_.scanlines) drawScanlines(gameX_, gameY_, gameW_, gameH_, info.height);
    }
    touch_.draw(batch_, ui_);
    batch_.end();
    lastDrawCalls_ = batch_.takeDrawCalls();
}

}  // namespace touhou
