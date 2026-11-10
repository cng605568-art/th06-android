#include "th08/Th08Game.hpp"

#include <format>
#include <fstream>
#include <iterator>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"

namespace touhou::th08 {

const GameInfo& Th08Game::info() const {
    static const GameInfo kInfo{
        .id = "th08",
        .windowTitle = "TH08 Port",
        .prefOrg = "th08",
        .prefApp = "th08-android",
        .envPrefix = "TH08",
    };
    return kInfo;
}

Th08Game::~Th08Game() {
    if (menu_) saveConfig();
    if (music_) music_->shutdown();
}

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::uint16_t buttonWord(const InputState& in) {
    std::uint16_t b = 0;
    if (in.shoot) b |= BtnShoot;
    if (in.bomb) b |= BtnBomb;
    if (in.focus) b |= BtnFocus;
    if (in.pause) b |= BtnMenu;
    if (in.up) b |= BtnUp;
    if (in.down) b |= BtnDown;
    if (in.left) b |= BtnLeft;
    if (in.right) b |= BtnRight;
    if (in.skip) b |= BtnSkip;
    if (in.quit) b |= BtnQ;
    return b;
}

}  // namespace

void Th08Game::saveConfig() const {
    if (storage_.empty()) return;
    const auto bytes = config_.save();
    std::ofstream f(storage_ + "th08.cfg", std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool Th08Game::start(const GameContext& ctx) {
    ui_ = ctx.ui;
    storage_ = ctx.storageDir;
    rng_.seed(ctx.seed);
    if (ctx.vfs) folder_ = inspectFolder(*ctx.vfs);
    log::info("th08.dat 1.00d: {}; th08.exe 1.00d: {}; thbgm.dat: {}; {} files", folder_.archive ? "mounted" : "missing",
              folder_.exe ? "found" : "missing", folder_.thbgm ? "found" : "missing", folder_.files);
    if (!folder_.archive || !ctx.vfs || !ctx.textures) return true;  // the status screen

    // th08.cfg (the port's own copy), re-initialised and written back when invalid.
    if (!storage_.empty()) {
        if (!config_.load(readFile(storage_ + "th08.cfg"), folder_.thbgm)) log::info("th08.cfg initialised");
        saveConfig();
    } else {
        config_.load({}, folder_.thbgm);
    }
    if (folder_.exe) {
        std::string why;
        tablesLoaded_ = loadExeTables(folder_.exeBytes, tables_, &why);
        if (!tablesLoaded_) log::warn("th08.exe tables: {}", why);
    }
    if (tablesLoaded_ && ctx.samples) {
        sound_ = std::make_unique<SoundPlayer>(ctx.samples, &tables_);
        log::info("Loaded {} of {} sound effects", sound_->loadSamples(*ctx.vfs), ExeTables::kSamples);
        sound_->setVolumePercent(config_.seVolume);
    }
    if (ctx.music) {
        std::vector<BgmRecord> records;
        if (const auto fmt = ctx.vfs->read("bgm/thbgm.fmt")) records = parseThbgmFmt(*fmt);
        songs_ = records.size();
        std::span<const std::uint8_t> thbgm = ctx.vfs->looseFile("thbgm.dat");
        if (!thbgm.empty() && !validThbgmHeader(thbgm)) {
            log::warn("thbgm.dat has the wrong header: playing without music");
            thbgm = {};
        }
        music_ = std::make_unique<BgmPlayer>(ctx.music);
        music_->setData(std::move(records), thbgm);
        music_->setMode(config_.music);
        music_->setVolumePercent(config_.bgmVolume);
        if (!storage_.empty()) music_->setOverrideDir(storage_ + "music/");
        log::info("Music: {} song(s) in thbgm.fmt, thbgm.dat {}", songs_, thbgm.empty() ? "missing" : "found");
    }

    // The files the boot loads (spec 1.3): text.anm (text is drawn into it), ascii.anm,
    // capture.anm, nowloading.anm.
    std::string err;
    bool ok = anm_.load(*ctx.vfs, *ctx.textures, "text.anm", kSlotText, &err, true);
    ok = ok && anm_.load(*ctx.vfs, *ctx.textures, "ascii.anm", kSlotAscii, &err);
    ok = ok && anm_.load(*ctx.vfs, *ctx.textures, "capture.anm", kSlotCapture, &err, true);
    ok = ok && anm_.load(*ctx.vfs, *ctx.textures, "nowloading.anm", kSlotNowLoading, &err);
    if (!ok) {
        log::warn("{}", err);
        return true;
    }
    if (!ctx.text) fallbackText_ = std::make_unique<FallbackTextRenderer>();

    flow_.vfs = ctx.vfs;
    flow_.textures = ctx.textures;
    flow_.text = ctx.text ? ctx.text : fallbackText_.get();
    flow_.ui = ctx.ui;
    flow_.anm = &anm_;
    flow_.rng = &rng_;
    flow_.sound = sound_.get();
    flow_.music = music_.get();
    flow_.tables = tablesLoaded_ ? &tables_ : nullptr;
    flow_.config = &config_;
    flow_.request = [this](Scene s) {
        if (s == Scene::Exit) quit_ = true;
    };
    flow_.saveConfig = [this]() { saveConfig(); };
    menu_ = std::make_unique<MainMenu>(flow_);
    if (!menu_->create(&err)) {
        log::warn("The title could not be built: {}", err);
        menu_.reset();
    }
    return true;
}

void Th08Game::tick(const InputState& input) {
    ++ticks_;
    if (music_) music_->fadeStep();
    if (menu_) {
        input_.sample(buttonWord(input));
        menu_->update(input_);
    } else if (input.quit) {
        quit_ = true;
    }
    if (music_) music_->pump();
    if (sound_) sound_->flush();
}

void Th08Game::render(SpriteSink& sink, float, const FrameInfo&) {
    if (menu_) {
        menu_->draw(sink);
        return;
    }
    if (!ui_) return;
    const std::uint32_t moon = rgba(250, 226, 150), white = rgba(255, 255, 255), grey = rgba(170, 170, 170);
    const std::uint32_t good = rgba(140, 230, 140), bad = rgba(255, 140, 140);
    drawText(sink, *ui_, 128, 120, "IMPERISHABLE NIGHT", 3.0f, moon);
    drawText(sink, *ui_, 176, 168, "BEING PORTED", 3.0f, white);
    drawText(sink, *ui_, 136, 240, folder_.archive ? "TH08.DAT 1.00D: FOUND" : "TH08.DAT 1.00D: MISSING", 2.0f,
             folder_.archive ? good : bad);
    drawText(sink, *ui_, 136, 272, folder_.thbgm ? "THBGM.DAT: FOUND" : "THBGM.DAT: MISSING", 2.0f,
             folder_.thbgm ? good : bad);
    drawText(sink, *ui_, 136, 336, folder_.exe ? "TH08.EXE 1.00D: FOUND" : "TH08.EXE 1.00D: MISSING", 2.0f,
             folder_.exe ? good : bad);
    drawText(sink, *ui_, 136, 416, std::format("{} FILES IN THE GAME FOLDER", folder_.files), 2.0f, grey);
}

void Th08Game::logStatus(const FrameInfo& frame) const {
    log::info("fps {:.1f} | ticks {} | {}", frame.fps, frame.ticks, menu_ ? "title" : "status screen");
}

}  // namespace touhou::th08
