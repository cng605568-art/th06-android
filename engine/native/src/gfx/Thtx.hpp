#pragma once

// THTX, the texture block inside ZUN's .anm files (th07's version 2, th08's version 3):
// formats 1 A8R8G8B8, 2 A1R5G5B5, 3 R5G6B5, 4 R8G8B8, 5 A4R4G4B4, decoded into RGBA8.

#include <cstdint>
#include <span>
#include <vector>

namespace touhou {

// False on an unknown format or short data.
bool decodeThtx(std::span<const std::uint8_t> pixels, int format, int width, int height, std::vector<std::uint8_t>& rgba);

}  // namespace touhou
