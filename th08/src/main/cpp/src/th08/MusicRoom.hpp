#pragma once

// Imperishable Night's Music Room (claude/th08-specs/spec_flow_menus.md §4.9, as TH07 §8):
// every track of musiccmt.txt with its comment over result/music.jpg; Select plays it, Return
// goes back to the title. music00.anm (slot 23) holds the deco script and the 30 title
// scripts; the comment lines are rendered into text.anm sprites. New in TH08: a track's
// title shows only once it has been heard (score.dat's PLST flags), else "？？？？？？？？".

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

struct MusicTrack {
    std::string path;   // e.g. "bgm/th08_01.mid"
    std::string title;  // Shift-JIS
    std::array<std::string, 8> lines;  // line 0 is always empty [QUIRK]
};

// musiccmt.txt (TH07 §8.2; up to 32 tracks).
std::vector<MusicTrack> parseMusicComments(std::span<const std::uint8_t> text);
// The track whose path is `path` (the music player's names, e.g. "bgm/th08_00.mid"), or -1.
int musicTrackIndex(const std::vector<MusicTrack>& tracks, std::string_view path);

class MusicRoom {
  public:
    static constexpr int kTracks = 32, kTitles = 30, kRows = 10, kLines = 8;
    static constexpr int kSlotMusic = 23;

    explicit MusicRoom(FlowContext& ctx) : ctx_(ctx) {}
    ~MusicRoom();

    bool create(std::string* error);
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    bool done() const { return done_; }
    void release();

    int cursor() const { return cursor_; }
    int top() const { return top_; }
    int selected() const { return selected_; }
    bool unlocked(int track) const;
    const std::vector<MusicTrack>& tracks() const { return tracks_; }

  private:
    void renderComment(int track);
    void resendTitles();

    FlowContext& ctx_;
    std::vector<MusicTrack> tracks_;
    bool loaded_ = false, done_ = false;
    Surface background_;
    AnmVm deco_;
    std::array<AnmVm, kTitles> titles_{};
    std::array<bool, kTitles> hasTitle_{};
    std::array<AnmVm, kLines> lines_{};
    std::array<bool, kLines> lineActive_{};
    int cursor_ = 0, top_ = 0, selected_ = -1;
    int timer_ = 0;
    AsciiText text_;
};

}  // namespace touhou::th08
