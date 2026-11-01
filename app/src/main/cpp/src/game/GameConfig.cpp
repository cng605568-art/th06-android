#include "game/GameConfig.hpp"

#include <cstring>

namespace th06 {
namespace {

template <typename T>
T rd(std::span<const std::uint8_t> b, std::size_t at) {
    T v{};
    std::memcpy(&v, b.data() + at, sizeof v);  // little-endian on every target
    return v;
}

template <typename T>
void wr(std::vector<std::uint8_t>& b, std::size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof v);
}

}  // namespace

bool GameConfig::load(std::span<const std::uint8_t> b) {
    *this = GameConfig{};
    if (b.size() != kSize || rd<std::int32_t>(b, 0x14) != kVersion) return false;
    GameConfig c;
    for (std::size_t i = 0; i < c.pad.size(); ++i) c.pad[i] = rd<std::int16_t>(b, 2 * i);
    c.lives = b[0x18];
    c.bombs = b[0x19];
    c.colourMode = b[0x1A];
    c.music = b[0x1B];
    c.sound = b[0x1C];
    c.defaultDifficulty = b[0x1D];
    c.windowMode = b[0x1E];
    c.frameSkip = b[0x1F];
    c.padX = rd<std::int16_t>(b, 0x20);
    c.padY = rd<std::int16_t>(b, 0x22);
    c.optionBits = rd<std::uint32_t>(b, 0x34);
    if (c.lives >= 5 || c.bombs >= 4 || c.colourMode >= 2 || c.music >= 3 || c.defaultDifficulty >= 5 || c.sound >= 2 ||
        c.windowMode >= 2 || c.frameSkip >= 3) {
        return false;
    }
    *this = c;
    return true;
}

std::vector<std::uint8_t> GameConfig::save() const {
    std::vector<std::uint8_t> b(kSize, 0);
    for (std::size_t i = 0; i < pad.size(); ++i) wr(b, 2 * i, pad[i]);
    wr(b, 0x14, kVersion);
    b[0x18] = lives;
    b[0x19] = bombs;
    b[0x1A] = colourMode;
    b[0x1B] = music;
    b[0x1C] = sound;
    b[0x1D] = defaultDifficulty;
    b[0x1E] = windowMode;
    b[0x1F] = frameSkip;
    wr(b, 0x20, padX);
    wr(b, 0x22, padY);
    wr(b, 0x34, optionBits);
    return b;
}

}  // namespace th06
