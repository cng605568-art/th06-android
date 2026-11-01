#pragma once

// SHA-256 (FIPS 180-4). Used to recognise the exact version of the player's game
// executable before reading data from it.

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace th06 {

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);
std::string sha256Hex(std::span<const std::uint8_t> data);

}  // namespace th06
