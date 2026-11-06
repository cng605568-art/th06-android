#pragma once

// score.dat of the 2002 game: high-score tables, clear progress, spell-card history and
// practice bests, in the original's format so the player's PC file can be read and the
// port's own file stays compatible.
//
// Layout: a 20-byte header (byte 1 = key seed, checksum at 2, chunk list offset at 8, data
// end at 0x10), then chunks with a 12-byte header (tag, length, length copy, version 0x10):
//   TH6K marker, HSCR high-score entry, CLRD clear progress per shot type, CATK spell-card
//   history, PSCR practice best.
// Bytes 2.. are obfuscated with a running-key XOR (key rotated left by 3 per byte, then the
// plain byte added); the checksum is the 16-bit sum of the plain bytes from offset 4. A
// missing, short or corrupt file counts as empty: every table is ten "Nanashi " placeholders
// from 1,000,000 down to 100,000, which are never written back.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/ZunMath.hpp"

namespace touhou::th06 {

class ScoreFile {
  public:
    static constexpr int kDifficulties = 5, kShotTypes = 4, kSpells = 64, kStages = 6;
    static constexpr std::uint8_t kStageCleared = 99;

    struct Entry {
        std::uint32_t score = 0;
        std::uint8_t shotType = 0, difficulty = 0;
        std::uint8_t stage = 1;  // 1..6, 7 = ended in the Extra stage, 99 = cleared
        std::array<char, 8> name{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};  // Shift-JIS, space padded
        bool placeholder = false;  // "Nanashi" defaults: shown, never saved
        bool naming = false;       // the entry the result screen is naming
    };
    using Table = std::vector<Entry>;  // best first

    struct Clear {
        std::array<std::uint8_t, 5> noContinue{1, 1, 1, 1, 1};  // stages cleared without a continue
        std::array<std::uint8_t, 5> cleared{1, 1, 1, 1, 1};     // per difficulty; 99 = all
    };

    struct Spell {
        bool present = false;  // carries the CATK tag (written)
        std::uint32_t captureBonus = 0;
        std::uint8_t nameSum = 0;
        std::array<std::uint8_t, 5> capturedBy{};  // shot types of the last 5 captures, newest first
        std::array<std::uint8_t, 34> name{};       // Shift-JIS, NUL terminated
        std::uint16_t attempts = 0, captures = 0;
    };

    ScoreFile();

    // Reads a score.dat image. False (and empty tables) when it is missing or invalid.
    bool load(std::span<const std::uint8_t> bytes);
    // The file image: the top 10 real entries of every table, the clear records, all spell
    // records that carry the tag, practice bests that are not 0. Draws two RNG numbers (key
    // seed and filler), as the original does.
    std::vector<std::uint8_t> save(ZunRng& rng) const;

    // Ranked tables (placeholders included).
    Table& table(int difficulty, int shotType) { return tables_[index(difficulty, shotType)]; }
    const Table& table(int difficulty, int shotType) const { return tables_[index(difficulty, shotType)]; }
    // Puts `e` before the first entry scoring <= it (a new score ties above); returns its rank.
    static int insert(Table& table, const Entry& e);
    // The HUD's starting hi-score: the best saved score, at least 1,000,000.
    std::uint32_t hiScore(int difficulty, int shotType) const;

    Clear& clear(int shotType) { return clears_[static_cast<std::size_t>(shotType & 3)]; }
    const Clear& clear(int shotType) const { return clears_[static_cast<std::size_t>(shotType & 3)]; }
    // Entering stage 1..7: "cleared" becomes at least stage - 1 (also "without continues"
    // unless one was used). The ending sets 99.
    void stageEntered(int shotType, int difficulty, int stage, bool continued);
    void gameCleared(int shotType, int difficulty, bool continued);

    std::uint32_t& practice(int shotType, int stageIndex, int difficulty) {
        return practice_[static_cast<std::size_t>(((shotType & 3) * kStages + stageIndex) * 4 + (difficulty & 3))];
    }
    std::uint32_t practice(int shotType, int stageIndex, int difficulty) const {
        return practice_[static_cast<std::size_t>(((shotType & 3) * kStages + stageIndex) * 4 + (difficulty & 3))];
    }

    const Spell& spell(int i) const { return spells_[static_cast<std::size_t>(i)]; }
    Spell& spell(int i) { return spells_[static_cast<std::size_t>(i)]; }
    // A spell card declared in live play: a changed name resets its record; then one more
    // attempt (up to 9999).
    void spellDeclared(int i, std::span<const std::uint8_t> name, std::uint32_t captureBonus);
    void spellCaptured(int i, int shotType);

    bool loadedFromFile() const { return loaded_; }

    // Decoded plain bytes of a file image (nullopt if short or the checksum fails). Exposed
    // for tests.
    static std::optional<std::vector<std::uint8_t>> decode(std::span<const std::uint8_t> bytes);
    static void encode(std::vector<std::uint8_t>& plain);  // in place, key seed at byte 1

  private:
    static std::size_t index(int difficulty, int shotType) {
        return static_cast<std::size_t>((difficulty < 0 || difficulty > 4 ? 0 : difficulty) * kShotTypes + (shotType & 3));
    }
    void reset();

    std::array<Table, kDifficulties * kShotTypes> tables_;
    std::array<Clear, kShotTypes> clears_{};
    std::array<std::uint32_t, kShotTypes * kStages * 4> practice_{};
    std::array<Spell, kSpells> spells_{};
    bool loaded_ = false;
};

}  // namespace touhou::th06
