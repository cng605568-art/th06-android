#pragma once

// th08.cfg, Imperishable Night's settings file (0x3C bytes, version 0x00080001): pad mapping
// and thresholds, the Option menu's lives / colour mode / music mode / BGM and SE volume /
// screen mode / slow mode, the remembered difficulty, frame skip, effect quality, "shot
// slow" and render option bits (claude/th08-specs/spec_files.md §4). An invalid file is
// zeroed and re-initialised (TH07 kept the stale bytes). The port keeps its own copy in the
// app's storage.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace touhou::th08 {

struct GameConfig {
    static constexpr std::size_t kSize = 0x3C;
    static constexpr std::int32_t kVersion = 0x00080001;
    enum Music : std::uint8_t { MusicOff = 0, MusicWav = 1, MusicMidi = 2 };

    // shoot, bomb, focus, menu, up, down, left, right, skip (pad button, -1 = none)
    std::array<std::int16_t, 9> pad{0, 1, 2, 4, -1, -1, -1, -1, 3};
    std::array<std::uint8_t, 2> padding{};
    std::int32_t version = kVersion;
    std::int16_t padX = 600, padY = 600;  // stick thresholds of +-1000
    std::uint8_t lives = 2;               // spare lives at the start of a game, 0..6
    std::uint8_t bombs = 3;               // 0..3 (validated)
    std::uint8_t colourMode = 0;          // 0 32-bit, 1 16-bit
    std::uint8_t music = MusicWav;        // 0 off, 1 WAV (thbgm.dat), 2 MIDI
    std::uint8_t sound = 1;               // sound effects on
    std::uint8_t difficulty = 1;          // remembered: 0..3, 4 Extra, 5 Last Word
    std::uint8_t windowed = 0;
    std::uint8_t frameSkip = 0;           // 0, 1 (every 2nd frame), 2 (every 3rd)
    std::uint8_t effectQuality = 2;       // 0 worst, 1 medium, 2 beautiful
    std::uint8_t slowMode = 0;
    std::uint8_t shotSlow = 0;            // pad: holding shoot adds focus
    std::int8_t bgmVolume = 100;          // 0..100 (the Option menu steps by 4; not validated)
    std::int8_t seVolume = 80;            // 0..100 (not validated)
    std::array<std::uint8_t, 15> unused{};
    std::uint32_t optionBits = 1;

    // Loads `bytes` (empty: no file). A file of the wrong size or version, or with a value
    // out of range, is zeroed and re-initialised; returns false then. `thbgmPresent` picks
    // the music mode a fresh file gets (WAV with thbgm.dat, MIDI without). Bit 0 of the
    // option bits is always forced on and bit 7 off.
    bool load(std::span<const std::uint8_t> bytes, bool thbgmPresent);
    std::vector<std::uint8_t> save() const;  // exactly 60 bytes

    bool operator==(const GameConfig&) const = default;
};

}  // namespace touhou::th08
