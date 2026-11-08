#include "th07/MusicRoom.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th07/Bgm.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {
constexpr int kMusicScripts = 0x900;
constexpr int kDescScript = 0x707;
constexpr std::uint32_t kTitleColor = 0xC0E0FFu, kDescColor = 0xFFE0C0u;  // GDI colours
bool eol(std::uint8_t c) { return c == '\r' || c == '\n'; }
}  // namespace

std::vector<MusicTrack> parseMusicComments(std::span<const std::uint8_t> b) {
    std::vector<MusicTrack> tracks;
    std::size_t i = 0;
    const std::size_t n = b.size();
    auto readLine = [&](std::size_t cap) {
        std::string s;
        while (i < n && !eol(b[i])) {
            if (s.size() < cap) s.push_back(static_cast<char>(b[i]));
            ++i;
        }
        return s;
    };
    auto skipBreaks = [&]() {
        while (i < n && eol(b[i])) ++i;
    };
    while (i < n && tracks.size() < static_cast<std::size_t>(MusicRoom::kTitles)) {
        if (b[i] != '@') {
            ++i;
            continue;
        }
        ++i;
        MusicTrack t;
        t.path = readLine(63);
        skipBreaks();
        t.title = readLine(65);
        // The break after the title is never skipped (the loop wants a byte that is both CR
        // and LF), so comment line 0 is always empty [QUIRK].
        for (std::size_t l = 0; l < t.lines.size(); ++l) {
            if (i < n && b[i] == '@') break;
            t.lines[l] = readLine(65);
            skipBreaks();
        }
        tracks.push_back(std::move(t));
    }
    return tracks;
}

MusicRoom::~MusicRoom() { release(); }

void MusicRoom::release() {
    if (loaded_) ctx_.anm->release(46);
    loaded_ = false;
    background_.release();
}

bool MusicRoom::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx_.anm;
    background_.load(*ctx_.vfs, *ctx_.textures, "data/result/music.jpg");
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "data/music00.anm", 46, kMusicScripts, error)) return false;
    loaded_ = true;
    if (const auto txt = ctx_.vfs->read("data/musiccmt.txt")) tracks_ = parseMusicComments(*txt);
    else log::warn("data/musiccmt.txt is missing: an empty Music Room");
    deco_ = AnmVm{};
    deco_.startFresh(lib, *ctx_.rng, kMusicScripts);
    for (int i = 0; i < kTitles; ++i) {
        AnmVm& t = titles_[static_cast<std::size_t>(i)];
        t = AnmVm{};
        if (!lib.script(kMusicScripts + 1 + i)) continue;
        t.startFresh(lib, *ctx_.rng, kMusicScripts + 1 + i);
        t.flags |= AnmVm::AnchorLeft | AnmVm::AnchorTop;
        if (i < static_cast<int>(tracks_.size())) renderSpriteText(ctx_.textures, ctx_.text, t, tracks_[static_cast<std::size_t>(i)].title, kTitleColor);
    }
    for (int i = 0; i < 8; ++i) {
        AnmVm& d = desc_[static_cast<std::size_t>(i)];
        d = AnmVm{};
        d.startFresh(lib, *ctx_.rng, kDescScript + i);
    }
    renderDescription(0);
    timer_ = 0;
    return true;
}

void MusicRoom::renderDescription(int track) {
    for (int i = 0; i < 8; ++i) {
        AnmVm& d = desc_[static_cast<std::size_t>(i)];
        const std::string line = track >= 0 && track < static_cast<int>(tracks_.size())
                                     ? tracks_[static_cast<std::size_t>(track)].lines[static_cast<std::size_t>(i)].substr(0, 64)
                                     : std::string();
        descActive_[static_cast<std::size_t>(i)] = !line.empty();
        if (!line.empty()) renderSpriteText(ctx_.textures, ctx_.text, d, line, kDescColor);
    }
}

void MusicRoom::resendTitles() {
    for (int i = 0; i < kTitles; ++i) titles_[static_cast<std::size_t>(i)].interrupt(i == cursor_ ? 1 : 2);
}

void MusicRoom::update(const MenuInput& in) {
    if (done_) return;
    AnmLibrary& lib = *ctx_.anm;
    const int count = static_cast<int>(tracks_.size());
    if (timer_ == 0) {
        resendTitles();
        for (AnmVm& d : desc_) d.interrupt(1);
    }
    if (timer_ >= 8) {
        bool moved = false;
        if (in.pressed(BtnUp) && count > 0) {
            if (--cursor_ < 0) {
                cursor_ = count - 1;
                top_ = std::max(0, count - kRows);
            } else if (top_ > cursor_) {
                top_ = cursor_;
            }
            moved = true;
        } else if (in.pressed(BtnDown) && count > 0) {
            if (++cursor_ >= count) {
                cursor_ = 0;
                top_ = 0;
            } else if (top_ <= cursor_ - kRows) {
                top_ = cursor_ - (kRows - 1);
            }
            moved = true;
        }
        if (moved) {
            ctx_.playSound(kSndMove);
            resendTitles();
        }
        if (in.pressed(BtnShoot | BtnEnter) && count > 0) {
            selected_ = cursor_;
            if (ctx_.music) ctx_.music->play(tracks_[static_cast<std::size_t>(cursor_)].path);
            renderDescription(cursor_);
            for (AnmVm& d : desc_) d.interrupt(1);
            log::info("th07: Music Room plays {}", tracks_[static_cast<std::size_t>(cursor_)].path);
        } else if (in.pressed(BtnBomb | BtnMenu)) {
            ctx_.playSound(kSndBack);
            ctx_.request(FlowState::MainMenu);
            done_ = true;
            return;
        }
    }
    deco_.tick(lib, *ctx_.rng);
    for (AnmVm& t : titles_) t.tick(lib, *ctx_.rng);
    for (AnmVm& d : desc_) d.tick(lib, *ctx_.rng);
    ++timer_;
}

void MusicRoom::draw(SpriteSink& sink) {
    if (done_) return;
    background_.blit(sink, 0, 0);
    const AnmDrawState none;
    drawVm(sink, deco_, none);
    const int count = std::min<int>(static_cast<int>(tracks_.size()), kTitles);
    for (int i = top_; i < top_ + kRows && i < count; ++i) {
        AnmVm vm = titles_[static_cast<std::size_t>(i)];
        const float x = 93.0f, y = static_cast<float>((i + 1 - top_) * 18 + 84);
        vm.position = {x, y, 0.0f};
        drawVm(sink, vm, none);
        const std::uint32_t color = i == cursor_ ? 0xFFFFFFFFu : 0xFFA0A0C0u;
        text_.add(x - 45.0f, y, std::format("{:02d}.", i + 1), color);
        if (i == cursor_) text_.add(x - 60.0f, y, std::string(1, static_cast<char>(127)), 0xFFFFFFFFu);
    }
    for (int i = 0; i < 8; ++i) {
        if (descActive_[static_cast<std::size_t>(i)]) drawVm(sink, desc_[static_cast<std::size_t>(i)], none);
    }
    text_.draw(sink, *ctx_.anm);
}

}  // namespace touhou::th07
