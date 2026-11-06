#include "th06/ShotData.hpp"

#include <cmath>
#include <cstring>

#include "core/Log.hpp"
#include "core/Sha256.hpp"

namespace touhou::th06 {
namespace {

// Minimal PE32 reader: maps virtual addresses to file offsets through the section table.
class PeImage {
  public:
    bool parse(std::span<const std::uint8_t> bytes, std::string* error) {
        bytes_ = bytes;
        auto fail = [&](const char* why) {
            if (error) *error = why;
            return false;
        };
        if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') return fail("not a Windows executable");
        const std::uint32_t pe = u32(0x3c);
        if (pe + 24 > bytes.size() || std::memcmp(bytes.data() + pe, "PE\0\0", 4) != 0) return fail("no PE header");
        const std::uint16_t sections = u16(pe + 6);
        const std::uint16_t optionalSize = u16(pe + 20);
        const std::uint32_t optional = pe + 24;
        if (optional + optionalSize > bytes.size() || optionalSize < 32 || u16(optional) != 0x10b) {
            return fail("not a 32-bit PE image");
        }
        imageBase_ = u32(optional + 28);
        std::uint32_t table = optional + optionalSize;
        for (std::uint16_t i = 0; i < sections; ++i, table += 40) {
            if (table + 40 > bytes.size()) return fail("section table cut off");
            Section s;
            s.virtualSize = u32(table + 8);
            s.virtualAddress = u32(table + 12);
            s.rawSize = u32(table + 16);
            s.rawOffset = u32(table + 20);
            sections_.push_back(s);
        }
        return true;
    }

    // File offset of `size` bytes at virtual address `va`, or -1 if not backed by the file.
    std::int64_t offsetOf(std::uint32_t va, std::uint32_t size) const {
        if (va < imageBase_) return -1;
        const std::uint32_t rva = va - imageBase_;
        for (const Section& s : sections_) {
            if (rva < s.virtualAddress || rva - s.virtualAddress >= s.rawSize) continue;
            const std::uint64_t off = static_cast<std::uint64_t>(s.rawOffset) + (rva - s.virtualAddress);
            if (rva - s.virtualAddress + size > s.rawSize || off + size > bytes_.size()) return -1;
            return static_cast<std::int64_t>(off);
        }
        return -1;
    }

    std::uint16_t u16(std::size_t at) const {
        std::uint16_t v;
        std::memcpy(&v, bytes_.data() + at, 2);
        return v;
    }
    std::uint32_t u32(std::size_t at) const {
        std::uint32_t v;
        std::memcpy(&v, bytes_.data() + at, 4);
        return v;
    }
    std::int16_t i16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
    std::int32_t i32(std::size_t at) const { return static_cast<std::int32_t>(u32(at)); }
    float f32(std::size_t at) const {
        float v;
        std::memcpy(&v, bytes_.data() + at, 4);
        return v;
    }

  private:
    struct Section {
        std::uint32_t virtualSize = 0, virtualAddress = 0, rawSize = 0, rawOffset = 0;
    };
    std::span<const std::uint8_t> bytes_;
    std::uint32_t imageBase_ = 0;
    std::vector<Section> sections_;
};

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
