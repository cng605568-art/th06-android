#pragma once

// Replays in the 2002 game's .rpy format (version 0x0102), from the clean-room replay spec:
//   a 0x50-byte header (magic "T6RP", shot type, difficulty, checksum, key, date, name,
//   score, slowdown, seven stage offsets), then one block per stage played: the state at
//   the stage's start (score at its end, RNG seed, point items, power, lives, bombs, rank,
//   power-item step) and the buttons, stored only when they change, ending in a sentinel.
//   Everything from 0x0F on is obfuscated with a running additive key; a checksum covers
//   the plain bytes from 0x0E on.
// Port extension: touch drag-to-move is analogue, which the button format cannot hold. Its
// samples go into an extra chunk after the last block ("T6XD" trailer at the very end), so
// the file still loads (and, without drags, plays) in the original game.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "game/Input.hpp"

namespace th06 {

inline constexpr std::uint16_t kReplayShoot = 0x0001, kReplayBomb = 0x0002, kReplayFocus = 0x0004, kReplayUp = 0x0010,
                               kReplayDown = 0x0020, kReplayLeft = 0x0040, kReplayRight = 0x0080, kReplaySkip = 0x0100;
inline constexpr std::uint16_t kReplayMask = 0x01F7;  // the recorded buttons
inline constexpr std::int32_t kReplaySentinel = 9999999;
inline constexpr int kReplayStages = 7;  // Stage 1..6, Extra
inline constexpr int kReplaySlots = 15;  // replay/th6_01.rpy .. th6_15.rpy

// The recorded buttons of an input, and an input made from recorded buttons (only the
// recorded fields change).
std::uint16_t replayButtons(const InputState& in);
void applyReplayButtons(InputState& in, std::uint16_t buttons);

// Drag positions are kept in 1/8 playfield pixels; live play uses the same rounding, so a
// replay feeds the ship exactly what it got.
inline constexpr float kReplayDragScale = 8.0f;
void quantizeDrag(InputState& in);

struct ReplayRecord {
    std::int32_t frame = 0;
    std::uint16_t buttons = 0;
};

struct ReplayDrag {
    std::int32_t frame = 0;
    bool active = false;
    bool newTouch = false;  // a new finger (the ship re-anchors)
    std::int16_t x = 0, y = 0;  // offset in 1/8 px
};

struct ReplayStage {
    bool present = false;
    std::int32_t endScore = 0;  // the score at the end of this stage
    std::uint16_t seed = 0;     // the RNG at the stage's set-up
    std::int16_t pointItems = 0;
    std::uint8_t power = 0;
    std::int8_t lives = 0, bombs = 0;
    std::uint8_t rank = 0;
    std::int8_t powerItemStep = 0;
    // Button changes, from the start record {0, 0}; the frame count closes the stream.
    std::vector<ReplayRecord> records;
    std::int32_t frames = 0;
    std::vector<ReplayDrag> drags;  // (port extension) drag changes
};

struct Replay {
    int shotType = 0;    // character * 2 + shot
    int difficulty = 0;  // 0..3, 4 = Extra
    std::string date;    // "MM/DD/YY"
    std::string name;    // 8 characters, space-padded
    std::uint32_t score = 0;
    float slowdown = 0.0f;  // %
    std::array<ReplayStage, kReplayStages> stages{};

    // Reads and checks a file (magic, checksum, version, and offsets that fit). Nothing on
    // failure (`error` says why).
    static std::optional<Replay> parse(std::span<const std::uint8_t> bytes, std::string* error = nullptr);
    // The file: offsets assigned, the key and fillers drawn from `rng` (four draws, as the
    // original), checksummed and obfuscated. The last stage gets the original's doubled
    // terminator.
    std::vector<std::uint8_t> serialize(ZunRng& rng) const;

    int firstStage() const;  // -1: none
    int lastStage() const;
    // "ReimuA ", "Normal ", ... as the replay screens show them.
    static const char* shotName(int shotType);
    static const char* difficultyName(int difficulty);
};

// Playback of one stage's stream.
class ReplayCursor {
  public:
    void start(const ReplayStage* stage);
    // The recorded input for the next gameplay pass (call once per pass); live is kept for
    // the fields a replay does not record (pause, quit ...).
    InputState next(const InputState& live);
    std::int32_t frame() const { return frame_; }
    bool finished() const { return stage_ && frame_ >= stage_->frames; }

  private:
    const ReplayStage* stage_ = nullptr;
    std::size_t record_ = 0, drag_ = 0;
    std::int32_t frame_ = 0;
    ReplayDrag current_{};
    unsigned dragId_ = 0;
};

// Recording of one stage's stream.
class ReplayRecorder {
  public:
    void start(ReplayStage* stage);
    void sample(const InputState& in);  // one gameplay pass
    void stop();                        // closes the stream (frame count)
    std::int32_t frame() const { return frame_; }

  private:
    ReplayStage* stage_ = nullptr;
    std::int32_t frame_ = 0;
    std::uint16_t last_ = 0;
    ReplayDrag lastDrag_{};
    unsigned lastDragId_ = 0;
};

}  // namespace th06
