#include "game/MusicRoom.hpp"

#include <algorithm>
#include <format>
#include <memory>

#include "audio/MusicPlayer.hpp"
#include "core/Log.hpp"
#include "game/Gui.hpp"
#include "game/World.hpp"
#include "gfx/Image.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

constexpr int kSlotDeco = 41, kSlotTitles = 42, kSlotTitles2 = 43;
constexpr int kOffsetDeco = 0x100, kOffsetTitles = 0x101, kOffsetTitles2 = 0x111;
constexpr int kTextSprites = 0x708;  // text.anm sprites 8..23
// Rendered text colours as they appear on screen.
constexpr std::uint32_t kTitleRgb = 0xffe0c0, kTitleShadow = 0x802030;
constexpr std::uint32_t kCommentRgb = 0xc0e0ff, kCommentShadow = 0x000030;
constexpr std::size_t kPathMax = 63, kTitleMax = 33, kLineMax = 64;

constexpr std::uint32_t kUp = 1, kDown = 2, kShoot = 4, kBomb = 8, kEsc = 16;

bool lineBreak(std::uint8_t c) { return c == '\r' || c == '\n'; }

std::span<const std::uint8_t> bytesOf(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace

std::vector<MusicRoom::Track> MusicRoom::parse(std::span<const std::uint8_t> b) {
    std::vector<Track> tracks;
    std::size_t p = 0;
    const std::size_t n = b.size();
    auto readLine = [&](std::string& out, std::size_t cap) {
        while (p < n && !lineBreak(b[p])) {
            if (out.size() < cap) out.push_back(static_cast<char>(b[p]));
            ++p;
        }
    };
    auto skipBreaks = [&] {
        while (p < n && lineBreak(b[p])) ++p;
    };
    while (p < n) {
        if (b[p] != '@') {  // outside a track everything but '@' is ignored
            ++p;
            continue;
        }
        ++p;
        bool another = true;
        while (another) {
            another = false;
            if (static_cast<int>(tracks.size()) >= kMaxTracks) return tracks;
            Track t;
            readLine(t.path, kPathMax);
            skipBreaks();
            readLine(t.title, kTitleMax);
            skipBreaks();  // (comment line 0 stays empty)
            for (int k = 1; k < kLines && p < n; ++k) {
                if (b[p] == '@') {  // the comment ends; this '@' starts the next track
                    ++p;
                    another = true;
                    break;
                }
                readLine(t.lines[static_cast<std::size_t>(k)], kLineMax);
                skipBreaks();
            }
            tracks.push_back(std::move(t));
        }
    }
    return tracks;
}

bool MusicRoom::start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui) {
    AnmManager& anm = *world.anm;
    haveBackground_ = false;
    if (auto jpg = vfs.read("data/result/music.jpg")) {
        std::string err;
        if (auto img = decodeImage(*jpg, &err)) {
            bgWidth_ = static_cast<float>(img->width);
            bgHeight_ = static_cast<float>(img->height);
            auto shared = std::make_shared<Image>(std::move(*img));
            background_ = textures.add("data/result/music.jpg", shared->width, shared->height,
                                       [shared] { return std::optional<Image>(*shared); });
            haveBackground_ = true;
        }
    }
    struct File {
        int slot;
        const char* path;
        int offset;
    };
    for (const File& f : {File{kSlotDeco, "data/music00.anm", kOffsetDeco}, File{kSlotTitles, "data/music01.anm", kOffsetTitles},
                          File{kSlotTitles2, "data/music02.anm", kOffsetTitles2}}) {
        std::string err;
        anm.releaseAnm(f.slot);
        if (!anm.loadAnm(vfs, f.slot, f.path, f.offset, &err)) {
            log::warn("Music room: {} not loaded ({})", f.path, err);
            release(world);
            return false;
        }
    }
    deco_ = AnmVm{};
    anm.setAndExecuteScript(deco_, kOffsetDeco);
    const auto text = vfs.read("data/musiccmt.txt");
    tracks_ = text ? parse(*text) : std::vector<Track>{};
    // One title sprite per track: a list longer than music01 + music02 provide stops there.
    std::size_t usable = 0;
    while (usable < tracks_.size() && anm.sprite(kOffsetTitles + static_cast<int>(usable))) ++usable;
    tracks_.resize(usable);
    if (tracks_.empty()) {
        log::warn("Music room: no tracks in data/musiccmt.txt");
        release(world);
        return false;
    }
    for (AnmVm& vm : titles_) vm = AnmVm{};
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        AnmVm& vm = titles_[i];
        anm.initializeAndSetSprite(vm, kOffsetTitles + static_cast<int>(i));
        gui.renderText(world, vm, bytesOf(tracks_[i].title), 15, kTitleRgb, Gui::Align::Left, kTitleShadow);
        vm.anchor = AnmAnchor::TopLeft;
    }
    for (int j = 0; j < kPieces; ++j) {
        AnmVm& vm = pieces_[static_cast<std::size_t>(j)];
        vm = AnmVm{};
        anm.initializeAndSetSprite(vm, kTextSprites + j);
        vm.anchor = AnmAnchor::TopLeft;
    }
    showComment(world, gui, 0);  // the title theme's comment, while it plays
    cursor_ = window_ = selected_ = 0;
    wait_ = 0;
    inputOpen_ = false;
    prevButtons_ = 0;
    return true;
}

void MusicRoom::release(World& world) {
    for (const int slot : {kSlotDeco, kSlotTitles, kSlotTitles2}) world.anm->releaseAnm(slot);
    deco_ = AnmVm{};
    for (AnmVm& vm : titles_) vm = AnmVm{};
    haveBackground_ = false;
}

// Line k of the comment in two pieces: bytes 0-31 on the left, 32-63 on the right (only
// when the line is longer than 32 bytes). An empty piece is hidden.
void MusicRoom::showComment(World& world, Gui& gui, int track) {
    const Track& t = tracks_[static_cast<std::size_t>(track)];
    for (int j = 0; j < kPieces; ++j) {
        AnmVm& vm = pieces_[static_cast<std::size_t>(j)];
        const int k = j / 2;
        const std::string& line = t.lines[static_cast<std::size_t>(k)];
        std::string half;
        if (j % 2 == 0) half = line.substr(0, std::min<std::size_t>(32, line.size()));
        else if (line.size() > 32) half = line.substr(32, 32);
        if (!half.empty()) gui.renderText(world, vm, bytesOf(half), 15, kCommentRgb, Gui::Align::Left, kCommentShadow);
        vm.visible = !half.empty();
        vm.pos = {j % 2 == 0 ? 96.0f : 344.0f, 320.0f + 16.0f * static_cast<float>(k), 0.0f};
    }
}

bool MusicRoom::update(World& world, Gui& gui, const InputState& in) {
    const std::uint32_t bits = (in.up ? kUp : 0) | (in.down ? kDown : 0) | (in.shoot ? kShoot : 0) | (in.bomb ? kBomb : 0) |
                               (in.pause ? kEsc : 0);
    auto pressed = [&](std::uint32_t m) { return (bits & m) != 0 && (bits & m) != (prevButtons_ & m); };
    const bool up = pressed(kUp), down = pressed(kDown), confirm = pressed(kShoot), cancel = pressed(kBomb | kEsc);
    prevButtons_ = bits;
    const int n = trackCount();
    if (!inputOpen_) {  // input opens on the 9th pass and is first read on the 10th
        if (wait_ >= 8) {
            inputOpen_ = true;
            wait_ = 0;
        } else {
            ++wait_;
        }
    } else {
        // Press only, no repeat and no sounds. The window follows the cursor a row at a time
        // and jumps when the cursor wraps.
        if (up) {
            if (cursor_ > 0) {
                --cursor_;
                if (window_ > cursor_) window_ = cursor_;
            } else {
                cursor_ = n - 1;
                window_ = std::max(0, n - kRows);
            }
        }
        if (down) {
            if (cursor_ < n - 1) {
                ++cursor_;
                if (cursor_ >= window_ + kRows) window_ = cursor_ - (kRows - 1);
            } else {
                cursor_ = 0;
                window_ = 0;
            }
        }
        if (confirm) {
            selected_ = cursor_;
            if (world.music) world.music->play(tracks_[static_cast<std::size_t>(selected_)].path);
            showComment(world, gui, selected_);
        }
        if (cancel) return true;
    }
    world.anm->executeScript(deco_);
    return false;
}

void MusicRoom::draw(World& world, Gui& gui, SpriteSink& sink) {
    if (haveBackground_) {
        SpriteInstance s;
        s.x = bgWidth_ * 0.5f;
        s.y = bgHeight_ * 0.5f;
        s.w = bgWidth_;
        s.h = bgHeight_;
        sink.draw(background_, Blend::Alpha, s);
    }
    {
        AnmVm vm = deco_;  // without its offset or rotation
        vm.rotation.z = 0.0f;
        world.anm->drawScreen(sink, vm);
    }
    const int n = trackCount();
    for (int r = 0; r < kRows; ++r) {
        const int i = window_ + r;
        const float y = 102.0f + 18.0f * static_cast<float>(r);
        const std::uint32_t color = i == cursor_ ? 0xffffffffu : 0xe0808080u;
        if (i < n) {
            AnmVm vm = titles_[static_cast<std::size_t>(i)];
            vm.pos = {93.0f, y, 0.0f};
            vm.color = color;
            world.anm->drawScreen(sink, vm);
        }
        if (i == cursor_) gui.queueString(33.0f, y, "\x7f", color);  // the arrow
        gui.queueString(48.0f, y, std::format("{:2d}.", i + 1), color);
    }
    for (const AnmVm& vm : pieces_) world.anm->drawScreen(sink, vm);
    gui.drawStrings(world, sink);
}

}  // namespace th06
