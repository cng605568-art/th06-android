// PBG4: the archive format of Perfect Cherry Blossom (th07.dat).
//
//   "PBG4"                    4 bytes
//   u32 entryCount            > 0
//   u32 tableOffset           absolute; the LZSS-compressed file table runs from here to EOF
//   u32 tableSize             the table's size once decompressed
//   ... LZSS-compressed member data ...
//   table (after decompression), per entry, packed:
//     NUL-terminated name (a bare file name), u32 offset, u32 size, u32 unused
//
// The LZSS is the same as PBG3's. A member's stored size runs to the next record's offset
// in table order (the last one to the table), as the game computes it. A member whose
// declared size is 0 cannot be loaded by the game, so it is left out here. Names are
// looked up case-insensitively by the Vfs, as the game does; the first of two equal names
// wins. No encryption, no checksums.

#include <cstring>
#include <unordered_set>

#include "io/Archive.hpp"
#include "io/Lzss.hpp"
#include "io/Vfs.hpp"

namespace touhou {
namespace {

constexpr std::uint32_t kMaxEntries = 65536;
constexpr std::uint32_t kMaxTableSize = 16u << 20;
constexpr std::uint64_t kMaxMemberSize = 256ull << 20;

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

class Pbg4Archive final : public Archive {
  public:
    bool load(std::string fileName, std::unique_ptr<MappedFile> file, std::string* error) {
        label_ = std::move(fileName);
        file_ = std::move(file);
        const auto bytes = file_->bytes();
        auto fail = [&](const std::string& why) {
            if (error) *error = label_ + ": " + why;
            return false;
        };
        if (bytes.size() < 16 || std::memcmp(bytes.data(), "PBG4", 4) != 0) return fail("not a PBG4 archive");
        const std::uint32_t count = le32(bytes, 4);
        const std::uint32_t tableOffset = le32(bytes, 8);
        const std::uint32_t tableSize = le32(bytes, 12);
        if (count == 0 || count > kMaxEntries || static_cast<std::int32_t>(count) <= 0 || tableOffset < 16 ||
            tableOffset >= bytes.size() || tableSize == 0 || tableSize > kMaxTableSize) {
            return fail("corrupt header");
        }

        std::vector<std::uint8_t> table(tableSize);
        if (lzss::decompress(bytes.subspan(tableOffset), table) != tableSize) return fail("truncated file table");

        struct Raw {
            std::string name;
            std::uint32_t offset, size;
        };
        std::vector<Raw> raw;
        raw.reserve(count);
        std::size_t at = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const void* nul = std::memchr(table.data() + at, 0, table.size() - at);
            if (!nul) return fail("bad table entry " + std::to_string(i));
            const std::size_t end = static_cast<const std::uint8_t*>(nul) - table.data();
            if (end + 1 + 12 > table.size()) return fail("bad table entry " + std::to_string(i));
            Raw r{std::string(reinterpret_cast<const char*>(table.data() + at), end - at), le32(table, end + 1),
                  le32(table, end + 5)};
            at = end + 1 + 12;
            raw.push_back(std::move(r));
        }

        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const Raw& r = raw[i];
            const std::uint64_t next = i + 1 < raw.size() ? raw[i + 1].offset : tableOffset;
            if (r.offset < 16 || next < r.offset || next > tableOffset) {
                return fail("member " + r.name + " lies outside the data area");
            }
            if (r.size == 0 || r.name.empty()) continue;  // the game cannot load these
            if (r.size > kMaxMemberSize) return fail("member " + r.name + " is too large");
            if (!seen.insert(Vfs::normalize(r.name)).second) continue;  // the first of equal names wins
            ArchiveEntry e;
            e.name = r.name;
            e.offset = r.offset;
            e.storedSize = next - r.offset;
            e.size = r.size;
            entries_.push_back(std::move(e));
        }
        return true;
    }

    std::string_view format() const override { return "PBG4"; }

    bool extract(std::size_t index, std::vector<std::uint8_t>& out, std::string* error) const override {
        if (index >= entries_.size()) {
            if (error) *error = "entry index out of range";
            return false;
        }
        const ArchiveEntry& e = entries_[index];
        out.resize(e.size);
        const std::size_t n = lzss::decompress(file_->bytes().subspan(e.offset, e.storedSize), out);
        if (n != e.size) {
            if (error) {
                *error = label_ + "/" + e.name + ": LZSS stream produced " + std::to_string(n) + " of " +
                         std::to_string(e.size) + " bytes";
            }
            out.clear();
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<Archive> openPbg4(std::string fileName, std::unique_ptr<MappedFile> file, std::string* error) {
    auto archive = std::make_unique<Pbg4Archive>();
    if (!archive->load(std::move(fileName), std::move(file), error)) return nullptr;
    return archive;
}

}  // namespace touhou
