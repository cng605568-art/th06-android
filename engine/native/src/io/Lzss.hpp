#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace touhou::lzss {

// ZUN's LZSS as used by the PBG3 archives of the 2002 game (and PBG4, th07):
//   * MSB-first bitstream.
//   * Flag bit 1: an 8-bit literal follows.
//   * Flag bit 0: a 13-bit dictionary position follows (0 = end of stream), then a 4-bit
//     length; the match copies length + 3 bytes starting at that dictionary position.
//   * 8 KiB ring dictionary, zero-filled, write head starts at position 1.
// Writes at most out.size() bytes and returns how many were produced; a result smaller
// than out.size() means the stream ended early or was truncated.
std::size_t decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

}  // namespace touhou::lzss
