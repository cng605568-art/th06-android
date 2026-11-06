#pragma once

// The ending and staff roll of the 2002 game: a text script from the ED archive
// (end00.end / end01.end / end10.end / end11.end for a good ending per shot type,
// end00b.end / end10b.end for the bad ending per character) drives pictures (`@b`, with
// vertical scrolling), sprites from staff01..03.anm (`@a`), lines of Shift-JIS text in up
// to 16 text.anm slots, waits, screen clears, fades, music, and a chained script for the
// staff roll (`@F`). Shoot ends a wait once its minimum has passed; holding Ctrl (or the
// touch BOMB) fast-forwards at up to 5 steps per frame once the ending has been seen
// before (and always in the staff roll). `@z` ends it: on to the result screen.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "input/InputState.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/SpriteSink.hpp"

namespace touhou {
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th06 {

class Translation;

class Gui;
struct World;

class Ending {
  public:
    static constexpr int kVms = 16;

    struct Setup {
        int shotType = 0, difficulty = 1;
        bool continued = false;
        bool seen = false;  // cleared before (fast-forward allowed from the start)
    };

    // The script file for a clear: bad ending on Easy or after a continue.
    static std::string scriptFor(int shotType, int difficulty, bool continued);

    // Loads staff01..03.anm and the script. False if the script is missing.
    bool start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui, const Setup& setup);
    // One frame (logic pass). True once the ending is over.
    bool update(World& world, Gui& gui, const InputState& in);
    void draw(World& world, SpriteSink& sink, const SpriteRegion& white);

    bool done() const { return done_; }
    // Inspection (tests).
    int lineCounter() const { return line_; }
    int waitTimer() const { return wait_; }
    bool seen() const { return seen_; }
    float backgroundRow() const { return bgY_; }
    const AnmVm& vm(int i) const { return vms_[static_cast<std::size_t>(i)]; }
    bool vmActive(int i) const { return active_[static_cast<std::size_t>(i)]; }
    std::uint32_t textColor() const { return textColor_; }
    int fadeAlpha() const { return fadeAlpha_; }
    int steps() const { return steps_; }

  private:
    enum class Fade { None, InBlack, OutBlack, InWhite, OutWhite };
    // One script step; false when the ending ends.
    bool step(World& world, Gui& gui, bool pressed, bool held, bool skip);
    bool parse(World& world, Gui& gui, bool held);
    bool loadScript(std::string_view path);
    int readInt();
    std::string readPath();
    void skipLine();
    void renderHalf(World& world, Gui& gui, int half);
    void wipe();
    void advanceFade();

    const Vfs* vfs_ = nullptr;
    TextureLibrary* textures_ = nullptr;
    std::vector<std::uint8_t> script_;
    std::size_t pc_ = 0;
    const Translation* translation_ = nullptr;
    std::array<AnmVm, kVms> vms_{};
    std::array<bool, kVms> active_{};
    std::vector<std::uint8_t> pending_;  // text bytes since the last render
    int half_ = 0;                        // the line's first half is already shown
    int line_ = 0;
    std::uint32_t textColor_ = 0;  // as written by @c (0xBBGGRR)
    int longDelay_ = 8, shortDelay_ = 0;
    int wait_ = 0, waitMin_ = 0, clear_ = 0, clearMin_ = 0;
    float bgY_ = 0.0f, scroll_ = 0.0f;
    bool haveBackground_ = false;
    std::uint16_t background_ = 0;
    float bgWidth_ = 640.0f, bgHeight_ = 480.0f;
    Fade fade_ = Fade::None;
    int fadeT_ = 0, fadeN_ = 0, fadeAlpha_ = 0;
    bool fadeWhite_ = false;
    bool seen_ = false;
    bool done_ = true;
    bool prevShoot_ = false;
    int steps_ = 0;
    int shotType_ = 0;
};

}  // namespace touhou::th06
