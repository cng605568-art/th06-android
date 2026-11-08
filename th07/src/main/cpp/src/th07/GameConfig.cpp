#include "th07/GameConfig.hpp"

#include <algorithm>
#include <cstring>

namespace touhou::th07 {
namespace {

std::int16_t rd16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::int16_t>(b[at] | b[at + 1] << 8); }
std::uint32_t rd32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
void wr16(std::vector<std::uint8_t>& b, std::size_t at, std::int16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8);
}
void wr32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

}  // namespace

bool GameConfig::load(std::span<const std::uint8_t> bytes, bool thbgmPresent) {
    const GameConfig defaults;
    *this = GameConfig{};
    bool valid = false;
    if (!bytes.empty()) {
        // The original copies the file's first 56 bytes in (a shorter file counts as invalid).
        std::array<std::uint8_t, kSize> raw{};
        std::memcpy(raw.data(), bytes.data(), std::min(bytes.size(), kSize));
        const std::span<const std::uint8_t> b(raw);
        for (std::size_t i = 0; i < 9; ++i) pad[i] = rd16(b, 2 * i);
        padding = {b[0x12], b[0x13]};
        version = static_cast<std::int32_t>(rd32(b, 0x14));
        padX = rd16(b, 0x18);
        padY = rd16(b, 0x1A);
        lives = b[0x1C], bombs = b[0x1D], colourMode = b[0x1E], music = b[0x1F], sound = b[0x20], difficulty = b[0x21];
        windowed = b[0x22], frameSkip = b[0x23], effectQuality = b[0x24], slowMode = b[0x25], shotSlow = b[0x26];
        std::memcpy(unused.data(), b.data() + 0x27, unused.size());
        optionBits = rd32(b, 0x34);
        valid = bytes.size() == kSize && version == kVersion && lives < 5 && bombs < 4 && colourMode < 2 && music < 3 &&
                sound < 2 && difficulty < 6 && windowed < 2 && frameSkip < 3 && effectQuality < 3 && slowMode < 2 &&
                shotSlow < 2;
        if (!valid) {
            // Re-initialised without clearing again: padding, unused bytes and option bits
            // keep the invalid file's values, as in the original.
            const auto keepPadding = padding;
            const auto keepUnused = unused;
            const auto keepBits = optionBits;
            *this = defaults;
            padding = keepPadding;
            unused = keepUnused;
            optionBits = keepBits;
        }
    }
    if (!valid) music = thbgmPresent ? MusicWav : MusicMidi;
    optionBits = (optionBits | 1u) & ~0x80u;
    return valid;
}

std::vector<std::uint8_t> GameConfig::save() const {
    std::vector<std::uint8_t> b(kSize, 0);
    for (std::size_t i = 0; i < 9; ++i) wr16(b, 2 * i, pad[i]);
    b[0x12] = padding[0];
    b[0x13] = padding[1];
    wr32(b, 0x14, static_cast<std::uint32_t>(version));
    wr16(b, 0x18, padX);
    wr16(b, 0x1A, padY);
    b[0x1C] = lives, b[0x1D] = bombs, b[0x1E] = colourMode, b[0x1F] = music, b[0x20] = sound, b[0x21] = difficulty;
    b[0x22] = windowed, b[0x23] = frameSkip, b[0x24] = effectQuality, b[0x25] = slowMode, b[0x26] = shotSlow;
    std::memcpy(b.data() + 0x27, unused.data(), unused.size());
    wr32(b, 0x34, optionBits);
    return b;
}

}  // namespace touhou::th07
