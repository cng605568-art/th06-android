#include "engine/App.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "core/Log.hpp"
#include "platform/DataMount.hpp"
#if defined(__ANDROID__)
#include "platform/AndroidStorage.hpp"
#endif

namespace th06 {
namespace {

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
        else if (a == "--stage" && next) startStage_ = std::atoi(argv[++i]);
        else if (a == "--difficulty" && next) startDifficulty_ = std::atoi(argv[++i]);
        else if (a == "--shot" && next) shotType_ = std::atoi(argv[++i]);
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

    clock_.setDisplay(refreshHz_, vsync_);

    vfs_ = mountGameData(argc, argv);
    ui_ = registerUiArt(textures_);

    std::string err;
    if (audio_.open(&err)) {
        sound_ = std::make_unique<SoundPlayer>(&audio_);
        const int found = sound_->loadSamples(*vfs_);
        log::info("Loaded {} of {} sound effects from the archives", found, SoundPlayer::kSamples);
    } else {
        log::warn("Audio disabled: {}", err);
    }

#if defined(__ANDROID__)
    text_ = makeAndroidTextRenderer();
#endif
    stage_ = std::make_unique<Stage>(textures_, ui_, vfs_.get(), sound_.get(), text_.get());
    if (audio_.isOpen()) {
        music_ = std::make_unique<MusicPlayer>(vfs_.get(), &audio_);
        stage_->setMusic(music_.get());
        if (vfs_->looseFileCount() == 0) log::info("No bgm/*.wav in the game folder: music is silent");
    }
#if defined(__ANDROID__)
    {
        const AndroidGameOptions o = readAndroidGameOptions();
        shotType_ = o.shotType;
        startStage_ = o.stage;
        startDifficulty_ = o.stage == 7 ? Extra : o.difficulty;
    }
#endif
    stage_->setShotType(shotType_);
    if (!stage_->load(startStage_, startDifficulty_)) log::warn("Stage not started: {}", stage_->error());

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

bool App::createGpu(std::string* error) {
    if (!batch_.create(error)) return false;
    if (!target_.create(kVirtualWidth, kVirtualHeight, error)) return false;
    gpu_.sync(textures_);
    return true;
}

void App::releaseGpu(bool contextLost) {
    if (stage_) stage_->setCaptureReady(false);  // the snapshot lived in a GL texture
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
    stage_.reset();  // before the mixer: the stage's SoundPlayer points at it
    if (music_) music_->stop();
    music_.reset();
    sound_.reset();
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
            if (stage_) stage_->pauseNow();
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
            // Android Back is the Menu button: it opens the pause menu and resumes from it
            // (Quit is in the menu). Without a running stage it leaves.
            if (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK && !e.key.repeat && stage_) {
                if (stage_->ready()) stage_->pressMenu();
                else running_ = false;
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

        const auto now = Clock::now();
        if (resetClock_) {
            clock_.reset(now);  // after start-up or resume: no catch-up burst
            resetClock_ = false;
        }

        // Physics: player movement, ECL, bullets and collision, in whole 1/60 s ticks.
        // Input is sampled once per rendered frame and applies to every tick it runs.
        const int ticks = clock_.advance(now);
        if (ticks > 0) {
            const InputState in = input_.sample();
            for (int i = 0; i < ticks && !stage_->quitRequested(); ++i) {
                stage_->tick(in);
                ++tickCount_;
            }
        }
        if (stage_->quitRequested()) {  // Quit in the pause menu, or game over: to the launcher
            running_ = false;
            break;
        }

        // Rendering: once per display refresh, interpolated to the present instant.
        gpu_.sync(textures_);
        if (stage_->takeCaptureRequest()) stage_->setCaptureReady(capturePlayfield());
        renderFrame(stage_->paused() ? 1.0f : clock_.alpha());
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
            logStats();
            logStart = now;
        }
    }
}

void App::logStats() const {
    if (!stage_->ready()) {
        log::info("fps {:.1f} | ticks {} | stage not running: {}", fps_, tickCount_, stage_->error());
        return;
    }
    Stage& st = *stage_;
    log::info("fps {:.1f} | ticks {} | stage {} frame {} | enemies {} | bullets {} (peak {}, slots full {}) | "
              "ECL instructions {} | draw calls {}",
              fps_, tickCount_, st.stageNumber(), st.enemies().timelineTime(), st.enemies().activeCount(),
              st.bullets().activeCount(), st.bullets().peakCount(), st.bullets().spawnFailures(),
              st.enemies().instructionsExecuted(), lastDrawCalls_);
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
    if (devPauseFrame_ >= 0 && frameCount_ == static_cast<std::uint64_t>(devPauseFrame_)) stage_->pressMenu();
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
    logStats();
    if (stage_->ready()) {
        const GameState& gs = stage_->state();
        log::info("score {} | grazes {} | misses {} | rank {}", gs.score, gs.grazes, gs.deaths, gs.rank);
        for (const auto& [name, count] : stage_->unported().counts()) log::info("  not ported: {} x{}", name, count);
    }
    running_ = false;
}

bool App::capturePlayfield() {
    const int id = stage_->captureTexture();
    if (id < 0 || frameCount_ == 0) return false;  // no capture.anm, or nothing rendered yet
    const auto tid = static_cast<std::uint16_t>(id);
    const GLuint tex = gpu_.get(tid);
    const int w = textures_.width(tid), h = textures_.height(tid);
    if (!captureFbo_) glGenFramebuffers(1, &captureFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, captureFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        batch_.begin(static_cast<float>(w), static_cast<float>(h), &gpu_);
        // The game target still holds the last frame, bottom-up. Drawn into the texture's
        // framebuffer the quad's bottom edge becomes texture row 0, so it samples the
        // playfield's top row there.
        SpriteInstance s;
        s.x = static_cast<float>(w) * 0.5f;
        s.y = static_cast<float>(h) * 0.5f;
        s.w = static_cast<float>(w);
        s.h = static_cast<float>(h);
        s.u0 = kGameRegionX / kVirtualWidth;
        s.u1 = (kGameRegionX + kGameRegionWidth) / kVirtualWidth;
        s.v0 = (kVirtualHeight - kGameRegionY - kGameRegionHeight) / kVirtualHeight;
        s.v1 = (kVirtualHeight - kGameRegionY) / kVirtualHeight;
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

void App::renderFrame(float alpha) {
    // 1) The game at its native 640x480.
    target_.bind();
    glClearColor(0, 0, 0, 1);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    batch_.begin(kVirtualWidth, kVirtualHeight, &gpu_);
    stage_->render(batch_, alpha, HudInfo{fps_, lastDrawCalls_});
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
