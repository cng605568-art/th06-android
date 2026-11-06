#pragma once

// The 2002 game's Music Room (the title's "Music Room" item): music.jpg behind one
// animation from music00.anm, the track list from data/musiccmt.txt with each title
// rendered into music01 / music02's sprites (10 rows at a time, scrolling with the cursor,
// numbered, an arrow on the cursor row), and the chosen track's comment in 16 text.anm
// pieces (7 lines, each split at byte 32). Shoot plays the track (it keeps playing after
// leaving until the title restarts its theme), Bomb / Esc / Back go back at once.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "input/InputState.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/SpriteSink.hpp"

namespace touhou {
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th06 {

class Gui;
struct World;

class MusicRoom {
  public:
    static constexpr int kMaxTracks = 32, kRows = 10, kLines = 8, kPieces = 16;

    struct Track {
        std::string path;   // e.g. "bgm/th06_02.mid"
        std::string title;  // Shift-JIS
        std::array<std::string, kLines> lines;  // line 0 is always empty, as in the original
    };

    // musiccmt.txt: "@path", the title, then up to 7 comment lines per track (a line starting
    // with '@' starts the next track; text before the first '@' is ignored; blank lines vanish).
    static std::vector<Track> parse(std::span<const std::uint8_t> text);

    // Loads the files and renders the titles and track 1's comment. False if a file is missing
    // or the list is empty.
    bool start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui);
    // One pass. True when the player leaves (the music is left playing).
    bool update(World& world, Gui& gui, const InputState& in);
    void draw(World& world, Gui& gui, SpriteSink& sink);
    void release(World& world);

    // Inspection (tests).
    int trackCount() const { return static_cast<int>(tracks_.size()); }
    const Track& track(int i) const { return tracks_[static_cast<std::size_t>(i)]; }
    int cursor() const { return cursor_; }
    int windowStart() const { return window_; }
    int selected() const { return selected_; }
    bool inputOpen() const { return inputOpen_; }
    const AnmVm& piece(int j) const { return pieces_[static_cast<std::size_t>(j)]; }
    const AnmVm& titleHolder(int i) const { return titles_[static_cast<std::size_t>(i)]; }

  private:
    void showComment(World& world, Gui& gui, int track);

    std::vector<Track> tracks_;
    AnmVm deco_;
    std::array<AnmVm, kMaxTracks> titles_{};
    std::array<AnmVm, kPieces> pieces_{};
    int cursor_ = 0, window_ = 0, selected_ = 0;
    int wait_ = 0;
    bool inputOpen_ = false;
    std::uint32_t prevButtons_ = 0;
    std::uint16_t background_ = 0;
    bool haveBackground_ = false;
    float bgWidth_ = 640.0f, bgHeight_ = 480.0f;
};

}  // namespace touhou::th06
