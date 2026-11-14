#include "th08/ScreenEffects.hpp"

#include <algorithm>

#include "gfx/SpriteSink.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

void ScreenEffects::reset() {
    jobs_.clear();
    offset_ = {};
}

void ScreenEffects::shake(std::int32_t frames, float from, float to) {
    Job j;
    j.type = 1;
    j.frames = frames;
    j.from = from;
    j.to = to;
    j.timer.assign(0);
    jobs_.push_back(j);
}

void ScreenEffects::rampedShake(float amplitude, std::int32_t up, std::int32_t hold, std::int32_t down) {
    Job j;
    j.type = 7;
    j.amplitude = amplitude;
    j.up = up;
    j.hold = hold;
    j.down = down;
    j.timer.assign(0);
    jobs_.push_back(j);
}

void ScreenEffects::flash(std::int32_t frames, std::int32_t repeats, std::uint32_t color) {
    Job j;
    j.type = 3;
    j.frames = frames;
    j.repeats = repeats;
    j.color = color;
    j.timer.assign(0);
    jobs_.push_back(j);
}

void ScreenEffects::fadeTo(std::int32_t frames, std::uint32_t color) {
    Job j;
    j.type = 4;
    j.frames = frames;
    j.color = color;
    j.timer.assign(0);
    jobs_.push_back(j);
}

void ScreenEffects::playfieldFadeTo(std::int32_t frames, std::uint32_t color) {
    fadeTo(frames, color);
    jobs_.back().type = 2;
}

// Each shake frame draws two u32 mod 3 (x, then y): 0 none, 1 +offset, 2 -offset.
static Vec2f shakeOffset(ZunRng& rng, float offset) {
    Vec2f o;
    const std::uint32_t x = rng.u32InRange(3), y = rng.u32InRange(3);
    o.x = x == 0 ? 0.0f : x == 1 ? offset : -offset;
    o.y = y == 0 ? 0.0f : y == 1 ? offset : -offset;
    return o;
}

void ScreenEffects::update(const World& w) {
    // The offset goes back to zero every frame (at the end of the popup draw in the original).
    offset_ = {};
    const bool frozen = (w.mode & ModeSpellFreeze) || w.timeStop;
    for (std::size_t i = 0; i < jobs_.size();) {
        Job& j = jobs_[i];
        bool remove = false;
        switch (j.type) {
            case 1:
                if (frozen) break;
                if (w.screenSuppress != 0) {
                    remove = true;
                    break;
                }
                j.timer.tick();
                if (j.timer.current >= j.frames) {
                    remove = true;
                    break;
                }
                if (w.rng) offset_ = shakeOffset(*w.rng, j.timer.asFloat() * (j.to - j.from) / static_cast<float>(j.frames) + j.from);
                break;
            case 7: {
                if (frozen) break;
                if (w.stageFrame <= 1) {
                    remove = true;
                    break;
                }
                j.timer.tick();
                const std::int32_t t = j.timer.current;
                float f;
                if (t < j.up) f = static_cast<float>(t) / static_cast<float>(j.up);
                else if (t < j.up + j.hold) f = 1.0f;
                else if (t < j.up + j.hold + j.down) f = static_cast<float>(j.up + j.hold + j.down - t) / static_cast<float>(j.down);
                else {
                    remove = true;
                    break;
                }
                if (w.rng) offset_ = shakeOffset(*w.rng, j.amplitude * f);
                break;
            }
            case 2:
            case 4:
                if (w.screenSuppress != 0) {
                    remove = true;
                    break;
                }
                if (j.timer.current < j.frames) j.timer.tick();
                break;
            case 3:
                if (w.screenSuppress != 0) {
                    remove = true;
                    break;
                }
                j.timer.tick();
                if (j.timer.current >= j.frames) {
                    if (--j.repeats > 0) j.timer.assign(0);
                    else remove = true;
                }
                break;
            default: remove = true; break;
        }
        if (remove) jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void ScreenEffects::draw(SpriteSink& sink, const SpriteRegion* white) const {
    if (!white) return;
    for (const Job& j : jobs_) {
        if ((j.type == 4 || j.type == 2) && j.frames > 0) {
            const std::uint32_t c = j.color;
            const auto alpha = static_cast<std::uint8_t>(std::min(255, j.timer.current * 255 / j.frames));
            const bool field = j.type == 2;
            sink.fillRect(*white, field ? kPlayfieldX : 0.0f, field ? kPlayfieldY : 0.0f, field ? kPlayfieldW : 640.0f, field ? kPlayfieldH : 480.0f,
                          rgba(static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c), alpha));
            continue;
        }
        if (j.type != 3 || j.frames <= 0) continue;
        const std::int32_t a = static_cast<std::int32_t>(j.color >> 24);
        const std::int32_t alpha = std::max(0, a - j.timer.current * a / j.frames);
        const std::uint32_t c = j.color;
        sink.fillRect(*white, kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH,
                      rgba(static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c),
                           static_cast<std::uint8_t>(alpha)));
    }
}

}  // namespace touhou::th08
