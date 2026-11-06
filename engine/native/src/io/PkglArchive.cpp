// PKGL: the archive format of Touhou Koumakyou: New Classic (2026, th06nc.exe).
// Layout as documented by the th06nc-re project (MIT, docs/archive-format.md):
//
//   "PKGL"               4 bytes, plaintext
//   u32 LE tableSize
//   entry table          tableSize bytes, XOR'd with a 16-byte repeating keystream whose
//                        seed is CRC32(file-name stem), e.g. CRC32("th06CM")
//   member payloads      each XOR'd with a keystream seeded by the member's own key;
//                        flag 1 = the result is a Zstandard frame
//
// Table records (little-endian, contiguous, ending exactly at tableSize):
//   u16 flag, u32 key, u64 realSize, u64 storedSize, u64 offset, u16 nameLen, name
//
// Keystream (SplitMix64-style):
//   st = ((seed * 0x9E3779B1 + 1) ^ (seed << 32)) + 0x9E3779B97F4A7C15
//   ks = LE64(avalanche(st)) || LE64(avalanche(st + 0x9E3779B97F4A7C15))

#include <array>
#include <cstring>

#include <zstd.h>

#include "core/Log.hpp"
#include "io/Archive.hpp"

namespace touhou {
namespace {

constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kMaxMemberSize = 1ull << 30;
constexpr std::size_t kRecordHeader = 32;

std::uint64_t avalanche(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::uint32_t crc32(std::string_view s) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : s) crc = table[(crc ^ static_cast<std::uint8_t>(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

template <class T>
T readLe(const std::uint8_t* p) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(p[i]) << (8 * i);
    return v;
}

std::string stemOf(std::string_view fileName) {
    const auto slash = fileName.find_last_of("/\\");
    if (slash != std::string_view::npos) fileName.remove_prefix(slash + 1);
    const auto dot = fileName.find_last_of('.');
    if (dot != std::string_view::npos) fileName = fileName.substr(0, dot);
    return std::string(fileName);
}

class PkglArchive final : public Archive {
  public:
    bool load(std::string fileName, std::unique_ptr<MappedFile> file, std::string* error) {
        label_ = std::move(fileName);
        file_ = std::move(file);
        const auto bytes = file_->bytes();
        if (bytes.size() < 8 || std::memcmp(bytes.data(), "PKGL", 4) != 0) {
            if (error) *error = label_ + ": not a PKGL archive";
            return false;
        }
        const std::uint32_t tableSize = readLe<std::uint32_t>(bytes.data() + 4);
        if (tableSize < kRecordHeader || 8ull + tableSize > bytes.size()) {
            if (error) *error = label_ + ": bad table size";
            return false;
        }

        // The key comes from the file name. Try the actual name first, then the seven
        // shipped names in case the player renamed a file while copying it.
        std::vector<std::string> stems{stemOf(label_)};
        for (const char* known : {"th06CM", "th06ED", "th06FN", "th06IN", "th06MD", "th06ST",
                                  "th06TL"}) {
            if (stems.front() != known) stems.emplace_back(known);
        }
        const auto encrypted = bytes.subspan(8, tableSize);
        for (const std::string& stem : stems) {
            if (parseTable(encrypted, crc32(stem), bytes.size())) {
                if (stem != stems.front()) {
                    log::info("{}: table key matched stem \"{}\" (file was renamed?)", label_,
                              stem);
                }
                return true;
            }
        }
        if (error) *error = label_ + ": entry table did not decrypt with any known key";
        return false;
    }

    std::string_view format() const override { return "PKGL"; }

    bool extract(std::size_t index, std::vector<std::uint8_t>& out,
                 std::string* error) const override {
        if (index >= entries_.size()) {
            if (error) *error = "entry index out of range";
            return false;
        }
        const ArchiveEntry& e = entries_[index];
        const auto stored = file_->bytes().subspan(e.offset, e.storedSize);

        std::vector<std::uint8_t> plain(stored.begin(), stored.end());
        const auto ks = keystream(e.key);
        for (std::size_t i = 0; i < plain.size(); ++i) plain[i] ^= ks[i & 15];

        if (e.flags == 0) {
            if (plain.size() != e.size) {
                log::warn("{}/{}: raw member is {} bytes, table says {}", label_, e.name,
                          plain.size(), e.size);
            }
            out = std::move(plain);
            return true;
        }

        out.resize(e.size);
        const std::size_t n = ZSTD_decompress(out.data(), out.size(), plain.data(), plain.size());
        if (ZSTD_isError(n) || n != e.size) {
            if (error) {
                *error = label_ + "/" + e.name + ": zstd: " +
                         (ZSTD_isError(n) ? ZSTD_getErrorName(n)
                                          : "size mismatch " + std::to_string(n));
            }
            out.clear();
            return false;
        }
        return true;
    }

    static std::array<std::uint8_t, 16> keystream(std::uint32_t seed32) {
        const std::uint64_t s = seed32;
        const std::uint64_t st = ((s * 0x9E3779B1ull + 1) ^ (s << 32)) + kGolden;
        const std::uint64_t a = avalanche(st);
        const std::uint64_t b = avalanche(st + kGolden);
        std::array<std::uint8_t, 16> ks{};
        for (int i = 0; i < 8; ++i) {
            ks[i] = static_cast<std::uint8_t>(a >> (8 * i));
            ks[8 + i] = static_cast<std::uint8_t>(b >> (8 * i));
        }
        return ks;
    }

  private:
    // Decrypts and parses the table with one candidate key. Accepts only a table that
    // parses to exactly its declared size with sane, printable entries.
    bool parseTable(std::span<const std::uint8_t> encrypted, std::uint32_t seed,
                    std::uint64_t fileSize) {
        std::vector<std::uint8_t> t(encrypted.begin(), encrypted.end());
        const auto ks = keystream(seed);
        for (std::size_t i = 0; i < t.size(); ++i) t[i] ^= ks[i & 15];

        std::vector<ArchiveEntry> parsed;
        std::size_t off = 0;
        while (off + kRecordHeader <= t.size()) {
            const std::uint8_t* p = t.data() + off;
            ArchiveEntry e;
            e.flags = readLe<std::uint16_t>(p);
            e.key = readLe<std::uint32_t>(p + 2);
            e.size = readLe<std::uint64_t>(p + 6);
            e.storedSize = readLe<std::uint64_t>(p + 14);
            e.offset = readLe<std::uint64_t>(p + 22);
            const std::uint16_t nameLen = readLe<std::uint16_t>(p + 30);
            if (e.flags > 1 || nameLen == 0 || nameLen >= 256) return false;
            if (off + kRecordHeader + nameLen > t.size()) return false;
            if (e.size > kMaxMemberSize || e.storedSize > fileSize ||
                e.offset > fileSize - e.storedSize) {
                return false;
            }
            e.name.assign(reinterpret_cast<const char*>(p + kRecordHeader), nameLen);
            for (const char c : e.name) {
                if (c < 0x20 || c > 0x7e) return false;
            }
            parsed.push_back(std::move(e));
            off += kRecordHeader + nameLen;
        }
        if (off != t.size() || parsed.empty()) return false;
        entries_ = std::move(parsed);
        return true;
    }
};

}  // namespace

std::unique_ptr<Archive> openPkgl(std::string fileName, std::unique_ptr<MappedFile> file,
                                  std::string* error) {
    auto archive = std::make_unique<PkglArchive>();
    if (!archive->load(std::move(fileName), std::move(file), error)) return nullptr;
    return archive;
}

// Exposed for unit tests (golden keystream vectors).
std::array<std::uint8_t, 16> pkglKeystreamForTests(std::uint32_t seed) {
    return PkglArchive::keystream(seed);
}
std::uint32_t pkglCrc32ForTests(std::string_view s) { return crc32(s); }

}  // namespace touhou
