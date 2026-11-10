#pragma once

// The block-permutation cipher of Imperishable Night (th08): the PBGZ archive's header,
// file table and members, and later score.dat.
//
// Parameters: an 8-bit key k and step s, a block size B and a limit L. Bytes are handled in
// blocks of B from the start while bytes remain and fewer than L bytes have been handled; a
// last block shorter than B is handled at its own size. Left alone: a short tail (when
// n mod B < B / 4, those last bytes), then the last byte when n is odd, and everything past
// the limit. In a block of b bytes (h = ceil(b / 2)) the first h stored bytes hold the
// plain bytes b-1, b-3, ... and the rest hold b-2, b-4, ..., each XORed with a key that
// starts at k and grows by s per byte, carried on from block to block.

#include <cstddef>
#include <cstdint>
#include <span>

namespace touhou::thcrypt {

struct Params {
    std::uint8_t key;
    std::uint8_t step;
    std::uint32_t block;
    std::uint32_t limit;
};

// In place. decrypt(encrypt(x)) == x for every input and parameter set.
void decrypt(std::span<std::uint8_t> data, const Params& p);
void encrypt(std::span<std::uint8_t> data, const Params& p);

}  // namespace touhou::thcrypt
