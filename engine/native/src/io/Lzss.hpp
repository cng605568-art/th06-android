#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

// An encoder for the same format (th07's score.dat and replays): greedy matches of 3 to 18
// bytes found through hash chains, literals otherwise, then the full terminator (flag 0 and
// position 0) padded with zero bits to a whole byte. Not byte-identical to the original's
// output (which nothing needs): any decoder of the format reads it back exactly.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> in);

}  // namespace touhou::lzss
