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
    auto words = [&](std::uint32_t va, std::size_t n, auto&& put) {
        const std::int64_t a = pe.offsetOf(va, 4 * n);
        if (a < 0) return false;
        for (std::size_t i = 0; i < n; ++i) put(i, pe.u32(static_cast<std::size_t>(a) + 4 * i));
        return true;
    };
    const bool scoring =
        words(ExeTables::kTimeQuota, t.timeQuota.size(), [&](std::size_t i, std::uint32_t v) { t.timeQuota[i] = static_cast<std::int32_t>(v); }) &&
        words(ExeTables::kRankTable, 3 * t.rank.size(),
              [&](std::size_t i, std::uint32_t v) {
                  ExeTables::Rank& r = t.rank[i / 3];
                  (i % 3 == 0 ? r.start : i % 3 == 1 ? r.min : r.max) = static_cast<std::int32_t>(v);
              }) &&
        words(ExeTables::kExtendMain, t.extendMain.size(), [&](std::size_t i, std::uint32_t v) { t.extendMain[i] = static_cast<std::int32_t>(v); }) &&
        words(ExeTables::kExtendExtra, t.extendExtra.size(), [&](std::size_t i, std::uint32_t v) { t.extendExtra[i] = static_cast<std::int32_t>(v); }) &&
        words(ExeTables::kBurstColors, t.burstColors.size(), [&](std::size_t i, std::uint32_t v) { t.burstColors[i] = v; });
    if (!scoring) return fail("scoring tables not in the file");
    const bool dialogue =
        words(ExeTables::kMsgFiles, 9 * 12,
              [&](std::size_t i, std::uint32_t va) { t.msgFiles[i / 12][i % 12] = pe.cString(va, 64); }) &&
        words(ExeTables::kMsgColors, 12 * 4, [&](std::size_t i, std::uint32_t v) { t.msgColors[i / 4][i % 4] = v; }) &&
        words(ExeTables::kClearBase, 9, [&](std::size_t i, std::uint32_t v) { t.clearBase[i] = static_cast<std::int32_t>(v); });
    if (!dialogue) return fail("dialogue tables not in the file");
    t.fakeCardName = pe.cString(ExeTables::kFakeCardName, 48);
    // Player Data's names, categories and texts (optional: the viewer degrades without them).
    (void)words(ExeTables::kFullNames, 13, [&](std::size_t i, std::uint32_t va) { t.fullNames[i] = pe.cString(va, 64); });
    {
        std::array<std::uint32_t, 6> tables{}, counts{};
        if (words(ExeTables::kCategoryTables, 6, [&](std::size_t i, std::uint32_t v) { tables[i] = v; }) &&
            words(ExeTables::kCategoryCounts, 6, [&](std::size_t i, std::uint32_t v) { counts[i] = v; })) {
            for (std::size_t k = 0; k < 6; ++k) {
                if (counts[k] > 222) continue;
                std::vector<std::int32_t> ids;
                if (words(tables[k], counts[k], [&](std::size_t, std::uint32_t v) { ids.push_back(static_cast<std::int32_t>(v)); })) {
                    t.cardCategories[k] = std::move(ids);
                }
            }
        }
    }
    {
        ExeTables::DataText& d = t.dataText;
        d.running = pe.cString(ExeTables::kTextRunning, 64);
        d.playTime = pe.cString(ExeTables::kTextPlayTime, 64);
        d.playsHeader = pe.cString(ExeTables::kTextPlaysHeader, 96);
        d.playsRow = pe.cString(ExeTables::kTextPlaysRow, 64);
        d.clears = pe.cString(ExeTables::kTextClears, 96);
        d.continues = pe.cString(ExeTables::kTextContinues, 96);
        d.practice = pe.cString(ExeTables::kTextPractice, 96);
        d.spellHeader = pe.cString(ExeTables::kTextSpellHeader, 64);
        d.unknownCard = pe.cString(ExeTables::kTextUnknownCard, 16);
    }
    for (std::size_t i = 0; i < t.bombNames.size(); ++i) {
        const auto push = pe.at(ExeTables::kBombNamePushes[i], 5);
        if (push.size() != 5 || push[0] != 0x68) continue;
        const std::uint32_t va = static_cast<std::uint32_t>(push[1]) | (static_cast<std::uint32_t>(push[2]) << 8) |
                                 (static_cast<std::uint32_t>(push[3]) << 16) | (static_cast<std::uint32_t>(push[4]) << 24);
        t.bombNames[i] = pe.cString(va, 131);
    }
    return true;
}

}  // namespace touhou::th08
