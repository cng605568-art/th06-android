#include "th06/ShotData.hpp"

#include <cmath>
#include <cstring>

#include "core/Log.hpp"
#include "core/Sha256.hpp"
#include "io/PeImage.hpp"

namespace touhou::th06 {
namespace {

bool sane(float v, float limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

}  // namespace

bool loadShotData(std::span<const std::uint8_t> exe, ShotData& out, std::string* error, bool requireKnownVersion) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (requireKnownVersion && sha256Hex(exe) != kTh06Exe102hSha256) return fail("not version 1.02h of 東方紅魔郷.exe");
    PeImage pe;
    std::string why;
    if (!pe.parse(exe, &why)) return fail(why);

    ShotData data;
    for (int type = 0; type < 4; ++type) {
        ShotType& st = data.types[static_cast<std::size_t>(type)];
        const std::int64_t ch = pe.offsetOf(exe102h::kCharacterData + type * exe102h::kCharacterDataSize,
                                            exe102h::kCharacterDataSize);
        if (ch < 0) return fail("character table not found");
        st.speed = pe.f32(static_cast<std::size_t>(ch));
        st.speedFocus = pe.f32(static_cast<std::size_t>(ch) + 4);
        if (!sane(st.speed, 32.0f) || !sane(st.speedFocus, 32.0f) || st.speed <= 0.0f || st.speedFocus <= 0.0f) {
            return fail("character table has implausible speeds");
        }
        for (int tier = 0; tier < 9; ++tier) {
            const std::int64_t pd = pe.offsetOf(exe102h::kPowerData[type] + tier * exe102h::kPowerDataSize,
                                                exe102h::kPowerDataSize);
            if (pd < 0) return fail("power table not found");
            const std::int32_t count = pe.i32(static_cast<std::size_t>(pd));
            ShotTier& t = st.tiers[static_cast<std::size_t>(tier)];
            t.threshold = pe.i32(static_cast<std::size_t>(pd) + 4);
            const std::uint32_t bulletsVa = pe.u32(static_cast<std::size_t>(pd) + 8);
            if (count < 1 || count > 64 || t.threshold < 0 || t.threshold > 1000) return fail("power table is implausible");
            for (std::int32_t i = 0; i < count; ++i) {
                const std::int64_t b = pe.offsetOf(bulletsVa + static_cast<std::uint32_t>(i) * exe102h::kBulletDataSize,
                                                   exe102h::kBulletDataSize);
                if (b < 0) return fail("bullet table not found");
                const auto at = static_cast<std::size_t>(b);
                ShotEntry e;
                e.every = pe.i16(at);
                e.phase = pe.i16(at + 2);
                e.offsetX = pe.f32(at + 4);
                e.offsetY = pe.f32(at + 8);
                e.width = pe.f32(at + 12);
                e.height = pe.f32(at + 16);
                e.angle = pe.f32(at + 20);
                e.speed = pe.f32(at + 24);
                e.damage = pe.u16(at + 28);
                const std::uint8_t origin = exe[at + 30];
                const std::uint8_t kind = exe[at + 31];
                e.script = pe.i16(at + 32);
                e.sound = pe.i16(at + 34) >= 0;
                if (origin > 2 || kind > 3 || e.every < 1 || !sane(e.angle, 8.0f) || !sane(e.speed, 100.0f) ||
                    !sane(e.offsetX, 512.0f) || !sane(e.offsetY, 512.0f) || !sane(e.width, 1024.0f) ||
                    !sane(e.height, 1024.0f) || e.script < 0) {
                    return fail("bullet table is implausible");
                }
                e.origin = static_cast<ShotOrigin>(origin);
                e.kind = static_cast<ShotKind>(kind);
                t.entries.push_back(e);
            }
        }
    }
    data.loaded = true;
    out = std::move(data);
    log::info("Shot tables read from the game executable");
    return true;
}

}  // namespace touhou::th06
