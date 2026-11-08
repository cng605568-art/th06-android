#pragma once

// A minimal reader for 32-bit Windows executables (PE32): maps the game's virtual addresses
// to file offsets through the section table, so a port can read the few data tables that
// exist only inside the player's own executable (th06 1.02h: shot types; th07 1.00b: sound,
// colour, drop and bonus tables). Nothing is executed or relocated.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace touhou {

class PeImage {
  public:
    bool parse(std::span<const std::uint8_t> bytes, std::string* error);

    std::uint32_t imageBase() const { return imageBase_; }
    // File offset of `size` bytes at virtual address `va`, or -1 if they are not all backed
    // by the file.
    std::int64_t offsetOf(std::uint32_t va, std::uint32_t size) const;
    // The `size` bytes at `va` (empty if not backed by the file).
    std::span<const std::uint8_t> at(std::uint32_t va, std::uint32_t size) const;
    // A NUL-terminated string at `va`, at most `maxLength` bytes (empty if not backed).
    std::string cString(std::uint32_t va, std::size_t maxLength = 256) const;

    // Little-endian reads at a file offset (the caller checked the range).
    std::uint16_t u16(std::size_t at) const;
    std::uint32_t u32(std::size_t at) const;
    std::int16_t i16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
    std::int32_t i32(std::size_t at) const { return static_cast<std::int32_t>(u32(at)); }
    float f32(std::size_t at) const;

  private:
    struct Section {
        std::uint32_t virtualSize = 0, virtualAddress = 0, rawSize = 0, rawOffset = 0;
    };
    std::span<const std::uint8_t> bytes_;
    std::uint32_t imageBase_ = 0;
    std::vector<Section> sections_;
};

}  // namespace touhou
