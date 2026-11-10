#pragma once

// The data tables of Imperishable Night that exist only inside th08.exe (version 1.00d),
// read at run time from the player's own copy (never shipped with the port): the sound
// effects (wave file and volume per sound id, claude/th08-specs/exe_map.md §3.18) and the
// menu descriptions (spec_flow_menus.md §3.1). More tables join as their systems are ported.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace touhou::th08 {

struct ExeTables {
    static constexpr int kSoundIds = 46;
    static constexpr int kSamples = 36;

    struct Sound {
        int sample = -1;           // 0..35
        std::int16_t volume = 0;   // DirectSound volume, hundredths of a dB (<= 0)
        std::int16_t third = 0;    // pan or priority (not used yet)
    };
    std::array<Sound, kSoundIds> sounds{};
    std::array<std::string, kSamples> sampleFiles;  // "se_....wav" (looked up by base name)

    // Shift-JIS strings behind pointer tables, with each string's address (what translation
    // patches key them by). Empty when a pointer does not lead to a string.
    struct MenuText {
        std::string sjis;
        std::uint32_t address = 0;
    };
    std::array<MenuText, 9> titleText;       // the title rows' descriptions
    std::array<MenuText, 10> optionText;     // the Option rows'
    std::array<MenuText, 12> keyConfigText;  // the Key Config rows'

    // The effect table (spec_stage_gui §4.5): each effect type's etama.anm script and the
    // addresses of its update and init routines (0: none), which the effect manager maps to
    // its own behaviours.
    static constexpr int kEffectTypes = 66;
    struct Effect {
        std::int32_t script = 0;
        std::uint32_t update = 0, init = 0;
    };
    std::array<Effect, kEffectTypes> effects{};
    static constexpr std::uint32_t kEffectTable = 0x004C6D30;

    // The 21 bullet types' etama.anm scripts: bullet, fast / normal / slow spawn, cancel
    // (spec_player_bullets_items §15.2), and the enemies' 32-entry random drop cycle (item
    // types, spec_ecl_enemies §8.8a).
    static constexpr int kBulletTypes = 21;
    std::array<std::array<std::int32_t, 5>, kBulletTypes> bulletScripts{};
    std::array<std::uint8_t, 32> dropCycle{};
    static constexpr std::uint32_t kBulletScripts = 0x004B4AD8, kDropCycle = 0x004C70D8;

    // Scoring tables (spec_player_bullets_items §7.4, §12.1, §14.1): the time quota per
    // stage x 4 + difficulty (entry 36, Extra, reads on into the rank table), the rank start,
    // minimum and maximum per difficulty, the extend thresholds (main game, Extra), and the
    // Reimu Last Spell burst colours (spec_bombs_familiars §2.2).
    std::array<std::int32_t, 37> timeQuota{};
    struct Rank {
        std::int32_t start = 10, min = 8, max = 16;
    };
    std::array<Rank, 6> rank{};
    std::array<std::int32_t, 6> extendMain{};
    std::array<std::int32_t, 3> extendExtra{};
    std::array<std::uint32_t, 7> burstColors{};
    // The dialogue (spec_msg_stage_end §2-§3, §10-§11): the MSG file per stage x character,
    // the text colours per character x speaker (GDI 0x00BBGGRR), and the stage-clear base per
    // stage.
    std::array<std::array<std::string, 12>, 9> msgFiles;
    std::array<std::array<std::uint32_t, 4>, 12> msgColors{};
    std::array<std::int32_t, 9> clearBase{};
    static constexpr std::uint32_t kMsgFiles = 0x004C74C0, kMsgColors = 0x004C7180, kClearBase = 0x004C7158;
    // The fake card name ECL ex 22 shows (spec_ecl_ex_routines 7.8), Shift-JIS.
    std::string fakeCardName;
    // The bomb and Last Spell names the banner shows (spec_bomb_names §3), in Player::Spell order:
    // each routine pushes its own string, so the name is read through the push at that site
    // (opcode 0x68, the pointer at +1). Empty when the site does not look as expected.
    std::array<std::string, 17> bombNames;
    static constexpr std::array<std::uint32_t, 17> kBombNamePushes{
        0x0040C058, 0x0040C958, 0x00410C88, 0x00411028, 0x0040E3F8, 0x0040E7C8, 0x0040D479, 0x0040D9B9, 0x0040FD18,
        0x00410438, 0x0040EE6A, 0x0040F5CA, 0x00411B78, 0x00412438, 0x00413188, 0x004139D8, 0x0040D153};
    static constexpr std::uint32_t kFakeCardName = 0x004B495C;
    // Player Data (spec_result_ending §2): the full character names (12 + "all characters"),
    // the spell list's six categories (card ids; Easy, Normal, Hard, Lunatic, Extra, all), and
    // the Shift-JIS lines and formats the pages render.
    std::array<std::string, 13> fullNames;
    std::array<std::vector<std::int32_t>, 6> cardCategories;
    struct DataText {
        std::string running, playTime, playsHeader, playsRow, clears, continues, practice, spellHeader, unknownCard;
    } dataText;
    static constexpr std::uint32_t kFullNames = 0x004C7F4C, kCategoryTables = 0x004C67D0, kCategoryCounts = 0x004C67E8;
    static constexpr std::uint32_t kTextRunning = 0x004B7A9C, kTextPlayTime = 0x004B7A80, kTextPlaysHeader = 0x004B7A44,
                                   kTextPlaysRow = 0x004B7A28, kTextClears = 0x004B79FC, kTextContinues = 0x004B79D0,
                                   kTextPractice = 0x004B79A4, kTextSpellHeader = 0x004B795C, kTextUnknownCard = 0x004B77D8;
    static constexpr std::uint32_t kTimeQuota = 0x004C77F0, kRankTable = 0x004C7880, kExtendMain = 0x004C7A84,
                                   kExtendExtra = 0x004C7A9C, kBurstColors = 0x004C6100;
    static constexpr std::uint32_t kSoundTable = 0x004C8040, kSampleNames = 0x004C81B0;
    static constexpr std::uint32_t kTitleTextTable = 0x004C8640, kOptionTextTable = 0x004C8664,
                                   kKeyConfigTextTable = 0x004C8690;
};

// Reads the tables from th08.exe 1.00d (checked by SHA-256 unless `checkHash` is false, for
// tests). False with `why` on failure.
bool loadExeTables(std::span<const std::uint8_t> exe, ExeTables& out, std::string* why, bool checkHash = true);

}  // namespace touhou::th08
