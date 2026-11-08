#pragma once

// th07_0100b.ver, a text file inside th07.dat: lines of "<5-character version tag><one
// separator><exe size> <exe checksum>". Its presence proves the archive is 1.00b, and a PC
// replay's header carries the 1.00b line's size and checksum (the PC game rejects replays
// whose values do not match a line).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace touhou::th07 {

inline constexpr std::string_view kVersionFile = "th07_0100b.ver";
inline constexpr std::string_view kVersionTag = "0100b";

struct ExeIdentity {
    std::int32_t size = 0;
    std::int32_t checksum = 0;
};

// The first line whose first five characters are `tag`.
std::optional<ExeIdentity> findVersionLine(std::span<const std::uint8_t> verFile, std::string_view tag);
// The game's executable checksum: the wrapping sum of the first floor(size / 4) - 1
// little-endian 32-bit words (the last whole word and any tail are left out).
std::int32_t exeChecksum(std::span<const std::uint8_t> exe);

}  // namespace touhou::th07
