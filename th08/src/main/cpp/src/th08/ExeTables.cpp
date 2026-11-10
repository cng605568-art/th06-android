#include "th08/ExeTables.hpp"

#include "io/PeImage.hpp"
#include "th08/GameFiles.hpp"

namespace touhou::th08 {
namespace {

template <std::size_t N>
bool readTexts(const PeImage& pe, std::uint32_t table, std::array<ExeTables::MenuText, N>& out) {
    const std::int64_t at = pe.offsetOf(table, 4 * N);
    if (at < 0) return false;
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint32_t va = pe.u32(static_cast<std::size_t>(at) + 4 * i);
        out[i].address = va;
        out[i].sjis = pe.cString(va, 256);
    }
    return true;
}

}  // namespace

bool loadExeTables(std::span<const std::uint8_t> exe, ExeTables& t, std::string* why, bool checkHash) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (checkHash && !isExe100d(exe, why)) return false;
    PeImage pe;
    if (!pe.parse(exe, why)) return false;
    std::int64_t at = pe.offsetOf(ExeTables::kSoundTable, 8 * ExeTables::kSoundIds);
    if (at < 0) return fail("sound table not in the file");
    for (int i = 0; i < ExeTables::kSoundIds; ++i) {
        const std::size_t base = static_cast<std::size_t>(at) + 8 * static_cast<std::size_t>(i);
        auto& s = t.sounds[static_cast<std::size_t>(i)];
        s.sample = pe.i32(base);
        s.volume = pe.i16(base + 4);
        s.third = pe.i16(base + 6);
        if (s.sample < 0 || s.sample >= ExeTables::kSamples) return fail("bad sound table");
    }
    at = pe.offsetOf(ExeTables::kSampleNames, 4 * ExeTables::kSamples);
    if (at < 0) return fail("sample names not in the file");
    for (int i = 0; i < ExeTables::kSamples; ++i) {
        const std::uint32_t va = pe.u32(static_cast<std::size_t>(at) + 4 * static_cast<std::size_t>(i));
        t.sampleFiles[static_cast<std::size_t>(i)] = pe.cString(va, 64);
        if (t.sampleFiles[static_cast<std::size_t>(i)].empty()) return fail("bad sample name");
    }
    if (!readTexts(pe, ExeTables::kTitleTextTable, t.titleText) || !readTexts(pe, ExeTables::kOptionTextTable, t.optionText) ||
        !readTexts(pe, ExeTables::kKeyConfigTextTable, t.keyConfigText)) {
        return fail("menu text tables not in the file");
    }
    at = pe.offsetOf(ExeTables::kEffectTable, 12 * ExeTables::kEffectTypes);
    if (at < 0) return fail("effect table not in the file");
    for (int i = 0; i < ExeTables::kEffectTypes; ++i) {
        const std::size_t base = static_cast<std::size_t>(at) + 12 * static_cast<std::size_t>(i);
        auto& e = t.effects[static_cast<std::size_t>(i)];
        e.script = pe.i32(base);
        e.update = pe.u32(base + 4);
        e.init = pe.u32(base + 8);
    }
    at = pe.offsetOf(ExeTables::kBulletScripts, 20 * ExeTables::kBulletTypes);
    if (at < 0) return fail("bullet type table not in the file");
    for (int i = 0; i < ExeTables::kBulletTypes; ++i) {
        for (int k = 0; k < 5; ++k) {
            t.bulletScripts[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] =
                pe.i32(static_cast<std::size_t>(at) + 20 * static_cast<std::size_t>(i) + 4 * static_cast<std::size_t>(k));
        }
    }
    at = pe.offsetOf(ExeTables::kDropCycle, 32);
    if (at < 0) return fail("drop table not in the file");
    for (int i = 0; i < 32; ++i) t.dropCycle[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pe.u16(static_cast<std::size_t>(at) + static_cast<std::size_t>(i)) & 0xFF);
    return true;
}

}  // namespace touhou::th08
