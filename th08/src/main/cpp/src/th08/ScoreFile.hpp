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
struct PracticeRecord {
    std::array<std::uint32_t, 45> plays{}, best{};
};

struct ScoreData {
    std::array<ClearRecord, kClearRecords> clears{};
    std::array<PracticeRecord, kCharacters> practice{};
    std::uint32_t totalPlays = 0;            // PLST's total row
    std::array<std::uint8_t, 32> musicHeard{};  // PLST +0x208: Music Room track i has been heard
    std::array<std::uint8_t, 17> lastWords{};  // FLSP: byte k = 205 + k when Last Word 205 + k is unlocked
    std::vector<std::uint8_t> image;         // the decoded header + body (empty: no file)
    std::size_t plstOffset = 0;              // the PLST record the image's flags come from (0: none)

    // The unlocks (spec_flow_menus §5.1): teams only, Easy-Lunatic.
    bool extraUnlocked() const;         // any team: a no-continue Final B clear
    bool spellPracticeUnlocked() const; // any team: Final A cleared (continues allowed)
    bool solosUnlocked() const;         // all four teams: a no-continue Final B clear
    bool teamHasNoContinueFinalB(int team) const;
    bool teamHasFinalA(int team) const;

    // Marks Music Room track `index` (musiccmt.txt order) as heard; true when it was not yet.
    bool setMusicHeard(int index);
    // The image to save: the loaded one (or a fresh one, spec_files §5.4, when there was no
    // valid file) with the port's fields patched into its last PLST record.
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
