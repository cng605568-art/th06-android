#include "th07/ScreenEffects.hpp"

#include <algorithm>

#include "core/ZunMath.hpp"
#include "gfx/SpriteSink.hpp"

namespace touhou::th07 {

void ScreenEffects::shake(int duration, float from, float to) {
    Effect e;
    e.type = Type::Shake;
    e.duration = duration;
    e.arg1 = static_cast<std::int32_t>(from);  // whole pixels in the original's interface
    e.arg2 = static_cast<std::int32_t>(to);
    effects_.push_back(e);
}

void ScreenEffects::pulse(int duration, int count, std::uint32_t argb) {
    Effect e;
    e.type = Type::Pulse;
    e.duration = duration;
    e.arg1 = count;
    e.color = argb;
    effects_.push_back(e);
}

void ScreenEffects::fade(int type, int duration, std::uint32_t rgb) {
    if (type != 0 && type != 2 && type != 4) return;
    Effect e;
    e.type = static_cast<Type>(type);
    e.duration = duration;
    e.color = rgb;
    effects_.push_back(e);
}

void ScreenEffects::update(ZunRng& rng, const GameState& g, float& offsetX, float& offsetY) {
    for (std::size_t i = 0; i < effects_.size();) {
        Effect& e = effects_[i];
        bool remove = false;
        switch (e.type) {
            case Type::Shake: {
                if (g.timeStop) break;  // no advance, no draws
                if (g.stageFrame <= 1) {
                    remove = true;
                    break;
                }
                e.timer.tick();
                if (e.timer.current >= e.duration) {
                    remove = true;  // no draw on the last frame
                    break;
                }
                const float o = static_cast<float>(e.arg2 - e.arg1) * e.timer.asFloat() / static_cast<float>(e.duration) +
                                static_cast<float>(e.arg1);
                const auto pick = [&](std::uint32_t r) { return r == 0 ? 0.0f : r == 1 ? o : -o; };
                offsetX = pick(rng.u32InRange(3));
                offsetY = pick(rng.u32InRange(3));
                break;
            }
            case Type::Pulse: {
                const std::int32_t peak = static_cast<std::int32_t>((e.color >> 24) & 0xFF);
                if (e.timer.current < e.duration) {
                    e.alpha = std::max(0, peak - static_cast<std::int32_t>(static_cast<float>(peak) * e.timer.asFloat() /
                                                                              static_cast<float>(e.duration)));
                } else {
                    // [QUIRK] One alpha-0 frame between cycles; later cycles start at timer 1.
                    e.alpha = 0;
                    if (--e.arg1 <= 0) {
                        remove = true;
                        break;
                    }
                    e.timer.assign(0);
                }
                e.timer.tick();
                break;
            }
            default: {  // fades
                if (e.duration != 0) {
                    const float t = e.timer.asFloat() * 255.0f / static_cast<float>(e.duration);
                    const float a = e.type == Type::FadeFromScreen ? 255.0f - t : t;
                    e.alpha = std::max(0, static_cast<std::int32_t>(a));
                }
                if (e.timer.current >= e.duration) {
                    remove = true;  // not drawn again
                    break;
                }
                e.timer.tick();
                break;
            }
        }
        if (remove) {
            effects_.erase(effects_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ScreenEffects::draw(SpriteSink& sink, const SpriteRegion& white) const {
    for (const Effect& e : effects_) {
        if (e.type == Type::Shake) continue;
        const auto a = static_cast<std::uint32_t>(e.alpha) << 24;
        const bool fullScreen = e.type == Type::FadeFromScreen || e.type == Type::FadeToScreen;
        // Fades OR the colour in unmasked [QUIRK]; pulses keep only its RGB.
        const std::uint32_t argb = e.type == Type::Pulse ? (a | (e.color & 0x00FFFFFFu)) : (a | e.color);
        if (fullScreen) {
            sink.fillRect(white, 0.0f, 0.0f, 640.0f, 480.0f, argbToRgbaBytes(argb));
        } else {
            sink.fillRect(white, kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH, argbToRgbaBytes(argb));
        }
    }
}

}  // namespace touhou::th07
