#pragma once

// Perfect Cherry Blossom's score.dat (format version 11), in the original's layout so the
// player's PC file can be read and the port's own file stays readable by the PC game.
//
// A 0x1C-byte header (key seed at 1, checksum at 2, version at 4, first-record offset at 8,
// lengths at 0x10..0x1B), then an LZSS-compressed body of records with a 12-byte header
// (tag, length, length copy, version 1, flag): TH7K marker, HSCR high-score entries, CLRD
// clear progress per shot, CATK spell-card history, PSCR practice bests, LSNM the last name
// entered, PLST play statistics, VRSM the executable stamp. Bytes 2.. are obfuscated with the
// same running-key XOR as th06's file; the checksum is the 16-bit sum of the plain bytes from
// offset 4.
//
// The game keeps the clear records, spell history, practice bests and play statistics in
// memory (ScoreData) and reads parts of the file over them at fixed moments (the supervisor
// and the result screen decide when, spec 5.1). Scores are internal units (a tenth of the
// displayed score); the displayed last digit is the continue count.

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

inline constexpr int kShots = 6;          // ReimuA .. SakuyaB
inline constexpr int kDifficulties = 6;   // Easy .. Phantasm
inline constexpr int kSpellCards = 141;
inline constexpr int kPracticeStages = 6;
inline constexpr int kPracticeDifficulties = 4;
inline constexpr std::uint8_t kCleared = 99;

// One high-score entry (HSCR).
struct ScoreEntry {
    std::uint32_t score = 0;  // internal units
    float slow = 0.0f;        // slowdown percentage of that game
    std::uint8_t shot = 0, difficulty = 0;
    std::uint8_t stage = 1;   // 1..8 reached, 99 cleared
    std::array<char, 9> name{'-', '-', '-', '-', '-', '-', '-', '-', 0};
    std::array<char, 6> date{'-', '-', '/', '-', '-', 0};
    std::int8_t continues = 0;
    bool placeholder = false;  // "DMYS": shown, never written
    bool thisRun = false;      // the entry the result screen is naming
};
using ScoreList = std::vector<ScoreEntry>;  // best first

// Clear progress of one shot (CLRD): "stages up to v reached", 99 cleared.
struct ClearRecord {
    std::array<std::uint8_t, kDifficulties> noContinue{1, 1, 1, 1, 1, 0};
    std::array<std::uint8_t, kDifficulties> reached{1, 1, 1, 1, 1, 0};
};

// The history of one spell card (CATK). Columns 0..5 are the shots, 6 the total.
struct SpellRecord {
    bool present = false;  // carries the CATK tag (only those are written)
    std::array<std::uint32_t, 7> bestBonus{};
    std::uint8_t sum = 0;  // the anti-tamper byte
    std::array<std::uint8_t, 49> name{};  // Shift-JIS, NUL terminated
    std::array<std::uint16_t, 7> attempts{}, captures{};
};

struct PracticeRecord {
    std::int32_t plays = 0;
    std::int32_t score = 0;  // internal units
};

// Hours, minutes, seconds and milliseconds with carries.
struct PlayTime {
    std::uint32_t hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
    void add(std::uint64_t ms);
    std::uint64_t totalSeconds() const { return std::uint64_t{hours} * 3600 + minutes * 60u + seconds; }
};

// PLST: rows 0..5 per difficulty, 6 the total. Every counter stops at 999999.
struct PlayStats {
    struct Row {
        std::uint32_t plays = 0;
        std::array<std::uint32_t, kShots> playsPerShot{};
        std::uint32_t retries = 0, clears = 0, continues = 0, practices = 0;
    };
    PlayTime running, play;
    std::array<Row, 7> rows{};

    static void bump(std::uint32_t& counter) {
        if (counter < 999999) ++counter;
    }
    // A play of (difficulty, shot): the difficulty's row and the total.
    void countPlay(int difficulty, int shot);
    void countRetry(int difficulty);
    void countPractice(int difficulty);
    void countContinue(int difficulty);
    void countClear(int difficulty);  // the difficulty only (the total is recomputed when shown)
    // The Player Data page's total row of clears: the sum of rows 0..5.
    std::uint32_t clearTotal() const;
};

// What the game keeps in memory between screens (spec 5.1).
struct ScoreData {
    std::array<ClearRecord, kShots> clears{};
    std::array<SpellRecord, kSpellCards> spells{};
    std::array<PracticeRecord, kShots * kPracticeStages * kPracticeDifficulties> practice{};
    PlayStats stats;

    static std::size_t practiceIndex(int shot, int stage, int difficulty) {
        return static_cast<std::size_t>((shot * kPracticeStages + stage) * kPracticeDifficulties + difficulty);
    }
    PracticeRecord& practiceAt(int shot, int stage, int difficulty) { return practice[practiceIndex(shot, stage, difficulty)]; }
    const PracticeRecord& practiceAt(int shot, int stage, int difficulty) const {
        return practice[practiceIndex(shot, stage, difficulty)];
    }

    // The fresh set-up's reset of every card to "empty" (tag set, counts 0).
    void resetSpells();

    // Unlock rules (spec 3.9).
    bool extraUnlocked() const;               // any shot cleared E/N/H/L without continuing
    bool extraShotUnlocked(int shot) const;
    bool phantasmUnlocked() const;            // any shot's Phantasm value at 99
    bool phantasmShotUnlocked(int shot) const;
    int spellsCapturedOnce() const;           // cards with a non-zero total capture count
    // At least 60 of the 141 cards captured: every shot with the Extra clear gets Phantasm.
    void phantasmCheck();
    // Selectable practice stages for (shot, difficulty): the "reached" value, 99 meaning 6.
    int practiceStages(int shot, int difficulty) const;

    // Stage s entered (not in a replay): progress becomes at least s - 1 (no-continue only
    // while no continue was used).
    void stageEntered(int shot, int difficulty, int stage, bool continued);
    // The main game's ending: returns whether this ending was seen before.
    bool endingReached(int shot, int difficulty, bool continued);
    // The same "seen before" answer without marking anything (a game that saves no records).
    bool endingSeen(int shot, int difficulty, bool continued) const;

    // Spell history in live play (spec 5.7). `name` is the card's Shift-JIS name from the ECL.
    void spellDeclared(int card, int shot, std::string_view name);
    void spellCaptured(int card, int shot, std::uint32_t bonus);
    static std::uint8_t spellSum(const SpellRecord& s);
};

// A decoded score.dat: the header and the whole uncompressed image (header + records).
struct ScoreImage {
    std::array<std::uint8_t, 0x1C> header{};
    std::vector<std::uint8_t> bytes;
};

class ScoreFile {
  public:
    static constexpr std::uint16_t kVersion = 11;
    static constexpr std::size_t kHeaderSize = 0x1C;
    static constexpr std::size_t kMaxImage = 0xA0000;

    // Validates and decodes a file (spec 5.2.3). `verFile` is th07_0100b.ver for the VRSM
    // check (empty: the check passes). nullopt: the file is ignored, as if absent.
    static std::optional<ScoreImage> decode(std::span<const std::uint8_t> file, std::span<const std::uint8_t> verFile);

    // The readers of 5.6, each walking the whole image for its records.
    static void defaultLists(std::array<ScoreList, kDifficulties * kShots>& lists);
    static void readHighScores(const ScoreImage& img, std::array<ScoreList, kDifficulties * kShots>& lists);
    static void readClears(const ScoreImage* img, std::array<ClearRecord, kShots>& clears);
    static void readSpells(const ScoreImage& img, std::array<SpellRecord, kSpellCards>& spells);
    static void readPractice(const ScoreImage* img, ScoreData& data);
    static bool readPlayStats(const ScoreImage& img, PlayStats& stats);
    // The first valid LSNM; false (and 8 spaces) without one.
    static bool readLastName(const ScoreImage* img, std::array<char, 12>& name);
    // The in-game hi-score for (difficulty, shot) from the file's entries only (at least
    // 100000), and that entry's continue count.
    static std::pair<std::uint32_t, int> hiScore(const ScoreImage* img, int difficulty, int shot);

    // Inserts before the first entry scoring <= e; returns the new entry's rank (0-based).
    static int insert(ScoreList& list, const ScoreEntry& e);
    static std::size_t listIndex(int difficulty, int shot) { return static_cast<std::size_t>(difficulty * kShots + shot); }

    struct WriteInput {
        const std::array<ScoreList, kDifficulties * kShots>* lists = nullptr;
        const ScoreData* data = nullptr;
        std::array<char, 12> lastName{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0, 0, 0, 0};
        ExeIdentity exe;
        const std::array<std::uint8_t, 0x1C>* previousHeader = nullptr;  // the last image read
    };
    // The whole file (spec 5.5), drawing the key seed and the filler (two u16 draws).
    static std::vector<std::uint8_t> encode(const WriteInput& in, ZunRng& rng);

    // The th06 codec, shared: XOR with a running key from byte 2; checksum of bytes 4.. at 2.
    static void obfuscate(std::vector<std::uint8_t>& plain);
    static bool deobfuscate(std::vector<std::uint8_t>& bytes);  // false: checksum mismatch
};

// "MM/DD" of the local date.
std::array<char, 6> todayDate();

}  // namespace touhou::th07
