#pragma once

// Imperishable Night's score.dat (claude/th08-specs/spec_files.md §5, spec_flow_menus.md §5):
// on disk the block cipher (key 0x59, step 0x79, block 0x100, limit 0xC00) over TH07's
// rolling XOR over a 0x1C-byte header and an LZSS body of tagged records (TH8K, HSCR, CLRD,
// CATK, PSCR, LSNM, FLSP, PLST, VRSM). This reads what the menus need so far: the clear
// records (the unlocks), the play statistics' total play count and Music Room flags, and the
// Last Word flags. The whole decoded image is kept, and a save writes it back with the fields
// the port changes patched in, so every record the port does not read yet is carried over.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace touhou::th08 {

inline constexpr int kCharacters = 12;      // 4 teams, then 8 solo characters
inline constexpr int kClearRecords = 13;    // + the all-characters record

// A CLRD record: per difficulty (Easy, Normal, Hard, Lunatic, Extra) a word of finished
// stages (bit k = stage index k) with bit 14 = Final B cleared and bit 15 = Final A (Extra:
// Extra) cleared - the no-continue words only while no continue was used.
struct ClearRecord {
    std::array<std::uint16_t, 5> noContinue{1, 1, 1, 1, 1};
    std::array<std::uint16_t, 5> reached{1, 1, 1, 1, 1};
    std::uint8_t pending = 0;
};

// A PSCR record: per stage slot (0-8) and difficulty (0-4) the practice play count and the
// best practice score (internal units, shown with a trailing 0), index slot x 5 + difficulty.
// Written only once it has data.
struct PracticeRecord {
    std::array<std::uint32_t, 45> plays{}, best{};
    bool hasData = false;
};

// A CATK record (spec_files §5.4): per spell card, per character (index 12 = all
// characters) the best bonus, attempts and captures, in story mode and in spell practice;
// the card's name, owner and comment lines (Shift-JIS) and the difficulty of the last play.
struct CardRecord {
    bool present = false;
    std::array<std::uint32_t, 13> best{}, attempts{}, captures{};
    std::array<std::uint32_t, 13> practiceBest{}, practiceAttempts{}, practiceCaptures{};
    std::array<std::uint8_t, 48> name{}, owner{};
    std::array<std::uint8_t, 64> comment1{}, comment2{};
    std::uint8_t level = 0;
    // The anti-tamper byte (+0x0E): the name's bytes as signed chars plus all 78 counters.
    std::uint8_t checksum() const;
};
inline constexpr int kCardCount = 222;

// An HSCR record (spec_result_ending §3.3), kept whole.
using HighScoreRecord = std::array<std::uint8_t, 0x168>;
inline constexpr std::size_t kPlstSize = 0x228;

struct ScoreData {
    std::array<ClearRecord, kClearRecords> clears{};
    std::array<PracticeRecord, kCharacters> practice{};
    std::uint32_t totalPlays = 0;            // PLST's total row
    std::array<std::uint8_t, 32> musicHeard{};  // PLST +0x208: Music Room track i has been heard
    std::array<std::uint8_t, 17> lastWords{};  // FLSP: byte k = 205 + k when Last Word 205 + k is unlocked
    std::array<CardRecord, kCardCount> cards{};  // CATK
    // The cards are written once they are tagged: the first game's set-up tags all 222 (a
    // fresh PC file has none).
    bool cardsTagged = false;
    std::vector<HighScoreRecord> highScores;  // HSCR, in file order (then the new entries)
    std::array<char, 9> lastName{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0};  // LSNM
    bool hasLastName = false;
    std::array<std::uint8_t, kPlstSize> plst{};  // the play statistics record, whole
    std::array<std::uint8_t, 0x1C> vrsm{};       // the version record as read (zero: write ours)
    std::vector<std::uint8_t> image;          // the decoded header + body (empty: no file)

    // The unlocks (spec_flow_menus §5.1): teams only, Easy-Lunatic.
    bool extraUnlocked() const;         // any team: a no-continue Final B clear
    bool spellPracticeUnlocked() const; // any team: Final A cleared (continues allowed)
    bool solosUnlocked() const;         // all four teams: a no-continue Final B clear
    bool teamHasNoContinueFinalB(int team) const;
    bool teamHasFinalA(int team) const;

    // Marks Music Room track `index` (musiccmt.txt order) as heard; true when it was not yet.
    bool setMusicHeard(int index);

    // ---- the play statistics (PLST rows: 0-4 difficulties, 6 total; spec_result_ending 2.7) ----
    enum PlstField { Plays = 0, PerCharacter = 1, Retries = 13, Clears = 14, Continues = 15, PracticePlays = 16 };
    std::uint32_t plstValue(int row, int field) const;
    void plstAdd(int row, int field, std::uint32_t v = 1);
    // The PLST times: +0x0C the running time, +0x1C the play time, each {h, m, s, ms}.
    static constexpr std::size_t kRunningTime = 0x0C, kPlayTime = 0x1C;
    std::array<std::uint32_t, 4> plstTime(std::size_t at) const;
    void addTime(std::size_t at, std::uint32_t ms);
    // A game's start (spec_flow_menus 6.2 step 9) and a continue (7.3, pass 90).
    void notePlay(int difficulty, int character, bool restart, bool practice);
    void noteContinue(int difficulty, int character);
    // CLRD words: the stage bit or an ending bit, no-continue only while no continue was used.
    void markClear(int character, int difficulty, std::uint16_t bit, bool noContinue);

    // Last Words (spec_flow_menus 5.3): the unlock bytes recomputed from the records (a met
    // condition sets its byte for good); the unlock test (0x46FD5F); "ever attempted" (story or
    // spell practice, all characters).
    void updateLastWords(const std::vector<std::int32_t>& bossFinal, const std::vector<std::int32_t>& normalCards);
    bool lastWordUnlocked(int card) const;
    bool everAttempted(int card) const;

    // ---- high scores (spec_result_ending 1.4-1.6) ----
    // The 0-based rank `score` would take in list (difficulty, character) among the records and
    // the ten placeholders (100000 down to 10000), ties going to the newer entry.
    int rankOf(int difficulty, int character, std::uint32_t score) const;
    // The list as shown: the first 10 nodes; a null entry is a placeholder with its score.
    struct ListRow {
        const HighScoreRecord* record = nullptr;
        std::uint32_t placeholderScore = 0;
    };
    std::array<ListRow, 10> listRows(int difficulty, int character) const;
    void addHighScore(const HighScoreRecord& r) { highScores.push_back(r); }
    std::uint32_t bestScore(int difficulty, int character) const;  // the in-game hi-score

    // The image to save (spec_files §5.4 order): TH8K, HSCR (the first 10 nodes of each of the
    // 60 lists), CLRD, CATK (once tagged), PSCR (with data), LSNM, FLSP, PLST, VRSM.
    std::vector<std::uint8_t> imageToSave() const;
};

// A score.dat image with the records a fresh PC install writes (spec_files §5.4-5.5): TH8K,
// the 13 default CLRD records, LSNM (eight spaces), FLSP, PLST and VRSM for th08.exe 1.00d.
std::vector<std::uint8_t> freshScoreImage();

// Decodes a score.dat file into its header + body image; nullopt when it is not a valid
// file (too short, checksum, version, header offset, LZSS).
std::optional<std::vector<std::uint8_t>> decodeScoreFile(std::span<const std::uint8_t> file);
// The other direction: `image` is a header + body whose 0x10/0x14/0x18 lengths and checksum
// are filled in here; `seed` and `filler` are the two random header bytes (1 and 6).
std::vector<std::uint8_t> encodeScoreFile(std::vector<std::uint8_t> image, std::uint8_t seed, std::uint8_t filler);

// Parses a decoded image. False (and `out` left at its defaults, as for a missing file) when a
// record has length 0, there is no TH8K record, or the last TH8K is not version 1. `exeVersion`,
// when given, is (size, checksum) of th08.exe 1.00d: a VRSM that does not match it voids the
// file, as on PC.
bool parseScoreImage(std::span<const std::uint8_t> image, ScoreData& out,
                     const std::optional<std::pair<std::int32_t, std::int32_t>>& exeVersion = std::nullopt);

}  // namespace touhou::th08
