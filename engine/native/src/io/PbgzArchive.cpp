// PBGZ: the archive format of Imperishable Night (th08.dat).
//
//   "PBGZ"                    4 bytes
//   12 bytes, encrypted (key 0x1B, step 0x37, block 12, limit 0x400): three u32s,
//     entry count + 123456, table offset + 345678, decompressed table size + 567891
//   ... member streams ...
//   table: from the table offset to EOF, encrypted (key 0x3E, step 0x9B, block 0x80,
//     limit 0x400: only its first 1 KiB changes), then LZSS-compressed; per entry, packed:
//     NUL-terminated name, u32 offset, u32 size (the decoded size + 4), u32 unused
//
// A member's stored size runs to the next record's offset in table order (the last one to
// the table), as in PBG4. Decompressed (the same LZSS as PBG3/PBG4), a member is "edz", a
// type tag, then the file encrypted with that tag's parameters (th08.exe's table, below).
// The entries report the file's true size, without the 4-byte prefix (the original reports
// the prefix too).

#include <array>
#include <cstring>
#include <unordered_set>

#include "io/Archive.hpp"
#include "io/Lzss.hpp"
#include "io/ThCrypt.hpp"
#include "io/Vfs.hpp"

namespace touhou {
namespace {

constexpr std::uint32_t kMaxEntries = 65536;
constexpr std::uint32_t kMaxTableSize = 16u << 20;
constexpr std::uint64_t kMaxMemberSize = 256ull << 20;

constexpr thcrypt::Params kHeaderCrypt{0x1B, 0x37, 12, 0x400};
constexpr thcrypt::Params kTableCrypt{0x3E, 0x9B, 0x80, 0x400};

struct TypeParams {
    char tag;
    thcrypt::Params crypt;
};
// th08.exe 1.00d's member parameters. (The 'M' row's limit is 0x2800 in the program; no
// shipped member uses that tag.)
constexpr std::array<TypeParams, 8> kTypes{{
    {'M', {0x1B, 0x37, 0x40, 0x2800}},
    {'T', {0x51, 0xE9, 0x40, 0x3000}},
    {'A', {0xC1, 0x51, 0x1400, 0x2000}},
    {'J', {0x03, 0x19, 0x1400, 0x7800}},
    {'E', {0xAB, 0xCD, 0x200, 0x1000}},
    {'W', {0x12, 0x34, 0x400, 0x2800}},
    {'-', {0x35, 0x97, 0x80, 0x2800}},
    {'*', {0x99, 0x37, 0x400, 0x1000}},
}};

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

class PbgzArchive final : public Archive {
  public:
    bool load(std::string fileName, std::unique_ptr<MappedFile> file, std::string* error) {
        label_ = std::move(fileName);
        file_ = std::move(file);
        const auto bytes = file_->bytes();
        auto fail = [&](const std::string& why) {
            if (error) *error = label_ + ": " + why;
            return false;
        };
        if (bytes.size() < 16 || std::memcmp(bytes.data(), "PBGZ", 4) != 0) return fail("not a PBGZ archive");
        std::array<std::uint8_t, 12> head;
        std::memcpy(head.data(), bytes.data() + 4, head.size());
        thcrypt::decrypt(head, kHeaderCrypt);
        const std::uint32_t count = le32(head, 0) - 123456u;
        const std::uint32_t tableOffset = le32(head, 4) - 345678u;
        const std::uint32_t tableSize = le32(head, 8) - 567891u;
        if (static_cast<std::int32_t>(count) <= 0 || count > kMaxEntries || tableOffset < 16 ||
            tableOffset >= bytes.size() || tableSize == 0 || tableSize > kMaxTableSize) {
            return fail("corrupt header");
        }

        std::vector<std::uint8_t> stored(bytes.begin() + tableOffset, bytes.end());
        thcrypt::decrypt(stored, kTableCrypt);
        std::vector<std::uint8_t> table(tableSize);
        lzss::decompress(stored, table);  // (zero padding after the last record; never parsed)

        struct Raw {
            std::string name;
            std::uint32_t offset, size;
        };
        std::vector<Raw> raw;
        raw.reserve(count);
        std::size_t at = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const void* nul = at < table.size() ? std::memchr(table.data() + at, 0, table.size() - at) : nullptr;
            if (!nul) return fail("bad table entry " + std::to_string(i));
            const std::size_t end = static_cast<const std::uint8_t*>(nul) - table.data();
            if (end + 1 + 12 > table.size()) return fail("bad table entry " + std::to_string(i));
            raw.push_back({std::string(reinterpret_cast<const char*>(table.data() + at), end - at), le32(table, end + 1),
                           le32(table, end + 5)});
            at = end + 1 + 12;
        }

        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const Raw& r = raw[i];
            const std::uint64_t next = i + 1 < raw.size() ? raw[i + 1].offset : tableOffset;
            if (r.offset < 16 || next < r.offset || next > tableOffset) {
                return fail("member " + r.name + " lies outside the data area");
            }
            if (r.size <= 4 || r.name.empty()) continue;  // nothing the game could load
            if (r.size > kMaxMemberSize) return fail("member " + r.name + " is too large");
            if (!seen.insert(Vfs::normalize(r.name)).second) continue;  // the first of equal names wins
            ArchiveEntry e;
            e.name = r.name;
            e.offset = r.offset;
            e.storedSize = next - r.offset;
            e.size = r.size - 4;  // the file, without the "edz" prefix
            entries_.push_back(std::move(e));
        }
        return true;
    }

    std::string_view format() const override { return "PBGZ"; }

    bool extract(std::size_t index, std::vector<std::uint8_t>& out, std::string* error) const override {
        if (index >= entries_.size()) {
            if (error) *error = "entry index out of range";
            return false;
        }
        const ArchiveEntry& e = entries_[index];
        auto fail = [&](const std::string& why) {
            if (error) *error = label_ + "/" + e.name + ": " + why;
            out.clear();
            return false;
        };
        out.resize(e.size + 4);
        const std::size_t n = lzss::decompress(file_->bytes().subspan(e.offset, e.storedSize), out);
        if (n != out.size()) {
            return fail("LZSS stream produced " + std::to_string(n) + " of " + std::to_string(out.size()) + " bytes");
        }
        if (out[0] != 'e' || out[1] != 'd' || out[2] != 'z') return fail("no edz prefix");
        const TypeParams* type = nullptr;
        for (const TypeParams& t : kTypes) {
            if (static_cast<std::uint8_t>(t.tag) == out[3]) type = &t;
        }
        if (!type) return fail("unknown member type " + std::to_string(out[3]));
        out.erase(out.begin(), out.begin() + 4);
        thcrypt::decrypt(out, type->crypt);
        return true;
    }
};

}  // namespace

std::unique_ptr<Archive> openPbgz(std::string fileName, std::unique_ptr<MappedFile> file, std::string* error) {
    auto archive = std::make_unique<PbgzArchive>();
    if (!archive->load(std::move(fileName), std::move(file), error)) return nullptr;
    return archive;
}

}  // namespace touhou
