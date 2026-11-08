#pragma once

// Perfect Cherry Blossom's replays (.rpy, spec 7), in the PC game's format so its files play
// here and the port's files play on the PC:
//   a 0x54-byte header ("T7RP", version 0x1100, checksum, filler, obfuscation key, image and
//   body sizes, 7 stage-block and 7 FPS-block offsets), then the LZSS-compressed body. The
//   uncompressed image continues with the 0x94-byte summary (shot, difficulty, date, name,
//   final score, the configuration copy, slowdown, the executable's size and checksum and its
//   version string), the stage blocks (a 0x2C-byte header with the state at the stage's start
//   and its RNG seed, then one 4-byte entry per simulated frame: the 16-bit button word and
//   the frame's event flags) and the FPS blocks (one byte per 30 frames). Bytes from 0x10 on
//   are obfuscated with a running additive key; the checksum covers the plain bytes from
//   0x0D on.
// Recording (ReplayRecorder) and playback (ReplayFeed) follow the original's hooks: the
// recorder latches the game input at the end of each simulated pass and writes the raw input,
// the feed hands each pass the next entry.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/ZunMath.hpp"
#include "th07/Version.hpp"

namespace touhou::th07 {

struct GameConfig;
struct World;

inline constexpr int kReplayStages = 7;  // stages 1-6, then Extra or Phantasm
inline constexpr std::size_t kReplayHeaderSize = 0x54;
inline constexpr std::size_t kReplaySummarySize = 0x94;
inline constexpr std::size_t kReplayBlockHeaderSize = 0x2C;
inline constexpr std::size_t kReplayFirstBlock = kReplayHeaderSize + kReplaySummarySize;  // 0xE8
inline constexpr std::uint16_t kReplayVersion = 0x1100;
inline constexpr std::uint32_t kReplayChecksumBase = 0x3F000318u;
// The original's stage buffer (about 32 minutes); the port stops recording there.
inline constexpr std::size_t kReplayMaxEntries = 115189;
inline constexpr std::uint16_t kReplayEventPaused = 0x0100;
inline constexpr int kReplaySlots = 15;  // replay/th7_01.rpy .. th7_15.rpy
inline constexpr int kReplayUserFiles = 45;  // replay/th7_ud????.rpy

// One input entry: the whole 16-bit raw button word and the frame's event flags.
struct ReplayEntry {
    std::uint16_t input = 0;
    std::uint16_t flags = 0;
    bool operator==(const ReplayEntry&) const = default;
};

// A stage block: the state captured at the end of the stage's set-up, its entries (entry 0,
// one per recorded frame, then the terminators) and its FPS samples.
struct ReplayStage {
    bool present = false;
    std::uint32_t endScore = 0;  // internal units, written when the next stage begins / at save
    std::int32_t extendPoints = 0;
    std::int32_t cherry = 0, cherryMax = 0, cherryPlus = 0;  // relative to the cherry base
    std::int32_t totalGraze = 0;
    std::int32_t extendsGranted = 0;
    std::int32_t nextExtend = 0;
    std::uint16_t seed = 0;
    std::uint8_t power = 0, lives = 0, bombs = 0, rank = 0, chain = 0, spells = 0;
    std::int32_t unused = 0;  // uninitialised memory in the original
    std::vector<ReplayEntry> entries;
    std::vector<std::uint8_t> fps;
};

// The summary (image offset 0x54), kept as its 0x94 bytes so unknown bytes round-trip.
struct ReplaySummary {
    std::array<std::uint8_t, kReplaySummarySize> bytes{};

    int shot() const { return bytes[0x02]; }
    int difficulty() const { return bytes[0x03]; }
    std::string date() const;  // "MM/DD"
    std::string name() const;  // up to 8 characters (space-padded as stored)
    std::uint32_t score() const;
    std::span<const std::uint8_t> config() const { return std::span(bytes).subspan(0x1C, 0x38); }
    bool slowModeRecorded() const { return bytes[0x1C + 0x25] != 0; }
    int lifeCount() const { return bytes[0x1C + 0x1C]; }
    float slowdown() const;
    std::int32_t exeSize() const;
    std::int32_t exeChecksum() const;
    std::string versionTag() const;  // the 5 characters at 0x8C

    void setShot(int shot) { bytes[0x02] = static_cast<std::uint8_t>(shot); }
    void setDifficulty(int d) { bytes[0x03] = static_cast<std::uint8_t>(d); }
    void setDate(std::string_view mmdd);
    void setName(std::string_view name);  // 8 characters (padded with spaces) + NUL
    void setScore(std::uint32_t score);
    void setConfig(std::span<const std::uint8_t> config56);
    void setSlowdown(float percent);  // the value and its +1.12 / +2.34 copies, and the 30
    void setExe(const ExeIdentity& exe, std::string_view tag = kVersionTag);
};

struct ReplayFile {
    std::uint8_t headerFiller = 0, key = 0;
    ReplaySummary summary;
    std::array<ReplayStage, kReplayStages> stages{};

    // The first stage block present (0-based), -1 none.
    int firstStage() const;
    bool hasStage(int index) const { return index >= 0 && index < kReplayStages && stages[static_cast<std::size_t>(index)].present; }
    // Stage-block bit mask (bit i: stage index i present).
    std::uint8_t stageMask() const;

    // Spec 7.7: magic and version, de-obfuscation and checksum, decompression, slow mode off
    // in the recorded configuration, and the integrity check of the summary's version, exe
    // size and checksum against th07_0100b.ver (`verFile`; empty: passes). With `headerOnly`
    // the stage entries are not kept (the replay list). nullopt with `error` on failure.
    static std::optional<ReplayFile> parse(std::span<const std::uint8_t> file, std::span<const std::uint8_t> verFile,
                                           std::string* error = nullptr, bool headerOnly = false);
    // Spec 7.6.3 steps 4-9: the present blocks concatenated in index order (offsets from
    // 0xE8), then their FPS blocks; the key, the summary filler and the header filler drawn
    // from `rng` in that order (three u16 draws); compressed, checksummed, obfuscated.
    std::vector<std::uint8_t> serialize(ZunRng& rng);

    // The container's codec (offset 0x10 on), exposed for tests.
    static void obfuscate(std::span<std::uint8_t> file);
    static void deobfuscate(std::span<std::uint8_t> file);
    static std::uint32_t checksum(std::span<const std::uint8_t> plainFile);
};

// The integrity check of spec 5.8: the first line of `verFile` whose tag is `tag` must carry
// this size and checksum; "debug" and an empty file always pass.
bool replayIntegrityOk(std::span<const std::uint8_t> verFile, std::string_view tag, std::int32_t exeSize, std::int32_t exeChecksum);

// Spec 7.8.2: the state block `index` restores inside the stage's set-up (step 11): extend
// progress, rank, lives (a regeneration draw), bombs, power (a draw), total graze, the
// power-item chain, the cherry values (cherry-plus clamped at its full value), the recorded
// configuration copy into `config` (and the life count and slow mode of the game state),
// spell cards captured, extends from points and the next threshold; for stages 2-6 with the
// previous block present, score and HUD score = that block's end score; last the RNG seed.
void restoreReplayStage(const World& w, const ReplayFile& file, int index, GameConfig& config);

// Recording (spec 7.5): created at the first stage's set-up, a new block at every later
// stage, the entries written by the end-of-pass hook. It outlives the game (the result screen
// saves it).
class ReplayRecorder {
  public:
    struct StageStart {
        std::uint16_t seed = 0;  // the stage seed (set-up step 13)
        std::uint32_t score = 0;  // the score now (the previous block's end score)
        std::int32_t extendPoints = 0, cherry = 0, cherryMax = 0, cherryPlus = 0, totalGraze = 0;
        std::int32_t extendsGranted = 0, nextExtend = 0;
        int power = 0, lives = 0, bombs = 0, rank = 0, chain = 0, spells = 0;
    };
    // The recording of a new game: shot, difficulty, the configuration copy after the set-up's
    // overrides, the executable stamp; the name starts as "NO N" [QUIRK].
    void create(int shot, int difficulty, const GameConfig& configCopy, const ExeIdentity& exe);
    bool active() const { return created_; }
    // Starts block `index` (0-6) with the values at the end of the stage's set-up.
    void startBlock(int index, const StageStart& s);
    // The record hook's write (spec 7.5.2 steps 4-6): one entry, an FPS sample every 30th
    // frame (`fps`, the timing-bad flag in bit 7 when `timingErrors`).
    void write(std::uint16_t raw, std::uint16_t flags, int fps = 60, bool timingErrors = false);
    // A terminator entry (game teardown, and once more at the save).
    void closeBlock();
    int currentBlock() const { return current_; }
    std::int32_t frame() const { return frame_; }

    // The save (spec 7.6.3): the second terminator, the current score as the last block's end
    // score, the summary's score, slowdown, name and date; the file's bytes (three RNG draws).
    std::vector<std::uint8_t> save(std::uint32_t score, std::uint32_t guiScore, float slowdown, std::string_view name,
                                   std::string_view date, ZunRng& rng);
    void drop();

    ReplayFile& file() { return file_; }
    const ReplayFile& file() const { return file_; }

  private:
    ReplayFile file_;
    bool created_ = false;
    int current_ = -1;
    std::int32_t frame_ = 0;
    std::size_t fpsCursor_ = 0;
};

// Playback of one stage block (spec 7.8.4): entry k for frame k + 1, 0 past the block end,
// and the FPS of the sample after the current one [QUIRK].
class ReplayFeed {
  public:
    void start(const ReplayStage* stage);
    // The next frame's game input; also advances the FPS reading and the frame counter.
    std::uint16_t next();
    std::int32_t frame() const { return frame_; }
    int fps() const { return fps_; }
    bool fpsBad() const { return fpsBad_; }
    // The recorded entry the last next() returned (this frame's game input).
    ReplayEntry last() const { return last_; }
    // The entry recorded during the frame just fed: its flags are that frame's events (for
    // desync checks).
    ReplayEntry upcoming() const {
        return stage_ && cursor_ < stage_->entries.size() ? stage_->entries[cursor_] : ReplayEntry{};
    }
    bool pastEnd() const { return pastEnd_; }

  private:
    const ReplayStage* stage_ = nullptr;
    std::size_t cursor_ = 0;
    std::int32_t frame_ = 0;
    int fps_ = 60;
    bool fpsBad_ = false;
    bool pastEnd_ = false;
    ReplayEntry last_{};
};

}  // namespace touhou::th07
