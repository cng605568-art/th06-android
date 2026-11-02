#include "game/Ending.hpp"

#include <algorithm>
#include <memory>

#include "audio/MusicPlayer.hpp"
#include "core/Log.hpp"
#include "game/Gui.hpp"
#include "game/Translation.hpp"
#include "game/World.hpp"
#include "gfx/Image.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

// staff01..03.anm: slots 44..46 at offsets only 7 apart (as in the original).
constexpr int kStaffSlots[3] = {44, 45, 46};
constexpr const char* kStaffFiles[3] = {"data/staff01.anm", "data/staff02.anm", "data/staff03.anm"};
constexpr int kStaffOffsets[3] = {0x600, 0x607, 0x60E};
constexpr int kStaffBase = 0x600;
constexpr int kTextScriptFirst = 0x708;  // text.anm script 8 + slot
// The game's GUI art at the same offsets (front.anm, loading.anm), gone after the game.
constexpr int kGuiSlotsToRelease[2] = {13, 14};

bool lineEnd(std::uint8_t b) { return b == 0 || b == '\r' || b == '\n'; }

// @c values are Windows COLORREFs (0x00BBGGRR).
std::uint32_t colorRefToRgb(std::uint32_t n) { return ((n & 0xffu) << 16) | (n & 0xff00u) | ((n >> 16) & 0xffu); }

}  // namespace

std::string Ending::scriptFor(int shotType, int difficulty, bool continued) {
    const int character = (shotType & 3) / 2;
    if (difficulty == Easy || continued) return character == 0 ? "data/end00b.end" : "data/end10b.end";
    static constexpr const char* kGood[4] = {"data/end00.end", "data/end01.end", "data/end10.end", "data/end11.end"};
    return kGood[shotType & 3];
}

bool Ending::loadScript(std::string_view path) {
    auto bytes = vfs_ ? vfs_->read(path) : std::nullopt;
    if (!bytes) {
        log::warn("Ending: {} cannot be loaded", path);
        return false;
    }
    script_ = std::move(*bytes);
    if (translation_) {  // translated text lines, by line number
        const std::size_t slash = path.find_last_of('/');
        translation_->patchEnd(script_, slash == std::string_view::npos ? path : path.substr(slash + 1));
    }
    pc_ = 0;
    longDelay_ = 8;
    wait_ = 0;
    return true;
}

bool Ending::start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui, const Setup& setup) {
    (void)gui;
    vfs_ = &vfs;
    textures_ = &textures;
    translation_ = world.translation;
    shotType_ = setup.shotType;
    AnmManager& anm = *world.anm;
    for (int slot : kGuiSlotsToRelease) anm.releaseAnm(slot);
    for (int i = 0; i < 3; ++i) {
        std::string err;
        if (!anm.loadAnm(vfs, kStaffSlots[i], kStaffFiles[i], kStaffOffsets[i], &err)) {
            log::warn("Ending: {} not loaded ({})", kStaffFiles[i], err);
        }
    }
    vms_ = {};
    active_ = {};
    pending_.clear();
    half_ = line_ = 0;
    textColor_ = 0;
    shortDelay_ = 0;
    waitMin_ = clear_ = clearMin_ = 0;
    bgY_ = scroll_ = 0.0f;
    haveBackground_ = false;
    fade_ = Fade::None;
    fadeT_ = fadeN_ = fadeAlpha_ = 0;
    seen_ = setup.seen;
    prevShoot_ = false;
    steps_ = 0;
    done_ = !loadScript(scriptFor(setup.shotType, setup.difficulty, setup.continued));
    return !done_;
}

int Ending::readInt() {
    // Decimal, optional blanks and sign, terminated by NUL bytes.
    while (pc_ < script_.size() && (script_[pc_] == ' ' || script_[pc_] == '\t')) ++pc_;
    bool negative = false;
    if (pc_ < script_.size() && (script_[pc_] == '-' || script_[pc_] == '+')) negative = script_[pc_++] == '-';
    long v = 0;
    while (pc_ < script_.size() && script_[pc_] >= '0' && script_[pc_] <= '9') {
        v = std::min<long>(v * 10 + (script_[pc_++] - '0'), 1'000'000'000L);
    }
    while (pc_ < script_.size() && script_[pc_] != 0 && !(script_[pc_] == '\r' || script_[pc_] == '\n')) ++pc_;
    while (pc_ < script_.size() && script_[pc_] == 0) ++pc_;
    return static_cast<int>(negative ? -v : v);
}

std::string Ending::readPath() {
    std::string p;
    while (pc_ < script_.size() && script_[pc_] != 0 && script_[pc_] != '\r' && script_[pc_] != '\n') {
        p.push_back(static_cast<char>(script_[pc_++]));
    }
    return p;
}

void Ending::skipLine() {
    while (pc_ < script_.size() && script_[pc_] != '\r' && script_[pc_] != '\n') ++pc_;
    while (pc_ < script_.size() && (script_[pc_] == '\r' || script_[pc_] == '\n')) ++pc_;
}

void Ending::wipe() {
    vms_ = {};
    active_ = {};
    line_ = 0;
}

void Ending::renderHalf(World& world, Gui& gui, int half) {
    const int j = 2 * line_ + half;
    if (j < 0 || j >= kVms) {  // a 9th line has no slot (the original wrote past its array)
        pending_.clear();
        return;
    }
    AnmVm& vm = vms_[static_cast<std::size_t>(j)];
    world.anm->setAndExecuteScript(vm, kTextScriptFirst + j);  // keeps the position
    active_[static_cast<std::size_t>(j)] = true;
    gui.renderText(world, vm, pending_, 15, colorRefToRgb(textColor_), Gui::Align::Left, 0xd0d0c0u);
    pending_.clear();
}

// Phase 3: runs commands until something yields. False: the ending ends.
bool Ending::parse(World& world, Gui& gui, bool held) {
    while (true) {
        if (pc_ >= script_.size()) return false;  // no @z: treat the end as one
        const std::uint8_t b = script_[pc_];
        if (b == '@') {
            const std::uint8_t cmd = pc_ + 1 < script_.size() ? script_[pc_ + 1] : 0;
            pc_ += 2;
            switch (cmd) {
                case '0': case '1': case '2': case '3': {
                    fadeN_ = readInt();
                    fadeT_ = 0;
                    fade_ = cmd == '0' ? Fade::InBlack : cmd == '1' ? Fade::OutBlack : cmd == '2' ? Fade::InWhite : Fade::OutWhite;
                    break;
                }
                case 'a': {
                    const int v = readInt(), s = readInt(), p = readInt();
                    if (v >= 0 && v < kVms) {
                        AnmVm& vm = vms_[static_cast<std::size_t>(v)];
                        vm = AnmVm{};
                        world.anm->setAndExecuteScript(vm, kStaffBase + s);
                        if (world.anm->sprite(kStaffBase + p)) world.anm->setActiveSprite(vm, kStaffBase + p);
                        active_[static_cast<std::size_t>(v)] = true;
                    }
                    break;
                }
                case 'b': {
                    const std::string path = readPath();
                    std::string err;
                    auto bytes = vfs_->read(path);
                    auto img = bytes ? decodeImage(*bytes, &err) : std::nullopt;
                    if (!img) {
                        log::warn("Ending: {} cannot be loaded", path);
                        return false;
                    }
                    bgWidth_ = static_cast<float>(std::min(img->width, 640));
                    bgHeight_ = static_cast<float>(std::min(img->height, 1024));
                    auto shared = std::make_shared<Image>(std::move(*img));
                    background_ = textures_->add("ending|" + path, shared->width, shared->height,
                                                 [shared] { return std::optional<Image>(*shared); });
                    haveBackground_ = true;
                    break;
                }
                case 'c': textColor_ = static_cast<std::uint32_t>(readInt()); break;
                case 'm': {
                    const std::string path = readPath();
                    if (world.music) world.music->play(path);
                    break;
                }
                case 'M': {
                    const int seconds = readInt();
                    if (world.music) world.music->fadeOut(static_cast<float>(seconds));
                    break;
                }
                case 'r': {
                    clear_ = readInt();
                    clearMin_ = readInt();
                    skipLine();
                    return true;
                }
                case 's':
                    longDelay_ = readInt();
                    shortDelay_ = readInt();
                    break;
                case 'v': bgY_ = static_cast<float>(readInt()); break;
                case 'V': {
                    const int d = readInt(), t = readInt();
                    scroll_ = t != 0 ? static_cast<float>(d) / static_cast<float>(t) : 0.0f;
                    break;
                }
                case 'w': {
                    wait_ = readInt();
                    waitMin_ = readInt();
                    skipLine();
                    return true;
                }
                case 'z':
                    return false;
                case 'F': {
                    const std::string path = readPath();
                    if (!loadScript(path)) return false;
                    seen_ = true;  // the staff roll can always be fast-forwarded
                    for (bool& a : active_) a = false;  // as @R
                    skipLine();  // the original skips the chained file's first line
                    continue;
                }
                case 'R':
                    for (bool& a : active_) a = false;
                    break;
                default:
                    break;
            }
            skipLine();
            continue;
        }
        if (lineEnd(b)) {
            if (!pending_.empty()) renderHalf(world, gui, half_);
            half_ = 0;
            while (pc_ < script_.size() && lineEnd(script_[pc_])) ++pc_;
            const int d = held ? shortDelay_ : longDelay_;
            wait_ = waitMin_ = d;
            ++line_;
            return true;
        }
        // A translated line (UTF-8): all of it, across the line's two halves.
        if (b == kUtf8TextMarker && pending_.empty() && half_ == 0) {
            std::vector<std::uint8_t> line;
            while (pc_ < script_.size() && !lineEnd(script_[pc_])) line.push_back(script_[pc_++]);
            const int j = 2 * line_;
            if (j + 1 < kVms) {
                AnmVm& a = vms_[static_cast<std::size_t>(j)];
                AnmVm& c = vms_[static_cast<std::size_t>(j + 1)];
                world.anm->setAndExecuteScript(a, kTextScriptFirst + j);
                world.anm->setAndExecuteScript(c, kTextScriptFirst + j + 1);
                active_[static_cast<std::size_t>(j)] = active_[static_cast<std::size_t>(j + 1)] = true;
                gui.renderTextAcross(world, a, c, line, 15, colorRefToRgb(textColor_), 0xd0d0c0u);
            }
            continue;
        }
        // Text, one full-width character (2 bytes) at a time.
        pending_.push_back(b);
        if (pc_ + 1 < script_.size()) pending_.push_back(script_[pc_ + 1]);
        pc_ += 2;
        if (pending_.size() >= 32) {
            const bool second = half_ == 1;
            renderHalf(world, gui, half_);
            half_ = 1;
            // 64 bytes: both halves are shown; the line end (delay, next line) comes next step.
            if (second) return true;
        }
    }
}

bool Ending::step(World& world, Gui& gui, bool pressed, bool held, bool skip) {
    ++steps_;
    const bool advance = pressed || (seen_ && skip);
    bool parseNow = true;
    if (clear_ > 0) {
        --clear_;
        if (clearMin_ != 0) --clearMin_;
        else if (advance) clear_ = 0;
        if (clear_ <= 0) wipe();
        else parseNow = false;
    }
    if (parseNow && wait_ > 0) {
        --wait_;
        if (waitMin_ != 0) --waitMin_;
        else if (advance) wait_ = 0;
        parseNow = false;
    }
    if (parseNow && !parse(world, gui, held)) return false;
    bgY_ -= scroll_;
    if (bgY_ <= 0.0f) {
        bgY_ = 0.0f;
        scroll_ = 0.0f;
    }
    return true;
}

void Ending::advanceFade() {
    fadeAlpha_ = 0;
    if (fade_ == Fade::None) return;
    const bool in = fade_ == Fade::InBlack || fade_ == Fade::InWhite;
    fadeWhite_ = fade_ == Fade::InWhite || fade_ == Fade::OutWhite;
    if (fadeT_ >= fadeN_) {
        if (in) fade_ = Fade::None;
        else fadeAlpha_ = 255;
        return;
    }
    const int a = fadeN_ > 0 ? 255 * fadeT_ / fadeN_ : 255;
    fadeAlpha_ = in ? 255 - a : a;
    ++fadeT_;
}

bool Ending::update(World& world, Gui& gui, const InputState& in) {
    if (done_) return true;
    const bool pressed = in.shoot && !prevShoot_;
    prevShoot_ = in.shoot;
    for (int extra = 0; extra < 5; ++extra) {
        if (!step(world, gui, pressed, in.shoot, in.skip)) {
            done_ = true;
            AnmManager& anm = *world.anm;
            for (int slot : kStaffSlots) anm.releaseAnm(slot);
            return true;
        }
        for (int i = 0; i < kVms; ++i) {
            if (active_[static_cast<std::size_t>(i)]) world.anm->executeScript(vms_[static_cast<std::size_t>(i)]);
        }
        if (!(seen_ && in.skip)) break;  // fast-forward: up to 5 steps a frame
    }
    advanceFade();
    return false;
}

void Ending::draw(World& world, SpriteSink& sink, const SpriteRegion& white) {
    sink.fillRect(white, 0.0f, 0.0f, 640.0f, 480.0f, rgba(0, 0, 0));
    if (haveBackground_) {
        // A 640 x 480 window of the picture from row trunc(y), clamped to the picture.
        const float th = static_cast<float>(textures_->height(background_)), tw = static_cast<float>(textures_->width(background_));
        const float row = std::clamp(std::trunc(bgY_), 0.0f, std::max(0.0f, bgHeight_ - 480.0f));
        const float h = std::min(480.0f, bgHeight_ - row), w = std::min(640.0f, bgWidth_);
        SpriteInstance s;
        s.x = w * 0.5f;
        s.y = h * 0.5f;
        s.w = w;
        s.h = h;
        s.u0 = 0.0f;
        s.u1 = w / tw;
        s.v0 = row / th;
        s.v1 = (row + h) / th;
        sink.draw(background_, Blend::Alpha, s);
    }
    for (int i = 0; i < kVms; ++i) {
        if (active_[static_cast<std::size_t>(i)]) world.anm->drawScreen(sink, vms_[static_cast<std::size_t>(i)]);
    }
    if (fadeAlpha_ != 0) {
        const auto a = static_cast<std::uint8_t>(fadeAlpha_);
        sink.fillRect(white, 0.0f, 0.0f, 640.0f, 480.0f, fadeWhite_ ? rgba(255, 255, 255, a) : rgba(0, 0, 0, a));
    }
}

}  // namespace th06
