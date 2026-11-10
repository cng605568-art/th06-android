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
constexpr std::size_t kPlstHeard = 0x208;

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
            std::memcpy(parsed.plst.data(), rec.data(), kPlstSize);
        } else if (tag("HSCR") && version == 4 && len >= 0x168) {
            HighScoreRecord r{};
            std::memcpy(r.data(), rec.data(), r.size());
            parsed.highScores.push_back(r);
        } else if (tag("LSNM") && version == 1 && len >= 0x18 && !parsed.hasLastName) {
            for (std::size_t i = 0; i < 8; ++i) parsed.lastName[i] = static_cast<char>(rec[0x0C + i]);
            parsed.lastName[8] = 0;
            parsed.hasLastName = true;
        } else if (tag("PSCR") && version == 2 && len >= 0x178) {
            const std::uint8_t shot = rec[0x174];
            if (shot < kCharacters && rec[0x175] != 0) {
                PracticeRecord& r = parsed.practice[shot];
                r.hasData = true;
                for (std::size_t i = 0; i < 45; ++i) {
                    r.plays[i] = rd32(rec, 0x0C + 4 * i);
                    r.best[i] = rd32(rec, 0xC0 + 4 * i);
                }
            }
        } else if (tag("CATK") && version == 3 && len >= 0x228) {
            const std::uint16_t card = rd16(rec, 0x0C);
            if (card < kCardCount) {
                CardRecord& c = parsed.cards[card];
                parsed.cardsTagged = true;
                c.present = true;
                c.level = rec[0x0F];
                std::memcpy(c.name.data(), rec.data() + 0x10, c.name.size());
                std::memcpy(c.owner.data(), rec.data() + 0x40, c.owner.size());
                std::memcpy(c.comment1.data(), rec.data() + 0x70, c.comment1.size());
                std::memcpy(c.comment2.data(), rec.data() + 0xB0, c.comment2.size());
                for (std::size_t i = 0; i < 13; ++i) {
                    c.best[i] = rd32(rec, 0xF0 + 4 * i);
                    c.attempts[i] = rd32(rec, 0x124 + 4 * i);
                    c.captures[i] = rd32(rec, 0x158 + 4 * i);
                    c.practiceBest[i] = rd32(rec, 0x18C + 4 * i);
                    c.practiceAttempts[i] = rd32(rec, 0x1C0 + 4 * i);
                    c.practiceCaptures[i] = rd32(rec, 0x1F4 + 4 * i);
                }
            }
        } else if (tag("FLSP") && version == 1 && len >= 0x0C + 17) {
            std::memcpy(parsed.lastWords.data(), rec.data() + 0x0C, parsed.lastWords.size());
        } else if (tag("VRSM") && version == 1 && len >= 0x1C && exeVersion) {
            const auto size = static_cast<std::int32_t>(rd32(rec, 0x14)), sum = static_cast<std::int32_t>(rd32(rec, 0x18));
            const bool debug = std::memcmp(rec.data() + 0x0C, "debug", 5) == 0;
            if (!debug && (size != exeVersion->first || sum != exeVersion->second)) return false;
        }
        if (tag("VRSM") && version == 1 && len >= 0x1C) std::memcpy(parsed.vrsm.data(), rec.data(), parsed.vrsm.size());
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

std::uint8_t CardRecord::checksum() const {
    std::uint32_t sum = 0;
    for (std::uint8_t b : name) sum += static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(b)));
    for (const auto* a : {&best, &attempts, &captures, &practiceBest, &practiceAttempts, &practiceCaptures}) {
        for (std::uint32_t v : *a) sum += v;
    }
    return static_cast<std::uint8_t>(sum & 0xFF);
}

// ---- play statistics -------------------------------------------------------------------------

std::uint32_t ScoreData::plstValue(int row, int field) const {
    if (row < 0 || row > 6 || field < 0 || field > 16) return 0;
    return rd32(plst, 0x2C + 0x44 * static_cast<std::size_t>(row) + 4 * static_cast<std::size_t>(field));
}

std::array<std::uint32_t, 4> ScoreData::plstTime(std::size_t at) const {
    return {rd32(plst, at), rd32(plst, at + 4), rd32(plst, at + 8), rd32(plst, at + 12)};
}

void ScoreData::addTime(std::size_t at, std::uint32_t ms) {
    std::array<std::uint32_t, 4> t = plstTime(at);
    t[3] += ms;
    t[2] += t[3] / 1000;
    t[3] %= 1000;
    t[1] += t[2] / 60;
    t[2] %= 60;
    t[0] += t[1] / 60;
    t[1] %= 60;
    for (std::size_t k = 0; k < 4; ++k) {
        for (int b = 0; b < 4; ++b) plst[at + 4 * k + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(t[k] >> (8 * b));
    }
}

void ScoreData::plstAdd(int row, int field, std::uint32_t v) {
    if (row < 0 || row > 6 || field < 0 || field > 16) return;
    const std::size_t at = 0x2C + 0x44 * static_cast<std::size_t>(row) + 4 * static_cast<std::size_t>(field);
    const std::uint32_t old = rd32(plst, at);
    for (int k = 0; k < 4; ++k) plst[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>((old + v) >> (8 * k));
    totalPlays = plstValue(6, Plays);
}

void ScoreData::notePlay(int difficulty, int character, bool restart, bool practicePlay) {
    for (int row : {std::clamp(difficulty, 0, 4), 6}) {
        plstAdd(row, Plays);
        plstAdd(row, PerCharacter + std::clamp(character, 0, 11));
        if (restart) plstAdd(row, Retries);
        if (practicePlay) plstAdd(row, PracticePlays);
    }
}

void ScoreData::noteContinue(int difficulty, int character) {
    for (int row : {std::clamp(difficulty, 0, 4), 6}) {
        plstAdd(row, Plays);
        plstAdd(row, PerCharacter + std::clamp(character, 0, 11));
        plstAdd(row, Continues);
    }
}

void ScoreData::markClear(int character, int difficulty, std::uint16_t bit, bool noContinue) {
    const auto d = static_cast<std::size_t>(std::clamp(difficulty, 0, 4));
    for (int r : {std::clamp(character, 0, 11), 12}) {
        ClearRecord& c = clears[static_cast<std::size_t>(r)];
        if (noContinue) c.noContinue[d] = static_cast<std::uint16_t>(c.noContinue[d] | bit);
        c.reached[d] = static_cast<std::uint16_t>(c.reached[d] | bit);
    }
}

// ---- high scores -----------------------------------------------------------------------------

namespace {

struct Node {
    std::uint32_t score;
    const HighScoreRecord* record;  // null: a placeholder
};

std::uint32_t recordScore(const HighScoreRecord& r) { return rd32(r, 0x0C); }

// Insertion (0x45A500): before the first node whose score is <= the new one.
std::size_t insertNode(std::vector<Node>& list, Node n) {
    std::size_t pos = 0;
    while (pos < list.size() && list[pos].score > n.score) ++pos;
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(pos), n);
    return pos;
}

std::vector<Node> buildList(const std::vector<HighScoreRecord>& records, int difficulty, int character) {
    std::vector<Node> list;
    for (int i = 0; i < 10; ++i) insertNode(list, Node{static_cast<std::uint32_t>(100000 - 10000 * i), nullptr});
    for (const HighScoreRecord& r : records) {
        if (r[0x14] == character && r[0x15] == difficulty) insertNode(list, Node{recordScore(r), &r});
    }
    return list;
}

}  // namespace

int ScoreData::rankOf(int difficulty, int character, std::uint32_t score) const {
    std::vector<Node> list = buildList(highScores, difficulty, character);
    return static_cast<int>(insertNode(list, Node{score, nullptr}));
}

std::array<ScoreData::ListRow, 10> ScoreData::listRows(int difficulty, int character) const {
    std::array<ListRow, 10> rows{};
    const std::vector<Node> list = buildList(highScores, difficulty, character);
    for (std::size_t i = 0; i < rows.size() && i < list.size(); ++i) rows[i] = ListRow{list[i].record, list[i].score};
    return rows;
}

std::uint32_t ScoreData::bestScore(int difficulty, int character) const {
    const std::vector<Node> list = buildList(highScores, difficulty, character);
    return list.empty() ? 100000u : std::max(list.front().score, 100000u);
}

// ---- the writer --------------------------------------------------------------------------------

std::vector<std::uint8_t> ScoreData::imageToSave() const {
    std::vector<std::uint8_t> b(kHeader, 0);
    if (image.size() >= kHeader) std::memcpy(b.data(), image.data(), kHeader);
    wr16(b, 4, 1);
    wr32(b, 8, kHeader);
    appendRecord(b, "TH8K", 0x0C, 1);
    // HSCR: the first 10 nodes of every list, real records only.
    for (int d = 0; d < 5; ++d) {
        for (int c = 0; c < kCharacters; ++c) {
            const std::vector<Node> list = buildList(highScores, d, c);
            for (std::size_t i = 0; i < 10 && i < list.size(); ++i) {
                if (!list[i].record) continue;
                const std::size_t at = b.size();
                b.insert(b.end(), list[i].record->begin(), list[i].record->end());
                std::memcpy(b.data() + at, "HSCR", 4);
                wr16(b, at + 4, 0x168);
                wr16(b, at + 6, 0x168);
                b[at + 8] = 4;
                b[at + 9] = 0;
                b[at + 0x14] = static_cast<std::uint8_t>(c);
                b[at + 0x15] = static_cast<std::uint8_t>(d);
            }
        }
    }
    for (int i = 0; i < kClearRecords; ++i) {
        const std::size_t at = b.size();
        appendRecord(b, "CLRD", 0x24, 4);
        const ClearRecord& c = clears[static_cast<std::size_t>(i)];
        for (std::size_t k = 0; k < 5; ++k) {
            wr16(b, at + 0x0C + 2 * k, c.noContinue[k]);
            wr16(b, at + 0x16 + 2 * k, c.reached[k]);
        }
        b[at + 0x20] = c.pending;
        b[at + 0x21] = static_cast<std::uint8_t>(i);
    }
    if (cardsTagged) {
        for (int i = 0; i < kCardCount; ++i) {
            const CardRecord& c = cards[static_cast<std::size_t>(i)];
            const std::size_t at = b.size();
            appendRecord(b, "CATK", 0x22C, 3);
            wr16(b, at + 0x0C, static_cast<std::uint16_t>(i));
            b[at + 0x0E] = c.checksum();
            b[at + 0x0F] = c.level;
            std::memcpy(b.data() + at + 0x10, c.name.data(), c.name.size());
            std::memcpy(b.data() + at + 0x40, c.owner.data(), c.owner.size());
            std::memcpy(b.data() + at + 0x70, c.comment1.data(), c.comment1.size());
            std::memcpy(b.data() + at + 0xB0, c.comment2.data(), c.comment2.size());
            for (std::size_t k = 0; k < 13; ++k) {
                wr32(b, at + 0xF0 + 4 * k, c.best[k]);
                wr32(b, at + 0x124 + 4 * k, c.attempts[k]);
                wr32(b, at + 0x158 + 4 * k, c.captures[k]);
                wr32(b, at + 0x18C + 4 * k, c.practiceBest[k]);
                wr32(b, at + 0x1C0 + 4 * k, c.practiceAttempts[k]);
                wr32(b, at + 0x1F4 + 4 * k, c.practiceCaptures[k]);
            }
        }
    }
    for (int i = 0; i < kCharacters; ++i) {
        const PracticeRecord& r = practice[static_cast<std::size_t>(i)];
        if (!r.hasData) continue;
        const std::size_t at = b.size();
        appendRecord(b, "PSCR", 0x178, 2);
        for (std::size_t k = 0; k < 45; ++k) {
            wr32(b, at + 0x0C + 4 * k, r.plays[k]);
            wr32(b, at + 0xC0 + 4 * k, r.best[k]);
        }
        b[at + 0x174] = static_cast<std::uint8_t>(i);
        b[at + 0x175] = 1;
    }
    std::uint8_t name[12] = {};
    for (std::size_t i = 0; i < 8; ++i) name[i] = static_cast<std::uint8_t>(lastName[i] != 0 ? lastName[i] : ' ');
    appendRecord(b, "LSNM", 0x18, 1, name);
    appendRecord(b, "FLSP", 0x20, 1, lastWords);
    {
        const std::size_t at = b.size();
        b.insert(b.end(), plst.begin(), plst.end());
        std::memcpy(b.data() + at, "PLST", 4);
        wr16(b, at + 4, static_cast<std::uint16_t>(kPlstSize));
        wr16(b, at + 6, static_cast<std::uint16_t>(kPlstSize));
        b[at + 8] = 2;
        b[at + 9] = 0;
        std::memcpy(b.data() + at + kPlstHeard, musicHeard.data(), musicHeard.size());
    }
    const std::size_t at = b.size();
    if (std::memcmp(vrsm.data(), "VRSM", 4) == 0) {
        b.insert(b.end(), vrsm.begin(), vrsm.end());  // (the PC's record carries a byte at +0x12)
    } else {
        appendRecord(b, "VRSM", 0x1C, 1);
        std::memcpy(b.data() + at + 0x0C, "0100d", 6);
        wr32(b, at + 0x14, 840704u);
        wr32(b, at + 0x18, 2724749753u);
    }
    wr32(b, 0x10, static_cast<std::uint32_t>(b.size()));
    wr32(b, 0x14, static_cast<std::uint32_t>(b.size() - kHeader));
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

// ---- Last Words (spec_flow_menus 5.3, spec_spell_practice 4) ---------------------------------

bool ScoreData::everAttempted(int card) const {
    if (card < 0 || card >= kCardCount) return false;
    const CardRecord& c = cards[static_cast<std::size_t>(card)];
    return c.attempts[12] != 0 || c.practiceAttempts[12] != 0;
}

bool ScoreData::lastWordUnlocked(int card) const {
    if (card >= 205 && card <= 221) return lastWords[static_cast<std::size_t>(card - 205)] == card;
    // (below 205 the original compares a byte before FLSP's data, a padding 0 for card 204
    // [QUIRK]: such a card opens once seen)
    return everAttempted(card);
}

void ScoreData::updateLastWords(const std::vector<std::int32_t>& bossFinal, const std::vector<std::int32_t>& normalCards) {
    auto captured = [&](int c) {
        const CardRecord& r = cards[static_cast<std::size_t>(c)];
        return r.captures[12] != 0 || r.practiceCaptures[12] != 0;
    };
    auto seen = [&](int c) { return everAttempted(c); };
    int finalB = 0, extraClears = 0;
    for (int c = 0; c < 12; ++c) {
        const ClearRecord& r = clears[static_cast<std::size_t>(c)];
        bool b = false;
        for (int d = 0; d < 4; ++d) b = b || (r.noContinue[static_cast<std::size_t>(d)] & 0x4000) != 0;
        finalB += b ? 1 : 0;
        extraClears += (r.noContinue[4] & 0x100) != 0 ? 1 : 0;
    }
    int spellMode = 0;
    for (const CardRecord& r : cards) spellMode += r.practiceCaptures[12] != 0 ? 1 : 0;
    int lastSpells = 0;
    for (const std::int32_t c : bossFinal) {
        if (c >= 0 && c < kCardCount) lastSpells += cards[static_cast<std::size_t>(c)].practiceCaptures[12] != 0 ? 1 : 0;
    }
    bool marisaNormal = !normalCards.empty();
    for (const std::int32_t c : normalCards) {
        if (c < 0 || c >= kCardCount) continue;
        const CardRecord& r = cards[static_cast<std::size_t>(c)];
        marisaNormal = marisaNormal && (r.captures[6] != 0 || r.practiceCaptures[6] != 0);
    }
    const ClearRecord& reimu = clears[4];
    bool allOthersSeen = true;
    for (int c = 205; c <= 220; ++c) allOthersSeen = allOthersSeen && seen(c);
    const bool met[17] = {
        finalB >= 2,
        finalB >= 3,
        spellMode >= 50,
        finalB >= 4,
        captured(137),
        lastSpells >= 15,
        captured(195) && seen(204) && captured(145),
        seen(208) && seen(209) && seen(210),
        seen(205) && seen(206) && seen(207) && seen(211),
        marisaNormal,
        ((reimu.noContinue[2] | reimu.noContinue[3]) & 0x4000) != 0,
        spellMode >= 120,
        finalB >= 6,
        extraClears >= 3,
        lastSpells >= 30,
        (clears[12].reached[3] & 0xC000) != 0,
        allOthersSeen,
    };
    for (int k = 0; k < 17; ++k) {
        if (met[k]) lastWords[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(205 + k);  // (never cleared)
    }
}

bool ScoreData::solosUnlocked() const {
    for (int t = 0; t < 4; ++t) {
        if (!teamHasNoContinueFinalB(t)) return false;
    }
    return true;
}

}  // namespace touhou::th08
