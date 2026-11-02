#include "engine/App.hpp"

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
#include "platform/CrashHandler.hpp"
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
#if defined(__ANDROID__)
    display_ = readAndroidDisplayOptions();
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
    if (char* pref = SDL_GetPrefPath("th06", "th06-android")) {
        const std::string logs = std::string(pref) + "logs";
        SDL_free(pref);
        std::error_code ec;
        std::filesystem::create_directories(logs, ec);
        if (log::openFile(logs + "/session.log", logs + "/previous.log")) installCrashHandler(logs + "/crash.txt");
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
        else if (a == "--stage" && next) startStage_ = std::atoi(argv[++i]), quickStart_ = true;
        else if (a == "--difficulty" && next) startDifficulty_ = std::atoi(argv[++i]), quickStart_ = true;
        else if (a == "--shot" && next) shotType_ = std::atoi(argv[++i]), quickStart_ = true;
        else if (a == "--quick") quickStart_ = true;
        else if (a == "--dpad") controls_.drag = false;
        else if (a == "--autofire") controls_.autoFire = true;
        else if (a == "--left-handed") controls_.leftHanded = true;
        else if (a == "--slow" && next) controls_.speedPercent = std::atoi(argv[++i]);
        else if (a == "--hitbox") controls_.showHitbox = true;
        else if (a == "--savestates") controls_.saveStates = true;
        else if (a == "--keys" && next) parseDevKeys(argv[++i]);
        else if (a == "--translation" && next) translation_ = argv[++i];  // patch folders, comma-separated
        else if (a == "--mischief" && next) mischief_ = static_cast<std::uint32_t>(std::atoi(argv[++i]));
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
        if (vfs_->looseFileCount() == 0) log::info("No bgm/*.wav in the game folder: only replaced songs play");
    }
#if defined(__ANDROID__)
    {
        const AndroidGameOptions o = readAndroidGameOptions();
        shotType_ = o.shotType;
        startStage_ = o.stage;
        startDifficulty_ = o.stage == 7 ? Extra : o.difficulty;
        mischief_ = o.mischief;
        quickStart_ = o.quickStart;
        controls_ = readAndroidControlOptions();
    }
#endif
    // The game RNG starts from the clock, as the original's does.
    stage_->seedRandom(static_cast<std::uint16_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    {
        // The game's text in another language (the launcher's downloaded translation).
        std::vector<std::string> folders;
#if defined(__ANDROID__)
        folders = readAndroidGameTextStack();
#else
        std::string list = translation_;
        if (list.empty()) {
            if (const char* env = std::getenv("TH06_TRANSLATION")) list = env;
        }
        for (std::size_t at = 0; at < list.size();) {
            const std::size_t comma = list.find(',', at);
            const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!item.empty()) folders.push_back(item);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
#endif
        stage_->setTranslation(std::move(folders));
    }
    stage_->setShotType(shotType_);
    stage_->setMischief(mischief_);
    stage_->setAutoFire(controls_.autoFire);
    stage_->setPracticeTools(controls_.speedPercent, controls_.showHitbox, controls_.saveStates);
    if (char* pref = SDL_GetPrefPath("th06", "th06-android")) {  // app storage on Android
        stage_->setScorePath(std::string(pref) + "score.dat");
        stage_->setConfigPath(std::string(pref) + "th06.cfg");
        // The launcher's replaced music: <files>/music/th06_NN.wav (+ .pos loop points).
        if (music_) music_->setOverrideDir(std::string(pref) + "music/");
        stage_->setReplayDir(std::string(pref) + "replay/");  // th6_01..15.rpy, as the PC game's replay folder
        SDL_free(pref);
    }
    // The game's title screen, unless the launcher asked for a quick start (or the title
    // files are missing).
    if (quickStart_ || !stage_->openTitle(true)) {
        if (!stage_->quickStart(startStage_, startDifficulty_, quickStart_)) log::warn("Stage not started: {}", stage_->error());
    }

    if (!createGpu(&err)) {
        log::error("GPU setup failed: {}", err);
        return false;
    }

    if (!touch_.attach(&err)) log::warn("Touch controls unavailable: {}", err);
    touch_.setOptions(controls_);
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
    const float w = static_cast<float>(drawWidth_), h = static_cast<float>(drawHeight_);
    auto snap = [&](float scale) {  // pixel-perfect: whole multiples once there is room
        return display_.scaling == DisplayOptions::PixelPerfect && scale >= 1.0f ? std::floor(scale) : scale;
    };
    // The whole 640x480 picture (landscape, and every screen but the playfield in portrait).
    const bool upright = h > w;
    const float scale = snap(std::min(w / kVirtualWidth, h / kVirtualHeight));
    gameW_ = kVirtualWidth * scale;
    gameH_ = kVirtualHeight * scale;
    gameX_ = std::floor((w - gameW_) * 0.5f);
    gameY_ = std::floor(upright ? h * 0.06f : (h - gameH_) * 0.5f);
    // Portrait play: the 384x448 playfield across the width (a strip kept clear at the top for
    // the camera cut-out), the panel's score rows (224x224 from its top) under its left half.
    const float top = std::floor(h * 0.035f);
    const float ps = snap(std::min(w / kGameRegionWidth, (h * 0.62f) / kGameRegionHeight));
    playW_ = kGameRegionWidth * ps;
    playH_ = kGameRegionHeight * ps;
    playX_ = std::floor((w - playW_) * 0.5f);
    playY_ = top;
    const float below = h - (playY_ + playH_);
    const float panelScale = std::max(0.5f, std::min(ps * 0.75f, below * 0.45f / 224.0f));
    panelW_ = panelH_ = 224.0f * panelScale;
    panelX_ = playX_ + std::floor(w * 0.02f);
    panelY_ = playY_ + playH_ + std::floor(below * 0.03f);
    lastPlayView_ = playView();
    layoutTouch();
    log::info("Screen {}x{}, game area {:.0f}x{:.0f} at ({:.0f},{:.0f}){}", drawWidth_, drawHeight_, gameW_, gameH_, gameX_,
              gameY_, display_.portrait ? " (portrait layout)" : "");
}

bool App::playView() const {
    if (!display_.portrait || drawHeight_ <= drawWidth_ || !stage_) return false;
    return stage_->ready() && !stage_->inMenus() && !stage_->inResults() && !stage_->inEnding();
}

// The touch controls follow the picture: around the playfield in the portrait play view (with
// the drag scaled to playfield pixels), around the whole picture otherwise.
void App::layoutTouch() {
    if (lastPlayView_) {
        touch_.layout(drawWidth_, drawHeight_, playX_, playY_, playW_, playH_, kGameRegionWidth / playW_);
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
                if (stage_->ready() || stage_->inMenus()) stage_->pressMenu();  // on the title: Cancel
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
        // Practice slow motion runs the stage in play only (not the title, menus or results).
        const bool slow = stage_->ready() && !stage_->inMenus() && !stage_->inResults() && !stage_->inEnding() &&
                          !stage_->menus().frozen();
        clock_.setSpeed(slow ? controls_.speedPercent : 100);
        const int ticks = clock_.advance(now);
        if (ticks > 0) {
            InputState in = input_.sample();
            touch_.fillDrag(in);
            applyDevKeys(in);
            for (int i = 0; i < ticks && !stage_->quitRequested(); ++i) {
                stage_->tick(in);
                ++tickCount_;
            }
        }
        haptics();
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

// A short vibration when the player is hit (after the deathbomb window) or bombs.
void App::haptics() {
    if (!stage_->ready() || stage_->inMenus()) return;
    const GameState& gs = stage_->state();
    const bool missed = gs.deaths > lastDeaths_, bombed = gs.bombsUsed > lastBombs_;
    lastDeaths_ = gs.deaths;
    lastBombs_ = gs.bombsUsed;
    if (!controls_.vibrate || !(missed || bombed)) return;
#if defined(__ANDROID__)
    androidVibrate(missed ? 80 : 35);
#else
    log::info("(vibrate {} ms)", missed ? 80 : 35);
#endif
}

void App::logStats() const {
    if (stage_->inMenus()) {
        log::info("fps {:.1f} | ticks {} | title screens", fps_, tickCount_);
        return;
    }
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
    if (devGameOverFrame_ >= 0 && frameCount_ == static_cast<std::uint64_t>(devGameOverFrame_)) stage_->endGameForTesting();
    if (devClearFrame_ >= 0 && frameCount_ == static_cast<std::uint64_t>(devClearFrame_)) stage_->clearGameForTesting();
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

// --keys "30:shoot,60:down,...": presses a button once game tick N is reached, for one
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
    const std::string& key = devKeys_[devKeyNext_].key;
    devKeyHeld_ = true;
    if (key == "up") in.up = true;
    else if (key == "down") in.down = true;
    else if (key == "left") in.left = true;
    else if (key == "right") in.right = true;
    else if (key == "shoot") in.shoot = true;
    else if (key == "bomb") in.bomb = true;
    else if (key == "pause") in.pause = true;
    else if (key == "quit") in.quit = true;
    else if (key == "save") in.saveState = true;
    else if (key == "load") in.loadState = true;
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

// Draws part of the 640x480 game picture (sx..: picture pixels) at dx.. on the screen.
void App::blitRegion(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh) {
    SpriteInstance blit;
    blit.x = dx + dw * 0.5f;
    blit.y = dy + dh * 0.5f;
    blit.w = dw;
    blit.h = dh;
    blit.u0 = sx / kVirtualWidth;
    blit.u1 = (sx + sw) / kVirtualWidth;
    blit.v0 = 1.0f - sy / kVirtualHeight;  // GL framebuffers are stored bottom-up
    blit.v1 = 1.0f - (sy + sh) / kVirtualHeight;
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
    // 1) The game at its native 640x480.
    target_.bind();
    glClearColor(0, 0, 0, 1);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    batch_.begin(kVirtualWidth, kVirtualHeight, &gpu_);
    stage_->render(batch_, alpha, HudInfo{fps_, lastDrawCalls_});
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
        blitRegion(kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight, playX_, playY_, playW_, playH_);
        blitRegion(kGameRegionX + kGameRegionWidth, kGameRegionY, 224.0f, 224.0f, panelX_, panelY_, panelW_, panelH_);
        if (display_.scanlines) {
            drawScanlines(playX_, playY_, playW_, playH_, static_cast<int>(kGameRegionHeight));
            drawScanlines(panelX_, panelY_, panelW_, panelH_, 224);
        }
    } else {
        blitRegion(0, 0, kVirtualWidth, kVirtualHeight, gameX_, gameY_, gameW_, gameH_);
        if (display_.scanlines) drawScanlines(gameX_, gameY_, gameW_, gameH_, kVirtualHeight);
    }
    touch_.draw(batch_, ui_);
    batch_.end();
    lastDrawCalls_ = batch_.takeDrawCalls();
}

}  // namespace th06
