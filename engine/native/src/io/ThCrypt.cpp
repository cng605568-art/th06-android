#include "io/ThCrypt.hpp"

#include <vector>

namespace touhou::thcrypt {
namespace {

// How many leading bytes the cipher handles (the short tail and an odd last byte are left
// alone; the limit is checked after each whole block).
std::size_t handledBytes(std::size_t n, const Params& p) {
    if (p.block == 0) return 0;
    std::size_t excluded = 0;
    const std::size_t tail = n % p.block;
    if (tail < p.block / 4) excluded += tail;
    if (n % 2 == 1) excluded += 1;
    return n > excluded ? n - excluded : 0;
}

template <bool Decrypt>
void run(std::span<std::uint8_t> data, const Params& p) {
    std::size_t remaining = handledBytes(data.size(), p);
    std::uint8_t k = p.key;
    std::vector<std::uint8_t> tmp;
    std::size_t at = 0, done = 0;
    while (remaining > 0 && done < p.limit) {
        const std::size_t b = remaining < p.block ? remaining : p.block;
        const std::size_t h = (b + 1) / 2;
        std::uint8_t* blk = data.data() + at;
        tmp.assign(blk, blk + b);
        // Stored byte i <-> plain position: the first half fills b-1, b-3, ...; the second
        // half fills b-2, b-4, ...
        std::size_t j = b - 1;
        for (std::size_t i = 0; i < b; ++i) {
            if (i == h) j = b - 2;
            if (Decrypt) blk[j] = static_cast<std::uint8_t>(tmp[i] ^ k);
            else blk[i] = static_cast<std::uint8_t>(tmp[j] ^ k);
            k = static_cast<std::uint8_t>(k + p.step);
            j -= 2;  // (wraps below 0 only after the last byte of a half)
        }
        at += b;
        done += b;
        remaining -= b;
    }
}

}  // namespace

void decrypt(std::span<std::uint8_t> data, const Params& p) { run<true>(data, p); }
void encrypt(std::span<std::uint8_t> data, const Params& p) { run<false>(data, p); }

}  // namespace touhou::thcrypt
