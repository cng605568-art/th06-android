#include "th08/MusicRoom.hpp"

#include "th08/English.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bgm.hpp"

namespace touhou::th08 {

namespace {
// text.anm's eight comment-line scripts: at (64, 320 + 16 k) on sprites 21 + k, interrupt 1
// fades a line in (staggered by k), 2 out [DATA; the scripts the music-room slots match].
constexpr int kLineScript = 10;
// GDI colours (0x00BBGGRR): titles, locked titles, comment lines (spec_anm_text §6.4).
constexpr std::uint32_t kTitleColor = 0xC0E0FFu, kLockedColor = 0x80A0C0u, kLineColor = 0xFFE0C0u;
// "？？？？？？？？" in Shift-JIS (eight full-width question marks).
constexpr char kLockedTitle[] = "\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48";
bool eol(std::uint8_t c) { return c == '\r' || c == '\n'; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    return out;
}
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
    while (i < n && tracks.size() < static_cast<std::size_t>(MusicRoom::kTracks)) {
        if (b[i] != '@') {
            ++i;
            continue;
        }
        ++i;
        MusicTrack t;
        t.path = readLine(63);
        skipBreaks();
        t.title = readLine(65);
        // The break after the title is never skipped, so comment line 0 is always empty
        // [QUIRK, as TH07].
        for (std::size_t l = 0; l < t.lines.size(); ++l) {
            if (i < n && b[i] == '@') break;
            t.lines[l] = readLine(65);
            skipBreaks();
        }
        tracks.push_back(std::move(t));
    }
    return tracks;
}

int musicTrackIndex(const std::vector<MusicTrack>& tracks, std::string_view path) {
    const std::string want = lower(path);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (lower(tracks[i].path) == want) return static_cast<int>(i);
    }
    return -1;
}

MusicRoom::~MusicRoom() { release(); }

void MusicRoom::release() {
    if (loaded_ && ctx_.anm) ctx_.anm->release(kSlotMusic);
    loaded_ = false;
    background_.release();
}

bool MusicRoom::unlocked(int track) const {
    if (track < 0 || track >= static_cast<int>(tracks_.size())) return false;
    return ctx_.musicHeard && track < static_cast<int>(ctx_.musicHeard->size()) &&
           (*ctx_.musicHeard)[static_cast<std::size_t>(track)] != 0;
}

bool MusicRoom::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures || !ctx_.rng) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx_.anm;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, "result/music.jpg")) log::warn("result/music.jpg could not be loaded");
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "music00.anm", kSlotMusic, error, true)) return false;
    loaded_ = true;
    if (const auto txt = ctx_.vfs->read("musiccmt.txt")) tracks_ = parseMusicComments(*txt);
    else log::warn("musiccmt.txt is missing: an empty Music Room");
    if (tracks_.size() > static_cast<std::size_t>(kTitles)) tracks_.resize(kTitles);  // one title VM each
    if (ctx_.english) {
        // The English text mode (thcrap's music_title and music_cmt): the title from themes.js
        // with the patch's numbered-title format, the comment from musiccmt.js; its first entry
        // goes to the line after the game's always empty line 0 ("@" is the numbered title).
        const std::string numbered = ctx_.english->stringDef("Music Room Numbered Title").value_or("No.%2d %s");
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            MusicTrack& t = tracks_[i];
            const int track = static_cast<int>(i) + 1;
            if (const auto title = ctx_.english->musicTitle(track)) t.title = English::gameText(formatPatchString(numbered, track, *title));
            const auto comment = ctx_.english->musicComment(track);
            if (!comment) continue;
            for (std::size_t l = 1; l < t.lines.size(); ++l) {
                if (l - 1 >= comment->size()) {
                    t.lines[l].clear();
                } else if ((*comment)[l - 1]) {
                    t.lines[l] = (*comment)[l - 1]->empty() ? std::string() : English::gameText(*(*comment)[l - 1]);
                }
            }
        }
    }
    deco_ = AnmVm{};
    deco_.startFresh(lib, *ctx_.rng, kSlotMusic, 0);
    const AnmFile* file = lib.file(kSlotMusic);
    for (int i = 0; i < kTitles; ++i) {
        const auto k = static_cast<std::size_t>(i);
        AnmVm& t = titles_[k];
        t = AnmVm{};
        hasTitle_[k] = file && file->script(i + 1) != nullptr;
        if (!hasTitle_[k]) continue;
        t.startFresh(lib, *ctx_.rng, kSlotMusic, i + 1);
        t.set(AnmVm::AnchorLeft, true);
        t.set(AnmVm::AnchorTop, true);
        if (i >= static_cast<int>(tracks_.size())) continue;
        if (unlocked(i)) renderSpriteText(ctx_.textures, ctx_.text, t, tracks_[k].title, kTitleColor);
        else renderSpriteText(ctx_.textures, ctx_.text, t, kLockedTitle, kLockedColor);
    }
    for (int i = 0; i < kLines; ++i) {
        AnmVm& d = lines_[static_cast<std::size_t>(i)];
        d = AnmVm{};
        d.startFresh(lib, *ctx_.rng, kSlotText, kLineScript + i);
    }
    renderComment(unlocked(0) ? 0 : -1);
    cursor_ = top_ = 0;
    selected_ = -1;
    timer_ = 0;
    done_ = false;
    return true;
}

void MusicRoom::renderComment(int track) {
    for (int i = 0; i < kLines; ++i) {
        AnmVm& d = lines_[static_cast<std::size_t>(i)];
        std::string line = track >= 0 && track < static_cast<int>(tracks_.size())
                               ? tracks_[static_cast<std::size_t>(track)].lines[static_cast<std::size_t>(i)]
                               : std::string();
        const bool translated = !line.empty() && static_cast<std::uint8_t>(line[0]) == kUtf8TextMarker;
        if (!translated && line.size() > 64) line.resize(64);  // each line limited to 64 bytes (TH07 §8.3)
        lineActive_[static_cast<std::size_t>(i)] = !line.empty();
        if (!line.empty()) renderSpriteText(ctx_.textures, ctx_.text, d, line, kLineColor);
    }
}

void MusicRoom::resendTitles() {
    for (int i = 0; i < kTitles; ++i) {
        if (hasTitle_[static_cast<std::size_t>(i)]) titles_[static_cast<std::size_t>(i)].interrupt(i == cursor_ ? 1 : 2);
    }
}

void MusicRoom::update(const MenuInput& in) {
    if (done_) return;
    ZunRng& rng = *ctx_.rng;
    const int count = static_cast<int>(tracks_.size());
    if (timer_ == 0) {
        resendTitles();
        for (AnmVm& d : lines_) d.interrupt(1);
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
        if (in.pressed(kSelect) && count > 0 && unlocked(cursor_)) {
            // A locked track cannot be played (spec §4.9 [GAP]: TH07's behaviour assumed).
            selected_ = cursor_;
            const std::string& path = tracks_[static_cast<std::size_t>(cursor_)].path;
            if (ctx_.music) ctx_.music->play(path);
            renderComment(cursor_);
            for (AnmVm& d : lines_) d.interrupt(1);
            log::info("th08: Music Room plays {}", path);
        } else if (in.pressed(kCancel)) {
            ctx_.playSound(kSndBack);
            if (ctx_.request) ctx_.request(Scene::MainMenu);
            done_ = true;
            return;
        }
    }
    deco_.tick(rng);
    for (int i = 0; i < kTitles; ++i) {
        if (hasTitle_[static_cast<std::size_t>(i)]) titles_[static_cast<std::size_t>(i)].tick(rng);
    }
    for (AnmVm& d : lines_) d.tick(rng);
    ++timer_;
}

void MusicRoom::draw(SpriteSink& sink) {
    if (done_) return;
    background_.blit(sink, 0, 0);
    const AnmDrawState none;
    drawVm(sink, deco_, none);
    const int count = static_cast<int>(tracks_.size());
    for (int i = top_; i < top_ + kRows && i < count; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const float x = 93.0f, y = static_cast<float>((i + 1 - top_) * 18 + 84);
        if (hasTitle_[k]) {
            AnmVm vm = titles_[k];
            vm.position = {x, y, 0.0f};
            drawVm(sink, vm, none);
        }
        const std::uint32_t color = i == cursor_ ? 0xFFFFFFFFu : 0xFFA0A0C0u;
        text_.add(x - 45.0f, y, std::format("{:02d}.", i + 1), color);
        if (i == cursor_) text_.add(x - 60.0f, y, std::string(1, static_cast<char>(127)), 0xFFFFFFFFu);
    }
    for (int i = 0; i < kLines; ++i) {
        if (lineActive_[static_cast<std::size_t>(i)]) drawVm(sink, lines_[static_cast<std::size_t>(i)], none);
    }
    text_.draw(sink, *ctx_.anm);
}

}  // namespace touhou::th08
