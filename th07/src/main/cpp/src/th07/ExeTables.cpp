#include "th07/ExeTables.hpp"

#include "core/Sha256.hpp"
#include "io/PeImage.hpp"

#include <type_traits>

namespace touhou::th07 {
namespace {

// Virtual addresses in th07.exe 1.00b (image base 0x400000).
constexpr std::uint32_t kSoundTable = 0x0049EA88;     // 38 x {i32 sample, i16 volume, i16 unused}
constexpr std::uint32_t kSampleNames = 0x0049EBB8;    // 30 x u32 pointer to a C string
constexpr std::uint32_t kChainTable = 0x0049ECF8;     // 31 x i32
constexpr std::uint32_t kColorsSaturated = 0x0049EE58;  // 28 x u32
constexpr std::uint32_t kColorsPale = 0x0049EEC8;       // 28 x u32
constexpr std::uint32_t kDropCycle = 0x0049EFA0;      // 32 x u8
constexpr std::uint32_t kSpellBonus = 0x0049F1B8;     // 141 x u32

}  // namespace

bool loadExeTables(std::span<const std::uint8_t> exe, ExeTables& out, std::string* error, bool requireKnownVersion) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (requireKnownVersion && sha256Hex(exe) != kTh07Exe100bSha256) return fail("not version 1.00b of th07.exe");
    PeImage pe;
    std::string why;
    if (!pe.parse(exe, &why)) return fail(why);
    ExeTables t;

    std::int64_t at = pe.offsetOf(kSoundTable, 8 * ExeTables::kSoundIds);
    if (at < 0) return fail("sound table missing");
    for (int i = 0; i < ExeTables::kSoundIds; ++i) {
        const auto base = static_cast<std::size_t>(at) + 8 * static_cast<std::size_t>(i);
        t.sounds[i].sample = pe.i32(base);
        t.sounds[i].volume = pe.i16(base + 4);
        if (t.sounds[i].sample < 0 || t.sounds[i].sample >= ExeTables::kSamples) return fail("bad sound table");
    }
    at = pe.offsetOf(kSampleNames, 4 * ExeTables::kSamples);
    if (at < 0) return fail("sample name table missing");
    for (int i = 0; i < ExeTables::kSamples; ++i) {
        t.sampleFiles[i] = pe.cString(pe.u32(static_cast<std::size_t>(at) + 4 * static_cast<std::size_t>(i)), 64);
        if (t.sampleFiles[i].empty()) return fail("bad sample name table");
    }
    auto readU32 = [&](std::uint32_t va, auto& dst) {
        const std::int64_t off = pe.offsetOf(va, static_cast<std::uint32_t>(4 * dst.size()));
        if (off < 0) return false;
        for (std::size_t i = 0; i < dst.size(); ++i) {
            dst[i] = static_cast<std::remove_reference_t<decltype(dst[0])>>(pe.u32(static_cast<std::size_t>(off) + 4 * i));
        }
        return true;
    };
    if (!readU32(kChainTable, t.fullPowerChain)) return fail("chain table missing");
    if (!readU32(kColorsSaturated, t.colorsSaturated) || !readU32(kColorsPale, t.colorsPale)) {
        return fail("colour tables missing");
    }
    if (!readU32(kSpellBonus, t.spellBonus)) return fail("spell bonus table missing");
    const auto drops = pe.at(kDropCycle, ExeTables::kDrops);
    if (drops.empty()) return fail("drop table missing");
    for (int i = 0; i < ExeTables::kDrops; ++i) t.dropCycle[i] = drops[static_cast<std::size_t>(i)];
    out = std::move(t);
    return true;
}

}  // namespace touhou::th07
