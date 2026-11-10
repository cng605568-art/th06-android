#include "th08/ScoreFile.hpp"

#include <algorithm>
#include <cstring>

#include "io/Lzss.hpp"
#include "io/ThCrypt.hpp"

namespace touhou::th08 {
namespace {

constexpr std::size_t kHeader = 0x1C;
constexpr thcrypt::Params kScoreCrypt{0x59, 0x79, 0x100, 0xC00};
constexpr std::size_t kMaxImage = 0xA001C;  // the original's buffer
constexpr std::size_t kPlstSize = 0x228, kPlstHeard = 0x208;

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
        } else if (tag("PLST") && version == 2 && len >= kPlstSize) {
            parsed.totalPlays = rd32(rec, 0x2C + 6 * 0x44);  // the last valid PLST wins
            std::memcpy(parsed.musicHeard.data(), rec.data() + kPlstHeard, parsed.musicHeard.size());
            parsed.plstOffset = at;
        } else if (tag("PSCR") && version == 2 && len >= 0x178) {
            const std::uint8_t shot = rec[0x174];
            if (shot < kCharacters && rec[0x175] != 0) {
                PracticeRecord& r = parsed.practice[shot];
                for (std::size_t i = 0; i < 45; ++i) {
                    r.plays[i] = rd32(rec, 0x0C + 4 * i);
                    r.best[i] = rd32(rec, 0xC0 + 4 * i);
                }
            }
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

namespace {

// A record header (spec_files §5.3): tag, length twice, version, flag 0, 2 bytes padding.
void appendRecord(std::vector<std::uint8_t>& b, const char* tag, std::uint16_t len, std::uint8_t version,
                  std::span<const std::uint8_t> body = {}) {
    const std::size_t at = b.size();
    b.resize(at + len, 0);
    std::memcpy(b.data() + at, tag, 4);
    wr16(b, at + 4, len);
    wr16(b, at + 6, len);
    b[at + 8] = version;
    if (!body.empty()) std::memcpy(b.data() + at + 0x0C, body.data(), std::min<std::size_t>(body.size(), len - 0x0C));
}

}  // namespace

std::vector<std::uint8_t> freshScoreImage() {
    std::vector<std::uint8_t> b(kHeader, 0);
    wr16(b, 4, 1);
    wr32(b, 8, kHeader);
    appendRecord(b, "TH8K", 0x0C, 1);
    for (int i = 0; i < kClearRecords; ++i) {
        const std::size_t at = b.size();
        appendRecord(b, "CLRD", 0x24, 4);
        for (int k = 0; k < 10; ++k) wr16(b, at + 0x0C + 2 * static_cast<std::size_t>(k), 1);
        b[at + 0x21] = static_cast<std::uint8_t>(i);
    }
    const std::uint8_t spaces[12] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0, 0, 0, 0};
    appendRecord(b, "LSNM", 0x18, 1, spaces);
    appendRecord(b, "FLSP", 0x20, 1);
    appendRecord(b, "PLST", static_cast<std::uint16_t>(kPlstSize), 2);
    const std::size_t vrsm = b.size();
    appendRecord(b, "VRSM", 0x1C, 1);
    std::memcpy(b.data() + vrsm + 0x0C, "0100d", 6);
    wr32(b, vrsm + 0x14, 840704u);       // th08.exe 1.00d: size
    wr32(b, vrsm + 0x18, 2724749753u);   // and checksum (the values thcrap's VRSM fixer writes)
    wr32(b, 0x10, static_cast<std::uint32_t>(b.size()));
    wr32(b, 0x14, static_cast<std::uint32_t>(b.size() - kHeader));
    return b;
}

bool ScoreData::setMusicHeard(int index) {
    if (index < 0 || index >= static_cast<int>(musicHeard.size())) return false;
    auto& flag = musicHeard[static_cast<std::size_t>(index)];
    if (flag != 0) return false;
    flag = 1;
    return true;
}

std::vector<std::uint8_t> ScoreData::imageToSave() const {
    std::vector<std::uint8_t> b = image;
    std::size_t plst = plstOffset;
    if (b.size() < kHeader) {
        b = freshScoreImage();
        plst = 0;
        for (std::size_t at = kHeader; at + 12 <= b.size(); at += rd16(b, at + 4)) {
            if (std::memcmp(b.data() + at, "PLST", 4) == 0) plst = at;
        }
    } else if (plst == 0 || plst + kPlstSize > b.size()) {
        plst = b.size();  // a valid file without a PLST record: add one
        appendRecord(b, "PLST", static_cast<std::uint16_t>(kPlstSize), 2);
    }
    std::memcpy(b.data() + plst + kPlstHeard, musicHeard.data(), musicHeard.size());
    return b;
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
