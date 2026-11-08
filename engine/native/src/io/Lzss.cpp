#include "io/Lzss.hpp"

#include <algorithm>
#include <array>

#include "io/BitReader.hpp"

namespace touhou::lzss {

std::size_t decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    constexpr unsigned kDictSize = 0x2000;
    constexpr unsigned kDictMask = kDictSize - 1;
    constexpr unsigned kMinMatch = 3;

    std::array<std::uint8_t, kDictSize> dict{};
    unsigned head = 1;
    std::size_t written = 0;
    BitReader bits(in);

    while (written < out.size()) {
        if (bits.read(1)) {
            const auto c = static_cast<std::uint8_t>(bits.read(8));
            out[written++] = c;
            dict[head] = c;
            head = (head + 1) & kDictMask;
        } else {
            const unsigned offset = bits.read(13);
            if (offset == 0) break;  // explicit end of stream
            const unsigned length = bits.read(4) + kMinMatch;
            for (unsigned i = 0; i < length && written < out.size(); ++i) {
                const std::uint8_t c = dict[(offset + i) & kDictMask];
                out[written++] = c;
                dict[head] = c;
                head = (head + 1) & kDictMask;
            }
        }
        if (bits.overrun()) {
            // The last symbol needed bits that are not in the input: truncated data.
            // Bytes produced from padding zeros are not trustworthy; report failure.
            return written > 0 ? written - 1 : 0;
        }
    }
    return written;
}

namespace {

class BitWriter {
  public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc_ = static_cast<std::uint8_t>((acc_ << 1) | ((value >> i) & 1u));
            if (++count_ == 8) {
                out_.push_back(acc_);
                acc_ = 0;
                count_ = 0;
            }
        }
    }
    void flush() {
        if (count_ == 0) return;
        out_.push_back(static_cast<std::uint8_t>(acc_ << (8 - count_)));
        acc_ = 0;
        count_ = 0;
    }

  private:
    std::vector<std::uint8_t>& out_;
    std::uint8_t acc_ = 0;
    int count_ = 0;
};

}  // namespace

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> in) {
    constexpr std::size_t kDictSize = 0x2000;
    constexpr std::size_t kDictMask = kDictSize - 1;
    constexpr std::size_t kMinMatch = 3, kMaxMatch = 18;
    // A copy overwrites the dictionary as it goes: references stay far enough back that they
    // never read a byte the same copy has already replaced (except a self-overlap, which the
    // decoder resolves to the intended bytes).
    constexpr std::size_t kMaxDistance = kDictSize - kMaxMatch - 2;
    constexpr int kHashBits = 13;
    constexpr int kChainLimit = 256;

    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 2 + 16);
    BitWriter bits(out);
    std::vector<std::int32_t> chainHead(std::size_t{1} << kHashBits, -1), chainPrev(in.size(), -1);
    const auto hash = [&](std::size_t i) {
        const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16 | static_cast<std::uint32_t>(in[i + 1]) << 8 | in[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    const auto insert = [&](std::size_t i) {
        if (i + 2 >= in.size()) return;
        const std::uint32_t h = hash(i);
        chainPrev[i] = chainHead[h];
        chainHead[h] = static_cast<std::int32_t>(i);
    };

    std::size_t i = 0;
    while (i < in.size()) {
        std::size_t bestLen = 0, bestPos = 0;
        if (i + kMinMatch <= in.size()) {
            const std::size_t limit = std::min(kMaxMatch, in.size() - i);
            int budget = kChainLimit;
            for (std::int32_t c = chainHead[hash(i)]; c >= 0 && budget-- > 0; c = chainPrev[static_cast<std::size_t>(c)]) {
                const auto cand = static_cast<std::size_t>(c);
                if (i - cand > kMaxDistance) break;
                // Input byte j sits at dictionary position (1 + j) mod 8192; position 0 means
                // "end of stream", so it cannot be referenced.
                const std::size_t pos = (1 + cand) & kDictMask;
                if (pos == 0) continue;
                std::size_t len = 0;
                while (len < limit && in[cand + len] == in[i + len]) ++len;
                if (len > bestLen) {
                    bestLen = len;
                    bestPos = pos;
                    if (len == limit) break;
                }
            }
        }
        if (bestLen >= kMinMatch) {
            bits.put(0, 1);
            bits.put(static_cast<std::uint32_t>(bestPos), 13);
            bits.put(static_cast<std::uint32_t>(bestLen - kMinMatch), 4);
            for (std::size_t k = 0; k < bestLen; ++k) insert(i + k);
            i += bestLen;
        } else {
            bits.put(1, 1);
            bits.put(in[i], 8);
            insert(i);
            ++i;
        }
    }
    bits.put(0, 1);
    bits.put(0, 13);
    bits.flush();
    return out;
}

}  // namespace touhou::lzss
