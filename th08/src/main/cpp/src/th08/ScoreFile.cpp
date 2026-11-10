#include "th08/ScoreFile.hpp"

#include <cstring>

#include "io/Lzss.hpp"
#include "io/ThCrypt.hpp"

namespace touhou::th08 {
namespace {

constexpr std::size_t kHeader = 0x1C;
constexpr thcrypt::Params kScoreCrypt{0x59, 0x79, 0x100, 0xC00};
constexpr std::size_t kMaxImage = 0xA001C;  // the original's buffer

std::uint16_t rd16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }
std::uint32_t rd32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
void wr16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void wr32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint8_t rotl3(std::uint8_t k) { return static_cast<std::uint8_t>((k << 3) | (k >> 5)); }

}  // namespace

std::optional<std::vector<std::uint8_t>> decodeScoreFile(std::span<const std::uint8_t> file) {
    if (file.size() < kHeader) return std::nullopt;
    std::vector<std::uint8_t> b(file.begin(), file.end());
    thcrypt::decrypt(b, kScoreCrypt);
    std::uint8_t k = b[1];
    for (std::size_t p = 2; p < b.size(); ++p) {
        k = rotl3(k);
        const std::uint8_t plain = static_cast<std::uint8_t>(b[p] ^ k);
        b[p] = plain;
        k = static_cast<std::uint8_t>(k + plain);
    }
    std::uint16_t sum = 0;
    for (std::size_t p = 4; p < b.size(); ++p) sum = static_cast<std::uint16_t>(sum + b[p]);
    if (sum != rd16(b, 2) || rd32(b, 8) != kHeader || rd16(b, 4) != 1) return std::nullopt;
    const std::uint32_t bodySize = rd32(b, 0x14), packedSize = rd32(b, 0x18);
    if (bodySize > kMaxImage - kHeader || packedSize > b.size() - kHeader) return std::nullopt;
    std::vector<std::uint8_t> image(kHeader + bodySize);
    std::memcpy(image.data(), b.data(), kHeader);
    if (lzss::decompress(std::span(b).subspan(kHeader, packedSize), std::span(image).subspan(kHeader)) != bodySize) {
        return std::nullopt;
    }
    return image;
}

std::vector<std::uint8_t> encodeScoreFile(std::vector<std::uint8_t> image, std::uint8_t seed, std::uint8_t filler) {
    if (image.size() < kHeader) image.resize(kHeader, 0);
    const std::vector<std::uint8_t> packed = lzss::compress(std::span(image).subspan(kHeader));
    std::vector<std::uint8_t> b(image.begin(), image.begin() + kHeader);
    b.insert(b.end(), packed.begin(), packed.end());
    wr32(b, 0x08, kHeader);
    wr32(b, 0x10, static_cast<std::uint32_t>(image.size()));
    wr32(b, 0x14, static_cast<std::uint32_t>(image.size() - kHeader));
    wr32(b, 0x18, static_cast<std::uint32_t>(packed.size()));
    b[1] = seed;
    wr16(b, 4, 1);
    b[6] = filler;
    std::uint16_t sum = 0;
    for (std::size_t p = 4; p < b.size(); ++p) sum = static_cast<std::uint16_t>(sum + b[p]);
    wr16(b, 2, sum);
    std::uint8_t k = b[1];
    for (std::size_t p = 2; p < b.size(); ++p) {
        k = rotl3(k);
        const std::uint8_t plain = b[p];
        b[p] = static_cast<std::uint8_t>(plain ^ k);
        k = static_cast<std::uint8_t>(k + plain);
    }
    thcrypt::encrypt(b, kScoreCrypt);
    return b;
}

bool parseScoreImage(std::span<const std::uint8_t> image, ScoreData& out,
                     const std::optional<std::pair<std::int32_t, std::int32_t>>& exeVersion) {
    ScoreData parsed;
    bool haveKey = false;
    int lastKeyVersion = 0;
    bool clearsStopped = false;
    for (std::size_t at = kHeader; at + 12 <= image.size();) {
        const std::uint16_t len = rd16(image, at + 4);
        if (len == 0) return false;
        if (at + len > image.size()) break;
        const std::span<const std::uint8_t> rec = image.subspan(at, len);
        const std::uint8_t version = rec[8];
        auto tag = [&](const char* t) { return std::memcmp(rec.data(), t, 4) == 0; };
        if (tag("TH8K")) {
            haveKey = true;
            lastKeyVersion = version;
        } else if (tag("CLRD") && version == 4 && len >= 0x24 && !clearsStopped) {
            const std::uint8_t index = rec[0x21];
            if (index >= kClearRecords) {
                clearsStopped = true;
            } else {
                ClearRecord& c = parsed.clears[index];
                for (int d = 0; d < 5; ++d) {
                    c.noContinue[static_cast<std::size_t>(d)] = rd16(rec, 0x0C + 2 * static_cast<std::size_t>(d));
                    c.reached[static_cast<std::size_t>(d)] = rd16(rec, 0x16 + 2 * static_cast<std::size_t>(d));
                }
                c.pending = rec[0x20];
            }
        } else if (tag("PLST") && version == 2 && len >= 0x2C + 7 * 0x44) {
            parsed.totalPlays = rd32(rec, 0x2C + 6 * 0x44);  // the last valid PLST wins
        } else if (tag("FLSP") && version == 1 && len >= 0x0C + 17) {
            std::memcpy(parsed.lastWords.data(), rec.data() + 0x0C, parsed.lastWords.size());
        } else if (tag("VRSM") && version == 1 && len >= 0x1C && exeVersion) {
            const auto size = static_cast<std::int32_t>(rd32(rec, 0x14)), sum = static_cast<std::int32_t>(rd32(rec, 0x18));
            const bool debug = std::memcmp(rec.data() + 0x0C, "debug", 5) == 0;
            if (!debug && (size != exeVersion->first || sum != exeVersion->second)) return false;
        }
        at += len;
    }
    if (!haveKey || lastKeyVersion != 1) return false;
    parsed.image.assign(image.begin(), image.end());
    out = std::move(parsed);
    return true;
}

bool ScoreData::teamHasNoContinueFinalB(int team) const {
    if (team < 0 || team >= kClearRecords) return false;
    for (int d = 0; d < 4; ++d) {
        if (clears[static_cast<std::size_t>(team)].noContinue[static_cast<std::size_t>(d)] & 0x4000) return true;
    }
    return false;
}

bool ScoreData::teamHasFinalA(int team) const {
    if (team < 0 || team >= kClearRecords) return false;
    for (int d = 0; d < 4; ++d) {
        if (clears[static_cast<std::size_t>(team)].reached[static_cast<std::size_t>(d)] & 0x8000) return true;
    }
    return false;
}

bool ScoreData::extraUnlocked() const {
    for (int t = 0; t < 4; ++t) {
        if (teamHasNoContinueFinalB(t)) return true;
    }
    return false;
}

bool ScoreData::spellPracticeUnlocked() const {
    for (int t = 0; t < 4; ++t) {
        if (teamHasFinalA(t)) return true;
    }
    return false;
}

bool ScoreData::solosUnlocked() const {
    for (int t = 0; t < 4; ++t) {
        if (!teamHasNoContinueFinalB(t)) return false;
    }
    return true;
}

}  // namespace touhou::th08
