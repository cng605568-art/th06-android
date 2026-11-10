#pragma once

// th08_0100d.ver, a text file inside th08.dat: lines of "<5-character version tag><one
// separator><exe size> <exe checksum>", one per released version (0009_ .. 0100d; 0100c
// twice). Its presence proves the archive is 1.00d. Unlike TH07, every line with the tag is
// tried and any match passes (claude/th08-specs/spec_files.md §3.10).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace touhou::th08 {

inline constexpr std::string_view kVersionFile = "th08_0100d.ver";
inline constexpr std::string_view kVersionTag = "0100d";

struct ExeIdentity {
    std::int32_t size = 0;
    std::int32_t checksum = 0;
    bool operator==(const ExeIdentity&) const = default;
};

// Every line whose first five characters are `tag`, in file order.
std::vector<ExeIdentity> findVersionLines(std::span<const std::uint8_t> verFile, std::string_view tag);
// True when some line with `tag` has exactly this size and checksum.
bool versionMatches(std::span<const std::uint8_t> verFile, std::string_view tag, const ExeIdentity& id);
// The executable checksum: the wrapping sum of the first floor(size / 4) - 1 little-endian
// 32-bit words (the last whole word and any tail are left out), as TH07.
std::int32_t exeChecksum(std::span<const std::uint8_t> exe);

}  // namespace touhou::th08
