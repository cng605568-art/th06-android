#pragma once

// The Music Room (spec 8): every track of data/musiccmt.txt with its comment; Select plays
// it (from thbgm.dat), Return goes back to the title. music00.anm's instances carry the
// titles; the comment lines are rendered into text.anm sprites. Logic priority 3, draw
// order 0.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "th07/Anm.hpp"
#include "th07/Flow.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

struct MusicTrack {
    std::string path;   // e.g. "bgm/th07_01.mid"
    std::string title;  // Shift-JIS
    std::array<std::string, 8> lines;  // line 0 is always empty [QUIRK]
};

// musiccmt.txt (spec 8.2): at most 31 tracks (the screen's title instances).
std::vector<MusicTrack> parseMusicComments(std::span<const std::uint8_t> text);

class MusicRoom {
  public:
    static constexpr int kTitles = 31, kRows = 10;

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
    const std::vector<MusicTrack>& tracks() const { return tracks_; }

  private:
    void renderDescription(int track);
    void resendTitles();

    FlowContext& ctx_;
    std::vector<MusicTrack> tracks_;
    bool loaded_ = false, done_ = false;
    Surface background_;
    AnmVm deco_;
    std::array<AnmVm, kTitles> titles_{};
    std::array<AnmVm, 8> desc_{};
    std::array<bool, 8> descActive_{};
    int cursor_ = 0, top_ = 0, selected_ = -1;
    int timer_ = 0;
    AsciiText text_;
};

}  // namespace touhou::th07
