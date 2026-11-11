#pragma once

// th07.cfg, Perfect Cherry Blossom's settings file (0x38 bytes, version 0x00070002): pad
// mapping and thresholds, the Option menu's lives / bombs / colour mode / music mode / sound
// effects / screen mode / slow mode, the remembered difficulty, frame skip, effect quality,
// "shot slow" and render option bits. Its validation and re-initialisation follow the
// original, including the fields a rebuild keeps from an invalid file. The port keeps its
// own copy in the app's storage. Replays carry a 56-byte copy of it, so the image is exact.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace touhou::th07 {

struct GameConfig {
    static constexpr std::size_t kSize = 0x38;
    static constexpr std::int32_t kVersion = 0x00070002;
    enum Music : std::uint8_t { MusicOff = 0, MusicWav = 1, MusicMidi = 2 };

    // shoot, bomb, focus, menu, up, down, left, right, skip (pad button, -1 = none)
    std::array<std::int16_t, 9> pad{0, 1, 2, 4, -1, -1, -1, -1, 3};
    std::array<std::uint8_t, 2> padding{};
    std::int32_t version = kVersion;
    std::int16_t padX = 600, padY = 600;  // stick thresholds of +-1000
    std::uint8_t lives = 2;               // spare lives at the start of a game, 0..4
    std::uint8_t bombs = 3;               // 0..3 (validated, never used: shot data decides)
    std::uint8_t colourMode = 255;        // 0 32-bit, 1 16-bit, 255 automatic
    std::uint8_t music = MusicWav;        // 0 off, 1 WAV (thbgm.dat), 2 MIDI
    std::uint8_t sound = 1;               // sound effects on
    std::uint8_t difficulty = 1;          // the difficulty select's cursor: 0..3, 4 Extra, 5 Phantasm
    std::uint8_t windowed = 0;
    std::uint8_t frameSkip = 0;           // 0, 1 (every 2nd frame), 2 (every 3rd)
    std::uint8_t effectQuality = 2;       // 0 worst, 1 medium, 2 beautiful
    std::uint8_t slowMode = 0;
    std::uint8_t shotSlow = 1;            // pad: holding shoot adds focus
    std::array<std::uint8_t, 13> unused{};
    std::uint32_t optionBits = 1;

    // Loads `bytes` (empty: no file). A file of the wrong size or version, or with a value
    // out of range, is re-initialised the original's way (keeping its padding, unused bytes
    // and option bits); returns false then. `thbgmPresent` picks the music mode a fresh or
    // rebuilt file gets (WAV with thbgm.dat, MIDI without). Bit 0 of the option bits is
    // always forced on and bit 7 off.
    bool load(std::span<const std::uint8_t> bytes, bool thbgmPresent);
    std::vector<std::uint8_t> save() const;  // exactly 56 bytes

    bool operator==(const GameConfig&) const = default;
};

}  // namespace touhou::th07
