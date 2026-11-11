#include "io/PeImage.hpp"

#include <cstring>

namespace touhou {

bool PeImage::parse(std::span<const std::uint8_t> bytes, std::string* error) {
    bytes_ = bytes;
    sections_.clear();
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') return fail("not a Windows executable");
    const std::uint32_t pe = u32(0x3c);
    if (pe + 24 > bytes.size() || std::memcmp(bytes.data() + pe, "PE\0\0", 4) != 0) return fail("no PE header");
    const std::uint16_t sections = u16(pe + 6);
    const std::uint16_t optionalSize = u16(pe + 20);
    const std::uint32_t optional = pe + 24;
    if (optional + optionalSize > bytes.size() || optionalSize < 32 || u16(optional) != 0x10b) {
        return fail("not a 32-bit PE image");
    }
    imageBase_ = u32(optional + 28);
    std::uint32_t table = optional + optionalSize;
    for (std::uint16_t i = 0; i < sections; ++i, table += 40) {
        if (table + 40 > bytes.size()) return fail("section table cut off");
        Section s;
        s.virtualSize = u32(table + 8);
        s.virtualAddress = u32(table + 12);
        s.rawSize = u32(table + 16);
        s.rawOffset = u32(table + 20);
        sections_.push_back(s);
    }
    return true;
}

std::int64_t PeImage::offsetOf(std::uint32_t va, std::uint32_t size) const {
    if (va < imageBase_) return -1;
    const std::uint32_t rva = va - imageBase_;
    for (const Section& s : sections_) {
        if (rva < s.virtualAddress || rva - s.virtualAddress >= s.rawSize) continue;
        const std::uint64_t off = static_cast<std::uint64_t>(s.rawOffset) + (rva - s.virtualAddress);
        if (static_cast<std::uint64_t>(rva - s.virtualAddress) + size > s.rawSize || off + size > bytes_.size()) return -1;
        return static_cast<std::int64_t>(off);
    }
    return -1;
}

std::span<const std::uint8_t> PeImage::at(std::uint32_t va, std::uint32_t size) const {
    const std::int64_t off = offsetOf(va, size);
    if (off < 0) return {};
    return bytes_.subspan(static_cast<std::size_t>(off), size);
}

std::string PeImage::cString(std::uint32_t va, std::size_t maxLength) const {
    std::string out;
    for (std::size_t i = 0; i < maxLength; ++i) {
        const std::int64_t off = offsetOf(va + static_cast<std::uint32_t>(i), 1);
        if (off < 0 || bytes_[static_cast<std::size_t>(off)] == 0) break;
        out.push_back(static_cast<char>(bytes_[static_cast<std::size_t>(off)]));
    }
    return out;
}

std::uint16_t PeImage::u16(std::size_t at) const {
    return static_cast<std::uint16_t>(bytes_[at] | bytes_[at + 1] << 8);
}

std::uint32_t PeImage::u32(std::size_t at) const {
    return static_cast<std::uint32_t>(bytes_[at]) | static_cast<std::uint32_t>(bytes_[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes_[at + 2]) << 16 | static_cast<std::uint32_t>(bytes_[at + 3]) << 24;
}

float PeImage::f32(std::size_t at) const {
    const std::uint32_t v = u32(at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace touhou
