#pragma once

// Perfect Cherry Blossom's screen effects: colour fades over the screen or the playfield,
// shakes and pulses. One list in registration order, updated at logic priority 15 (after
// the GUI) and drawn at priority 17 (after the pause and continue menus). Only shakes touch
// the simulation: each running shake draws two numbers per frame.

#include <cstdint>
#include <vector>

#include "th07/Anm.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
struct SpriteRegion;
}  // namespace touhou

namespace touhou::th07 {

class ScreenEffects final : public ShakeSink {
  public:
    enum class Type : std::uint8_t {
        FadeFromScreen = 0,  // alpha 255 -> 0, full screen
        Shake = 1,
        FadeToPlayfield = 2,  // alpha 0 -> 255, playfield
        Pulse = 3,            // playfield
        FadeToScreen = 4,     // alpha 0 -> 255, full screen
    };

    // ShakeSink.
    void shake(int duration, float from, float to) override;
    void pulse(int duration, int count, std::uint32_t argb) override;
    void fade(int type, int duration, std::uint32_t rgb) override;

    // Logic priority 15. Shakes write the draw offset (zeroed first by the caller).
    void update(ZunRng& rng, const GameState& g, float& offsetX, float& offsetY);
    // Draw priority 17.
    void draw(SpriteSink& sink, const SpriteRegion& white) const;
    void clear() { effects_.clear(); }
    std::size_t size() const { return effects_.size(); }

  private:
    struct Effect {
        Type type = Type::Shake;
        int duration = 0;
        std::int32_t arg1 = 0;  // shake: start amplitude (pixels); pulse: cycles left
        std::int32_t arg2 = 0;  // shake: end amplitude
        std::uint32_t color = 0;  // fades: RGB (not masked [QUIRK]); pulse: ARGB, alpha = peak
        Timer timer;
        std::int32_t alpha = 0;
    };
    std::vector<Effect> effects_;
};

}  // namespace touhou::th07
