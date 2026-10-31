// PBG3: the archive format of the 2002 Embodiment of Scarlet Devil.
//
//   "PBG3"                              4 bytes
//   varint entryCount, varint tableOffset   (MSB-first bitstream starting at byte 4)
//   ... LZSS-compressed member data ...
//   table @ tableOffset (fresh bitstream), per entry:
//     varint unknown, varint unknown, varint checksum, varint offset, varint size,
//     NUL-terminated name (at most 255 chars)
//
// varint = 2-bit (byteCount - 1) followed by byteCount bytes, MSB first.
// A member's stored size is implicit: it runs to the next member's offset (or the table).
// checksum = 32-bit sum of the member's stored (compressed) bytes.
// There is no encryption in this format.

#include <algorithm>
#include <cstring>
#include <numeric>

#include "core/Log.hpp"
#include "io/Archive.hpp"
#include "io/BitReader.hpp"
#include "io/Lzss.hpp"

namespace th06 {
namespace {

constexpr std::uint32_t kMaxEntries = 65536;
constexpr std::uint64_t kMaxMemberSize = 256ull << 20;

class Pbg3Archive final : public Archive {
  public:
    bool load(std::string fileName, std::unique_ptr<MappedFile> file, std::string* error) {
        label_ = std::move(fileName);
        file_ = std::move(file);
        const auto bytes = file_->bytes();
        auto fail = [&](const std::string& why) {
            if (error) *error = label_ + ": " + why;
            return false;
        };
        if (bytes.size() < 8 || std::memcmp(bytes.data(), "PBG3", 4) != 0) {
            return fail("not a PBG3 archive");
        }

        BitReader header(bytes.subspan(4));
        const std::uint32_t count = header.readVarU32();
        const std::uint32_t tableOffset = header.readVarU32();
        if (header.overrun() || count == 0 || count > kMaxEntries || tableOffset < 5 ||
            tableOffset >= bytes.size()) {
            return fail("corrupt header");
        }

        BitReader table(bytes.subspan(tableOffset));
        entries_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ArchiveEntry e;
            table.readVarU32();  // unknown (always small)
            table.readVarU32();  // unknown
            e.key = table.readVarU32();
            e.offset = table.readVarU32();
            e.size = table.readVarU32();
            for (int c = 0; c < 255; ++c) {
                const auto ch = static_cast<char>(table.read(8));
                if (ch == '\0') break;
                e.name.push_back(ch);
            }
            if (table.overrun()) return fail("truncated file table");
            if (e.name.empty() || e.offset < 4 || e.offset >= bytes.size() ||
                e.size > kMaxMemberSize) {
                return fail("bad table entry " + std::to_string(i));
            }
            entries_.push_back(std::move(e));
        }

        // Stored sizes: each member ends where the next one (by offset) begins; the last
        // ends at the file table when the table follows the data, else at end of file.
        std::vector<std::size_t> order(entries_.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return entries_[a].offset < entries_[b].offset;
        });
        for (std::size_t k = 0; k < order.size(); ++k) {
            ArchiveEntry& e = entries_[order[k]];
            std::uint64_t end = bytes.size();
            if (tableOffset > e.offset) end = tableOffset;
            for (std::size_t j = k + 1; j < order.size(); ++j) {
                if (entries_[order[j]].offset > e.offset) {
                    end = std::min<std::uint64_t>(end, entries_[order[j]].offset);
                    break;
                }
            }
            e.storedSize = end - e.offset;
        }
        return true;
    }

    std::string_view format() const override { return "PBG3"; }

    bool extract(std::size_t index, std::vector<std::uint8_t>& out,
                 std::string* error) const override {
        if (index >= entries_.size()) {
            if (error) *error = "entry index out of range";
            return false;
        }
        const ArchiveEntry& e = entries_[index];
        const auto stored = file_->bytes().subspan(e.offset, e.storedSize);

        std::uint32_t sum = 0;
        for (const std::uint8_t b : stored) sum += b;
        if (sum != e.key) {
            log::warn("{}: checksum mismatch on {} (stored {:08x}, computed {:08x})", label_,
                      e.name, e.key, sum);
        }

        out.resize(e.size);
        const std::size_t n = lzss::decompress(stored, out);
        if (n != e.size) {
            if (error) {
                *error = label_ + "/" + e.name + ": LZSS stream produced " + std::to_string(n) +
                         " of " + std::to_string(e.size) + " bytes";
            }
            out.clear();
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<Archive> openPbg3(std::string fileName, std::unique_ptr<MappedFile> file,
                                  std::string* error) {
    auto archive = std::make_unique<Pbg3Archive>();
    if (!archive->load(std::move(fileName), std::move(file), error)) return nullptr;
    return archive;
}

}  // namespace th06
