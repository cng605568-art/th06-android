#include "io/Lzss.hpp"

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

}  // namespace touhou::lzss
