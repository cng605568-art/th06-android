#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace th06 {

// MSB-first bit reader, as used by ZUN's PBG3 tables and LZSS streams.
// Reading past the end yields zero bits and sets overrun() (mirrors thtk's behaviour of
// reading zeros at EOF, while letting callers detect truncated data).
class BitReader {
  public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    // Reads 1..32 bits.
    std::uint32_t read(unsigned bits) {
        if (count_ < bits) refill();
        count_ -= bits;
        consumedBits_ += bits;
        const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
        return static_cast<std::uint32_t>((buffer_ >> count_) & mask);
    }

    // ZUN's variable-length integer: 2-bit byte count minus one, then that many bytes.
    std::uint32_t readVarU32() {
        const unsigned bytes = read(2) + 1;
        return read(bytes * 8);
    }

    bool overrun() const { return consumedBits_ > data_.size() * 8; }
    std::uint64_t consumedBits() const { return consumedBits_; }

  private:
    void refill() {
        while (count_ <= 56) {
            const std::uint8_t byte = pos_ < data_.size() ? data_[pos_] : 0;
            ++pos_;
            buffer_ = (buffer_ << 8) | byte;
            count_ += 8;
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;  // valid bits in the low end of buffer_
    std::uint64_t consumedBits_ = 0;
};

}  // namespace th06
