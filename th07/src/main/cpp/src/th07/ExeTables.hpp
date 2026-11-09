#pragma once

// The few data tables of Perfect Cherry Blossom that exist only inside th07.exe (version
// 1.00b), read at run time from the player's own copy (never shipped with the port):
// sound effects (sample and volume per sound id, and the sample file names), the full-power
// item chain scores, the bullet colour tables, the enemy item-drop cycle, the effect id table
// and the spell card base bonuses. The executable is identified by its SHA-256.

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace touhou::th07 {

inline constexpr const char* kTh07Exe100bSha256 = "35467eaf8dc7fc85f024f16fb2037255f151cefda33cf4867bc9122aaa2e80ca";

struct ExeTables {
    static constexpr int kSoundIds = 38;
    static constexpr int kSamples = 30;
    static constexpr int kChain = 31;
    static constexpr int kColors = 28;
    static constexpr int kDrops = 32;
    static constexpr int kSpellCards = 141;
    static constexpr int kEffects = 34;

    // The effect id table: each id's global ANM script and the routines it runs at spawn
    // (init) and every frame (update), as behaviours.
    enum class EffectInit : std::uint8_t {
        None, BigBurst, SmallBurst, OrbitMarker, RandomDirection, RandomDirectionSpeed,
        WeatherForward, WeatherVortex, WeatherBackward, WeatherSlow, WeatherFalling,
    };
    enum class EffectUpdate : std::uint8_t {
        None, Physics, Orbit, Gather60, Gather240, Burst30, EaseOutBurst, FollowPlayer, FollowCamera, Noop, Weather,
    };
    struct Effect {
        std::int32_t script = 0;
        EffectUpdate update = EffectUpdate::None;
        EffectInit init = EffectInit::None;
    };

    struct Sound {
        int sample = -1;           // 0..29
        std::int16_t volume = 0;   // DirectSound volume, hundredths of a dB (<= 0)
    };
    std::array<Sound, kSoundIds> sounds{};
    std::array<std::string, kSamples> sampleFiles;  // "data/wav/se_....wav"
    std::array<std::int32_t, kChain> fullPowerChain{};
    std::array<std::uint32_t, kColors> colorsSaturated{}, colorsPale{};
    std::array<std::uint8_t, kDrops> dropCycle{};
    std::array<std::uint32_t, kSpellCards> spellBonus{};
    std::array<Effect, kEffects> effects{};
    // The bomb names (Shift-JIS), [shot index * 2 + focused]; empty if not readable. Their
    // addresses in th07.exe 1.00b (also what translation patches key them by).
    std::array<std::string, 12> bombNames;
    static constexpr std::array<std::uint32_t, 12> kBombNameAddresses = {0x00498874, 0x0049885C, 0x0049884C, 0x00498838,
                                                                         0x00498818, 0x00498800, 0x004987D8, 0x004987BC,
                                                                         0x0049879C, 0x00498788, 0x00498768, 0x00498748};
};

// False (with a reason) when `exe` is not th07.exe 1.00b (unless requireKnownVersion is
// false, for tests) or a table is not where it should be.
bool loadExeTables(std::span<const std::uint8_t> exe, ExeTables& out, std::string* error, bool requireKnownVersion = true);

}  // namespace touhou::th07
