#pragma once

// th06.cfg, the 2002 game's settings file (0x38 bytes): pad mapping, the Option menu's
// lives / bombs / colour mode / music mode / sound effects / screen mode, the remembered
// difficulty, frame skip, pad thresholds and render option bits. The port keeps its own
// copy in the app's storage (the colour and screen modes and the pad mapping are kept for
// the format's sake; Android maps controllers itself).

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace touhou::th06 {

struct GameConfig {
    static constexpr std::size_t kSize = 0x38;
    static constexpr std::int32_t kVersion = 0x0102;
    enum Music : std::uint8_t { MusicOff = 0, MusicWav = 1, MusicMidi = 2 };

    // shoot, bomb, focus, menu, up, down, left, right, skip (pad button, -1 = none)
    std::array<std::int16_t, 9> pad{0, 1, 2, 4, -1, -1, -1, -1, 3};
    std::uint8_t lives = 2;              // spare lives at the start of a game, 0..4
    std::uint8_t bombs = 3;              // 0..3
    std::uint8_t colourMode = 0;         // 0 32-bit, 1 16-bit (the port always draws in 32-bit)
    std::uint8_t music = MusicWav;       // 0 off, 1 WAV, 2 MIDI
    std::uint8_t sound = 1;              // sound effects on
    std::uint8_t defaultDifficulty = 1;  // the difficulty select's cursor, 0..4
    std::uint8_t windowMode = 0;         // 0 full screen, 1 window (no meaning on a phone)
    std::uint8_t frameSkip = 0;
    std::int16_t padX = 600, padY = 600;  // stick thresholds of +-1000
    std::uint32_t optionBits = 1;

    // The rebuild rules of the original: a file of the wrong size or version, or with any
    // value out of range, gives the defaults (false).
    bool load(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> save() const;

    bool operator==(const GameConfig&) const = default;
};

}  // namespace touhou::th06
