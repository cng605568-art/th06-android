#include "th07/ExeTables.hpp"

#include "core/Sha256.hpp"
#include "io/PeImage.hpp"

#include <algorithm>
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
constexpr std::uint32_t kEffectTable = 0x0049EFC0;    // 34 x {i32 script, u32 update, u32 init}

using Init = ExeTables::EffectInit;
using Update = ExeTables::EffectUpdate;
// The routine addresses of 1.00b, by behaviour.
bool effectInit(std::uint32_t va, Init& out) {
    switch (va) {
        case 0: out = Init::None; return true;
        case 0x0041A5A0: out = Init::BigBurst; return true;
        case 0x0041A370: out = Init::SmallBurst; return true;
        case 0x0041A730: out = Init::OrbitMarker; return true;
        case 0x0041AA60: out = Init::RandomDirection; return true;
        case 0x0041BEC0: out = Init::RandomDirectionSpeed; return true;
        case 0x0041B0B0: out = Init::WeatherForward; return true;
        case 0x0041B4A0: out = Init::WeatherVortex; return true;
        case 0x0041B770: out = Init::WeatherBackward; return true;
        case 0x0041B9F0: out = Init::WeatherSlow; return true;
        case 0x0041BC20: out = Init::WeatherFalling; return true;
        default: return false;
    }
}
bool effectUpdate(std::uint32_t va, Update& out) {
    switch (va) {
        case 0: out = Update::None; return true;
        case 0x0041A4F0: out = Update::Physics; return true;
        case 0x0041A750: out = Update::Orbit; return true;
        case 0x0041AAF0: out = Update::Gather60; return true;
        case 0x0041AC30: out = Update::Gather240; return true;
        case 0x0041AD10: out = Update::Burst30; return true;
        case 0x0041BFD0: out = Update::EaseOutBurst; return true;
        case 0x0041ABE0: out = Update::FollowPlayer; return true;
        case 0x0041C100: out = Update::FollowCamera; return true;
        case 0x0041C1B0: out = Update::Noop; return true;
        case 0x0041AEF0: out = Update::Weather; return true;
        default: return false;
    }
}

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
    at = pe.offsetOf(kEffectTable, 12 * ExeTables::kEffects);
    if (at < 0) return fail("effect table missing");
    for (int i = 0; i < ExeTables::kEffects; ++i) {
        const auto base = static_cast<std::size_t>(at) + 12 * static_cast<std::size_t>(i);
        t.effects[i].script = pe.i32(base);
        if (!effectUpdate(pe.u32(base + 4), t.effects[i].update) || !effectInit(pe.u32(base + 8), t.effects[i].init)) {
            return fail("unknown routine in the effect table");
        }
    }
    for (std::size_t i = 0; i < 12; ++i) t.bombNames[i] = pe.cString(ExeTables::kBombNameAddresses[i], 48);
    // The menu descriptions (optional: the menus fall back to the port's own text).
    const auto menuText = [&](std::uint32_t table, auto& dst) {
        const std::int64_t off = pe.offsetOf(table, static_cast<std::uint32_t>(4 * dst.size()));
        if (off < 0) return;
        for (std::size_t i = 0; i < dst.size(); ++i) {
            const std::uint32_t va = pe.u32(static_cast<std::size_t>(off) + 4 * i);
            dst[i].sjis = pe.cString(va, 128);
            dst[i].address = dst[i].sjis.empty() ? 0 : va;
        }
    };
    {
        // The Player Data formats and labels (optional: the page falls back to the port's text).
        ExeTables::PlayerDataText& p = t.playerData;
        const auto pair = [&](std::array<std::string, 2>& dst, std::uint32_t five, std::uint32_t six) {
            dst[0] = pe.cString(five, 96);
            dst[1] = pe.cString(six, 96);
        };
        pair(p.retry, 0x00496554, 0x00496580);
        pair(p.practice, 0x004965B0, 0x004965DC);
        pair(p.continues, 0x0049660C, 0x00496638);
        pair(p.clear, 0x00496668, 0x00496694);
        pair(p.row, 0x004966C4, 0x004966E0);
        pair(p.header, 0x00496700, 0x00496740);
        p.playTime = pe.cString(0x00496784, 64);
        p.runningTime = pe.cString(0x004967A0, 64);
        for (std::size_t s = 0; s < 6; ++s) p.rowLabels[s] = pe.cString(0x004969B0 - 20 * static_cast<std::uint32_t>(s), 20);
        p.rowLabels[6] = pe.cString(0x00496938, 20);
    }
    menuText(ExeTables::kMainMenuTextTable, t.mainMenuText);
    menuText(ExeTables::kOptionTextTable, t.optionText);
    menuText(ExeTables::kKeyConfigTextTable, t.keyConfigText);
    const auto drops = pe.at(kDropCycle, ExeTables::kDrops);
    if (drops.empty()) return fail("drop table missing");
    for (int i = 0; i < ExeTables::kDrops; ++i) t.dropCycle[i] = drops[static_cast<std::size_t>(i)];
    out = std::move(t);
    return true;
}

bool ExeTables::PlayerDataText::complete() const {
    for (const auto* pair : {&header, &row, &clear, &continues, &practice, &retry}) {
        if ((*pair)[0].empty() || (*pair)[1].empty()) return false;
    }
    if (playTime.empty() || runningTime.empty()) return false;
    for (const std::string& l : rowLabels) {
        if (l.empty()) return false;
    }
    return true;
}

std::string formatExeString(std::string_view format, std::span<const FormatArg> args) {
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if (c != '%') {
            out += c;
            continue;
        }
        std::size_t j = i + 1;
        if (j < format.size() && format[j] == '%') {
            out += '%';
            i = j;
            continue;
        }
        bool left = false, zero = false;
        for (; j < format.size() && (format[j] == '-' || format[j] == '0'); ++j) (format[j] == '-' ? left : zero) = true;
        int width = 0, precision = -1;
        for (; j < format.size() && format[j] >= '0' && format[j] <= '9'; ++j) width = std::min(width * 10 + (format[j] - '0'), 64);
        if (j < format.size() && format[j] == '.') {
            precision = 0;
            for (++j; j < format.size() && format[j] >= '0' && format[j] <= '9'; ++j) precision = std::min(precision * 10 + (format[j] - '0'), 64);
        }
        if (j >= format.size() || (format[j] != 'd' && format[j] != 's')) {
            out += c;  // not a conversion this formatter knows: copied as it is
            continue;
        }
        const FormatArg arg = next < args.size() ? args[next] : FormatArg{};
        ++next;
        std::string field;
        if (format[j] == 'd') {
            const std::int64_t v = arg.isText ? 0 : arg.number;
            std::string digits = std::to_string(v < 0 ? -v : v);
            if (precision >= 0 && static_cast<int>(digits.size()) < precision) digits.insert(0, static_cast<std::size_t>(precision) - digits.size(), '0');
            const std::string sign = v < 0 ? "-" : "";
            if (zero && !left && precision < 0 && static_cast<int>(sign.size() + digits.size()) < width) {
                digits.insert(0, static_cast<std::size_t>(width) - sign.size() - digits.size(), '0');
            }
            field = sign + digits;
        } else {
            field = std::string(arg.isText ? arg.text : std::string_view());
            if (precision >= 0 && static_cast<int>(field.size()) > precision) field.resize(static_cast<std::size_t>(precision));
        }
        if (static_cast<int>(field.size()) < width) {
            const std::string pad(static_cast<std::size_t>(width) - field.size(), ' ');
            field = left ? field + pad : pad + field;
        }
        out += field;
        i = j;
    }
    return out;
}

}  // namespace touhou::th07
