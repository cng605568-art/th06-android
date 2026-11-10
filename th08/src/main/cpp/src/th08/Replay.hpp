#pragma once

// Imperishable Night's replays (.rpy, claude/th08-specs/spec_files.md §6 and spec_replays.md),
// in the PC game's format so its files (and the four attract demos in th08.dat) play here:
//   a 0x68-byte header ("T8RP", version 6, the end of the compressed data, checksum, filler,
//   obfuscation key, compressed and body sizes, 9 stage-block and 9 FPS-block offsets by stage
//   slot), the LZSS-compressed body, then optional USER sections (plain text). The image
//   (header + body) holds the 0xCC-byte summary at 0x68 (character, difficulty, date, name,
//   practice flag, the spell-practice card and its name, score, the configuration copy,
//   slowdown, the route byte, the executable's size, checksum and version), then the stage
//   blocks (a 0x24-byte header with the state at the stage's set-up and its RNG seed, then one
//   u16 game-input word per stage frame, entry k being the input of frame k) and the FPS
//   blocks (one byte per 30 frames). Bytes from 0x18 to the end of the compressed data are
//   obfuscated with a running additive key (+7 per byte); the checksum covers the plain bytes
//   from 0x15 to that end.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/ZunMath.hpp"

namespace touhou::th08 {

inline constexpr int kReplayStages = 9;  // stage slots = stage indices 0-8
inline constexpr std::size_t kReplayHeaderSize = 0x68;
inline constexpr std::size_t kReplaySummarySize = 0xCC;
inline constexpr std::size_t kReplayBlockHeaderSize = 0x24;
inline constexpr std::size_t kReplayFirstBlock = kReplayHeaderSize + kReplaySummarySize;  // 0x134
inline constexpr std::uint16_t kReplayVersion = 6;
inline constexpr std::uint32_t kReplayChecksumBase = 0x3F000318u;
// The original's stage buffer: 432 000 entries; the port stops recording there.
inline constexpr std::size_t kReplayMaxEntries = 432000;
inline constexpr int kReplaySlots = 15;      // replay/th8_01.rpy .. th8_15.rpy
inline constexpr int kReplayUserFiles = 45;  // replay/th8_ud????.rpy

// A stage block (spec_replays 2.4): the state snapshot at the stage's set-up, its entries
// (entry 0 = 0, then one per recorded frame, then the tail of the final block) and its FPS
// samples.
struct ReplayStage {
    bool present = false;
    std::uint32_t carriedScore = 0;   // +0x00: the score carried into the next recorded stage
    std::int32_t pointItems = 0;      // +0x04: toward the next extend
    std::int32_t graze = 0;           // +0x08: the game's total
    std::int32_t extends = 0;         // +0x0C
    std::int32_t nextExtend = 0;      // +0x10: the threshold in point items
    std::int32_t pointValue = 0;      // +0x14: the maximum point-item value
    std::int16_t gauge = 0;           // +0x18: the human/youkai gauge
    std::uint16_t seed = 0;           // +0x1A: the stage's RNG seed
    std::uint8_t power = 0, lives = 0, bombs = 0, rank = 0;  // +0x1C-0x1F
    std::uint8_t fullPowerByte = 0;   // +0x20 (GM+0x3DBA8)
    std::uint8_t cardsCaptured = 0;   // +0x21
    std::int8_t nightSteps = 0;       // +0x22
    std::uint8_t garbage = 0;         // +0x23: never written by the original
    std::vector<std::uint16_t> entries;
    std::vector<std::uint8_t> fps;
};

// The summary (image offset 0x68), kept as its 0xCC bytes so unknown bytes round-trip.
struct ReplaySummary {
    std::array<std::uint8_t, kReplaySummarySize> bytes{};

    int character() const { return bytes[0x02]; }
    int difficulty() const { return bytes[0x03]; }
    std::string date() const;  // "MM/DD"
    std::string name() const;  // up to 8 characters
    bool practice() const { return bytes[0x13] != 0; }
    int card() const { return static_cast<std::int16_t>(bytes[0x14] | bytes[0x15] << 8); }  // -1 outside spell practice
    std::string cardName() const;
    std::uint32_t score() const;
    std::span<const std::uint8_t> config() const { return std::span(bytes).subspan(0x4C, 60); }
    bool slowModeRecorded() const { return bytes[0x4C + 0x25] != 0; }
    float slowdown() const;
    int routeByte() const { return bytes[0xB4]; }
    std::int32_t exeSize() const;
    std::int32_t exeChecksum() const;
    std::string versionTag() const;  // the 5 characters at 0xC4

    void setCharacter(int c) { bytes[0x02] = static_cast<std::uint8_t>(c); }
    void setDifficulty(int d) { bytes[0x03] = static_cast<std::uint8_t>(d); }
    void setDate(std::string_view mmdd);
    void setName(std::string_view name);  // 8 characters (padded with spaces) + NUL
    void setPractice(bool on) { bytes[0x13] = on ? 1 : 0; }
    void setCard(int card);
    void setCardName(std::span<const std::uint8_t> name48);
    void setScore(std::uint32_t score);
    void setConfig(std::span<const std::uint8_t> config60);
    void setSlowdown(float percent);  // the value and its +1.12 copy, and the 30
    void setRouteByte(int b) { bytes[0xB4] = static_cast<std::uint8_t>(b); }
    void setExe(std::int32_t size, std::int32_t checksum, std::string_view tag = "0100d");
};

struct ReplayFile {
    std::uint8_t headerFiller = 0, key = 0;
    ReplaySummary summary;
    std::array<ReplayStage, kReplayStages> stages{};
    std::vector<std::uint8_t> user;  // the USER sections after the compressed data, as stored

    int firstStage() const;
    // The next slot above `index` that has a block, -1 none (the playback's next stage).
    int nextStage(int index) const;
    bool hasStage(int index) const { return index >= 0 && index < kReplayStages && stages[static_cast<std::size_t>(index)].present; }

    // FILES 6.7: magic and version, de-obfuscation and checksum, decompression, slow mode off
    // in the recorded configuration; with `headerOnly` the entries are not kept (the replay
    // list). The version triple is not checked here (the caller decides). nullopt with
    // `error` on failure.
    static std::optional<ReplayFile> parse(std::span<const std::uint8_t> file, std::string* error = nullptr, bool headerOnly = false);
    // spec_replays 4.1 step 7: the present blocks in slot order (from 0x134), then their FPS
    // blocks; the key, the summary filler and the header filler drawn from `rng` in that order;
    // compressed, checksummed, obfuscated; `userText` (Shift-JIS) appended as one USER section.
    std::vector<std::uint8_t> serialize(ZunRng& rng, std::string_view userText = {});

    static void obfuscate(std::span<std::uint8_t> file);    // from 0x18 to the end of the compressed data
    static void deobfuscate(std::span<std::uint8_t> file);
    static std::uint32_t checksum(std::span<const std::uint8_t> plainFile);
};

}  // namespace touhou::th08
