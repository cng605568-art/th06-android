#include "th07/ScoreFile.hpp"

#include <algorithm>
#include <ctime>
#include <cstring>
#include <functional>

#include "io/Lzss.hpp"

namespace touhou::th07 {

namespace {

constexpr std::size_t kRecordHeader = 0x0C;
constexpr std::uint16_t kLenTh7k = 0x0C, kLenHscr = 0x28, kLenClrd = 0x1C, kLenCatk = 0x78, kLenPscr = 0x18;
constexpr std::uint16_t kLenLsnm = 0x18, kLenPlst = 0x160, kLenVrsm = 0x1C;
constexpr std::uint8_t kRecordVersion = 1;

std::uint16_t rd16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}
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
float rdF(std::span<const std::uint8_t> b, std::size_t at) {
    const std::uint32_t v = rd32(b, at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void wrF(std::vector<std::uint8_t>& b, std::size_t at, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    wr32(b, at, v);
}

bool tagIs(std::span<const std::uint8_t> b, std::size_t at, const char* tag) { return std::memcmp(b.data() + at, tag, 4) == 0; }

// Walks the records of an image: fn(offset, length) returns false to stop. A record whose
// length runs past the image ends the walk (the original trusts the lengths).
void walk(std::span<const std::uint8_t> img, const std::function<bool(std::size_t, std::uint16_t)>& fn) {
    const std::size_t end = img.size();
    std::size_t at = ScoreFile::kHeaderSize;
    while (at + kRecordHeader <= end) {
        const std::uint16_t len = rd16(img, at + 4);
        if (len < kRecordHeader || at + len > end) return;
        if (!fn(at, len)) return;
        at += len;
    }
}

// A record of `tag` and at least `len` bytes with version 1.
bool record(std::span<const std::uint8_t> b, std::size_t at, std::uint16_t have, const char* tag, std::uint16_t len) {
    return tagIs(b, at, tag) && have >= len && b[at + 8] == kRecordVersion;
}

void recordHeader(std::vector<std::uint8_t>& out, const char* tag, std::uint16_t len) {
    const std::size_t at = out.size();
    out.resize(at + len, 0);
    std::memcpy(out.data() + at, tag, 4);
    wr16(out, at + 4, len);
    wr16(out, at + 6, len);
    out[at + 8] = kRecordVersion;
}

void writeTime(std::vector<std::uint8_t>& b, std::size_t at, const PlayTime& t) {
    wr32(b, at, t.hours);
    wr32(b, at + 4, t.minutes);
    wr32(b, at + 8, t.seconds);
    wr32(b, at + 12, t.milliseconds);
}
PlayTime readTime(std::span<const std::uint8_t> b, std::size_t at) {
    return {rd32(b, at), rd32(b, at + 4), rd32(b, at + 8), rd32(b, at + 12)};
}

bool verCheck(std::span<const std::uint8_t> verFile, std::string_view version, std::int32_t size, std::int32_t checksum) {
    if (verFile.empty() || version == "debug") return true;
    const auto line = findVersionLine(verFile, version);
    return line && line->size == size && line->checksum == checksum;
}

}  // namespace

// ---- PlayTime and PlayStats -----------------------------------------------------------------

void PlayTime::add(std::uint64_t ms) {
    std::uint64_t m = milliseconds + ms;
    std::uint64_t s = seconds + m / 1000;
    milliseconds = static_cast<std::uint32_t>(m % 1000);
    std::uint64_t mi = minutes + s / 60;
    seconds = static_cast<std::uint32_t>(s % 60);
    hours += static_cast<std::uint32_t>(mi / 60);
    minutes = static_cast<std::uint32_t>(mi % 60);
}

void PlayStats::countPlay(int difficulty, int shot) {
    for (int r : {difficulty, 6}) {
        Row& row = rows[static_cast<std::size_t>(std::clamp(r, 0, 6))];
        bump(row.plays);
        bump(row.playsPerShot[static_cast<std::size_t>(std::clamp(shot, 0, kShots - 1))]);
    }
}
void PlayStats::countRetry(int difficulty) {
    for (int r : {difficulty, 6}) bump(rows[static_cast<std::size_t>(std::clamp(r, 0, 6))].retries);
}
void PlayStats::countPractice(int difficulty) {
    for (int r : {difficulty, 6}) bump(rows[static_cast<std::size_t>(std::clamp(r, 0, 6))].practices);
}
void PlayStats::countContinue(int difficulty) {
    for (int r : {difficulty, 6}) bump(rows[static_cast<std::size_t>(std::clamp(r, 0, 6))].continues);
}
void PlayStats::countClear(int difficulty) { bump(rows[static_cast<std::size_t>(std::clamp(difficulty, 0, 5))].clears); }
std::uint32_t PlayStats::clearTotal() const {
    std::uint32_t t = 0;
    for (int r = 0; r < 6; ++r) t += rows[static_cast<std::size_t>(r)].clears;
    return t;
}

// ---- ScoreData ------------------------------------------------------------------------------

void ScoreData::resetSpells() {
    for (SpellRecord& s : spells) {
        s = SpellRecord{};
        s.present = true;
    }
}

bool ScoreData::extraUnlocked() const {
    for (int s = 0; s < kShots; ++s) {
        if (extraShotUnlocked(s)) return true;
    }
    return false;
}
bool ScoreData::extraShotUnlocked(int shot) const {
    const ClearRecord& c = clears[static_cast<std::size_t>(shot)];
    for (int d = 0; d < 4; ++d) {
        if (c.noContinue[static_cast<std::size_t>(d)] == kCleared) return true;
    }
    return false;
}
bool ScoreData::phantasmUnlocked() const {
    for (int s = 0; s < kShots; ++s) {
        if (phantasmShotUnlocked(s)) return true;
    }
    return false;
}
bool ScoreData::phantasmShotUnlocked(int shot) const { return clears[static_cast<std::size_t>(shot)].noContinue[5] == kCleared; }
int ScoreData::spellsCapturedOnce() const {
    int n = 0;
    for (const SpellRecord& s : spells) n += s.captures[6] != 0 ? 1 : 0;
    return n;
}
void ScoreData::phantasmCheck() {
    if (spellsCapturedOnce() < 60) return;
    for (ClearRecord& c : clears) {
        if (c.noContinue[4] == kCleared) c.noContinue[5] = kCleared;
    }
}
int ScoreData::practiceStages(int shot, int difficulty) const {
    const int v = clears[static_cast<std::size_t>(std::clamp(shot, 0, kShots - 1))].reached[static_cast<std::size_t>(std::clamp(difficulty, 0, 5))];
    return v == kCleared ? 6 : std::min(v, 6);
}

void ScoreData::stageEntered(int shot, int difficulty, int stage, bool continued) {
    if (shot < 0 || shot >= kShots || difficulty < 0 || difficulty >= kDifficulties || stage < 1) return;
    ClearRecord& c = clears[static_cast<std::size_t>(shot)];
    const auto v = static_cast<std::uint8_t>(stage - 1);
    std::uint8_t& nc = c.noContinue[static_cast<std::size_t>(difficulty)];
    std::uint8_t& r = c.reached[static_cast<std::size_t>(difficulty)];
    if (!continued && nc < v) nc = v;
    if (r < v) r = v;
}

bool ScoreData::endingReached(int shot, int difficulty, bool continued) {
    ClearRecord& c = clears[static_cast<std::size_t>(std::clamp(shot, 0, kShots - 1))];
    const auto d = static_cast<std::size_t>(std::clamp(difficulty, 0, 5));
    bool seen;
    if (!continued) {
        seen = c.noContinue[d] == kCleared;
        c.noContinue[d] = kCleared;
    } else {
        seen = c.reached[d] == kCleared;
    }
    c.reached[d] = kCleared;
    return seen;
}

std::uint8_t ScoreData::spellSum(const SpellRecord& s) {
    std::uint8_t sum = 0;
    for (std::uint8_t c : s.name) {
        if (c == 0) break;
        sum = static_cast<std::uint8_t>(sum + c);
    }
    for (int i = 0; i < 7; ++i) {
        sum = static_cast<std::uint8_t>(sum + s.captures[static_cast<std::size_t>(i)] + s.attempts[static_cast<std::size_t>(i)] +
                                        s.bestBonus[static_cast<std::size_t>(i)]);
    }
    return sum;
}

void ScoreData::spellDeclared(int card, int shot, std::string_view name) {
    if (card < 0 || card >= kSpellCards || shot < 0 || shot >= kShots) return;
    SpellRecord& s = spells[static_cast<std::size_t>(card)];
    s.present = true;
    s.name.fill(0);
    std::memcpy(s.name.data(), name.data(), std::min(name.size(), s.name.size() - 1));
    if (spellSum(s) != s.sum) {
        s.bestBonus.fill(0);
        s.attempts.fill(0);
        s.captures.fill(0);
    }
    for (int c : {shot, 6}) {
        std::uint16_t& a = s.attempts[static_cast<std::size_t>(c)];
        if (a < 9999) ++a;
    }
    s.sum = spellSum(s);
}

void ScoreData::spellCaptured(int card, int shot, std::uint32_t bonus) {
    if (card < 0 || card >= kSpellCards || shot < 0 || shot >= kShots) return;
    SpellRecord& s = spells[static_cast<std::size_t>(card)];
    if (spellSum(s) != s.sum) {
        s.bestBonus.fill(0);
        s.attempts.fill(0);
        s.captures.fill(0);
    }
    for (int c : {shot, 6}) {
        std::uint32_t& b = s.bestBonus[static_cast<std::size_t>(c)];
        if (b < bonus) b = bonus;
        std::uint16_t& n = s.captures[static_cast<std::size_t>(c)];
        if (n < 9999) ++n;
    }
    s.sum = spellSum(s);
}

// ---- the file -------------------------------------------------------------------------------

void ScoreFile::obfuscate(std::vector<std::uint8_t>& b) {
    std::uint16_t sum = 0;
    for (std::size_t i = 4; i < b.size(); ++i) sum = static_cast<std::uint16_t>(sum + b[i]);
    wr16(b, 2, sum);
    std::uint8_t k = b[1];
    for (std::size_t i = 2; i < b.size(); ++i) {
        k = static_cast<std::uint8_t>((k << 3) | (k >> 5));
        const std::uint8_t p = b[i];
        b[i] = static_cast<std::uint8_t>(p ^ k);
        k = static_cast<std::uint8_t>(k + p);
    }
}

bool ScoreFile::deobfuscate(std::vector<std::uint8_t>& b) {
    if (b.size() < 4) return false;
    std::uint8_t k = b[1];
    for (std::size_t i = 2; i < b.size(); ++i) {
        k = static_cast<std::uint8_t>((k << 3) | (k >> 5));
        b[i] = static_cast<std::uint8_t>(b[i] ^ k);
        k = static_cast<std::uint8_t>(k + b[i]);
    }
    std::uint16_t sum = 0;
    for (std::size_t i = 4; i < b.size(); ++i) sum = static_cast<std::uint16_t>(sum + b[i]);
    return sum == rd16(b, 2);
}

std::optional<ScoreImage> ScoreFile::decode(std::span<const std::uint8_t> file, std::span<const std::uint8_t> verFile) {
    if (file.size() < kHeaderSize) return std::nullopt;
    std::vector<std::uint8_t> plain(file.begin(), file.end());
    if (!deobfuscate(plain)) return std::nullopt;
    if (rd32(plain, 0x08) != kHeaderSize || rd16(plain, 0x04) != kVersion) return std::nullopt;
    const std::uint32_t bodyLen = rd32(plain, 0x14);
    const std::uint32_t total = rd32(plain, 0x10);
    if (bodyLen > kMaxImage - kHeaderSize) return std::nullopt;
    ScoreImage img;
    std::memcpy(img.header.data(), plain.data(), kHeaderSize);
    img.bytes.assign(kHeaderSize + bodyLen, 0);
    std::memcpy(img.bytes.data(), plain.data(), kHeaderSize);
    const std::size_t got = lzss::decompress(std::span(plain).subspan(kHeaderSize), std::span(img.bytes).subspan(kHeaderSize));
    // The records are walked up to the stated total length (the rest of the buffer is zero).
    img.bytes.resize(std::min<std::size_t>(img.bytes.size(), std::max<std::size_t>(total, kHeaderSize)));
    (void)got;
    const std::span<const std::uint8_t> b(img.bytes);
    bool ok = true, marker = false, markerOk = false;
    std::size_t at = kHeaderSize;
    while (at + kRecordHeader <= b.size()) {
        const std::uint16_t len = rd16(b, at + 4);
        if (len == 0) {
            ok = false;
            break;
        }
        if (at + len > b.size()) break;
        if (tagIs(b, at, "VRSM") && b[at + 8] == kRecordVersion && len >= kLenVrsm) {
            char v[6] = {};
            std::memcpy(v, b.data() + at + 0x0C, 5);
            const std::string_view version(v, std::strlen(v));
            if (!verCheck(verFile, version, static_cast<std::int32_t>(rd32(b, at + 0x14)), static_cast<std::int32_t>(rd32(b, at + 0x18)))) {
                ok = false;
                break;
            }
        }
        if (tagIs(b, at, "TH7K")) {
            marker = true;
            markerOk = b[at + 8] == kRecordVersion;
        }
        at += len;
    }
    if (!ok || !marker || !markerOk) return std::nullopt;
    return img;
}

void ScoreFile::defaultLists(std::array<ScoreList, kDifficulties * kShots>& lists) {
    for (int d = 0; d < kDifficulties; ++d) {
        for (int s = 0; s < kShots; ++s) {
            ScoreList& l = lists[listIndex(d, s)];
            l.clear();
            for (int i = 0; i < 10; ++i) {
                ScoreEntry e;
                e.score = static_cast<std::uint32_t>(100000 - 10000 * i);
                e.shot = static_cast<std::uint8_t>(s);
                e.difficulty = static_cast<std::uint8_t>(d);
                e.placeholder = true;
                l.push_back(e);
            }
        }
    }
}

int ScoreFile::insert(ScoreList& list, const ScoreEntry& e) {
    std::size_t r = 0;
    while (r < list.size() && list[r].score > e.score) ++r;
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(r), e);
    return static_cast<int>(r);
}

void ScoreFile::readHighScores(const ScoreImage& img, std::array<ScoreList, kDifficulties * kShots>& lists) {
    const std::span<const std::uint8_t> b(img.bytes);
    walk(b, [&](std::size_t at, std::uint16_t len) {
        if (!record(b, at, len, "HSCR", kLenHscr)) return true;
        ScoreEntry e;
        e.score = rd32(b, at + 0x0C);
        e.slow = rdF(b, at + 0x10);
        e.shot = b[at + 0x14];
        e.difficulty = b[at + 0x15];
        e.stage = b[at + 0x16];
        std::memcpy(e.name.data(), b.data() + at + 0x17, 9);
        e.name[8] = 0;
        std::memcpy(e.date.data(), b.data() + at + 0x20, 6);
        e.date[5] = 0;
        e.continues = static_cast<std::int8_t>(b[at + 0x26]);
        if (e.shot < kShots && e.difficulty < kDifficulties) insert(lists[listIndex(e.difficulty, e.shot)], e);
        return true;
    });
}

std::pair<std::uint32_t, int> ScoreFile::hiScore(const ScoreImage* img, int difficulty, int shot) {
    if (!img) return {100000u, 0};
    std::array<ScoreList, kDifficulties * kShots> lists;
    readHighScores(*img, lists);
    const ScoreList& l = lists[listIndex(std::clamp(difficulty, 0, 5), std::clamp(shot, 0, 5))];
    if (l.empty()) return {100000u, 0};
    return {std::max<std::uint32_t>(l.front().score, 100000u), l.front().continues};
}

void ScoreFile::readClears(const ScoreImage* img, std::array<ClearRecord, kShots>& clears) {
    clears.fill(ClearRecord{});
    if (!img) return;
    const std::span<const std::uint8_t> b(img->bytes);
    walk(b, [&](std::size_t at, std::uint16_t len) {
        if (!record(b, at, len, "CLRD", kLenClrd)) return true;
        const std::uint8_t shot = b[at + 0x18];
        if (shot >= kShots) return false;
        ClearRecord& c = clears[shot];
        std::memcpy(c.noContinue.data(), b.data() + at + 0x0C, 6);
        std::memcpy(c.reached.data(), b.data() + at + 0x12, 6);
        return true;
    });
}

void ScoreFile::readSpells(const ScoreImage& img, std::array<SpellRecord, kSpellCards>& spells) {
    const std::span<const std::uint8_t> b(img.bytes);
    walk(b, [&](std::size_t at, std::uint16_t len) {
        if (!record(b, at, len, "CATK", kLenCatk)) return true;
        const std::uint16_t card = rd16(b, at + 0x28);
        if (card >= kSpellCards) return false;
        SpellRecord& s = spells[card];
        s.present = true;
        for (std::size_t i = 0; i < 7; ++i) {
            s.bestBonus[i] = rd32(b, at + 0x0C + 4 * i);
            s.attempts[i] = rd16(b, at + 0x5C + 2 * i);
            s.captures[i] = rd16(b, at + 0x6A + 2 * i);
        }
        s.sum = b[at + 0x2A];
        std::memcpy(s.name.data(), b.data() + at + 0x2B, s.name.size());
        s.name.back() = 0;
        return true;
    });
}

void ScoreFile::readPractice(const ScoreImage* img, ScoreData& data) {
    data.practice.fill(PracticeRecord{});
    if (!img) return;
    const std::span<const std::uint8_t> b(img->bytes);
    walk(b, [&](std::size_t at, std::uint16_t len) {
        if (!record(b, at, len, "PSCR", kLenPscr)) return true;
        const std::uint8_t shot = b[at + 0x14], difficulty = b[at + 0x15], stage = b[at + 0x16];
        if (shot >= kShots || difficulty >= 5 || stage >= 7) return false;
        // The original's checks are looser than its table (difficulty 4 or stage 6 lands in
        // the next cell); the port keeps that but stays inside the table.
        const std::size_t i = ScoreData::practiceIndex(shot, stage, difficulty);
        if (i < data.practice.size()) {
            data.practice[i].plays = static_cast<std::int32_t>(rd32(b, at + 0x0C));
            data.practice[i].score = static_cast<std::int32_t>(rd32(b, at + 0x10));
        }
        return true;
    });
}

bool ScoreFile::readPlayStats(const ScoreImage& img, PlayStats& stats) {
    const std::span<const std::uint8_t> b(img.bytes);
    bool found = false;
    walk(b, [&](std::size_t at, std::uint16_t len) {
        if (!record(b, at, len, "PLST", kLenPlst)) return true;
        stats.running = readTime(b, at + 0x0C);
        stats.play = readTime(b, at + 0x1C);
        for (std::size_t r = 0; r < 7; ++r) {
            const std::size_t o = at + 0x2C + 0x2C * r;
            PlayStats::Row& row = stats.rows[r];
            row.plays = rd32(b, o);
            for (std::size_t s = 0; s < 6; ++s) row.playsPerShot[s] = rd32(b, o + 4 + 4 * s);
            row.retries = rd32(b, o + 0x1C);
            row.clears = rd32(b, o + 0x20);
            row.continues = rd32(b, o + 0x24);
            row.practices = rd32(b, o + 0x28);
        }
        found = true;
        return true;  // the last one wins
    });
    return found;
}

bool ScoreFile::readLastName(const ScoreImage* img, std::array<char, 12>& name) {
    name = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0, 0, 0, 0};
    if (!img) return false;
    const std::span<const std::uint8_t> b(img->bytes);
    bool found = false;
    walk(b, [&](std::size_t at, std::uint16_t len) {
        if (!record(b, at, len, "LSNM", kLenLsnm)) return true;
        std::memcpy(name.data(), b.data() + at + 0x0C, 12);
        name[8] = 0;
        found = true;
        return false;
    });
    return found;
}

std::vector<std::uint8_t> ScoreFile::encode(const WriteInput& in, ZunRng& rng) {
    std::vector<std::uint8_t> body;
    body.reserve(0x9000);
    recordHeader(body, "TH7K", kLenTh7k);
    if (in.lists) {
        for (int d = 0; d < kDifficulties; ++d) {
            for (int s = 0; s < kShots; ++s) {
                const ScoreList& l = (*in.lists)[listIndex(d, s)];
                for (std::size_t i = 0; i < l.size() && i < 10; ++i) {
                    const ScoreEntry& e = l[i];
                    if (e.placeholder) continue;
                    const std::size_t at = body.size();
                    recordHeader(body, "HSCR", kLenHscr);
                    wr32(body, at + 0x0C, e.score);
                    wrF(body, at + 0x10, e.slow);
                    body[at + 0x14] = static_cast<std::uint8_t>(s);
                    body[at + 0x15] = static_cast<std::uint8_t>(d);
                    body[at + 0x16] = e.stage;
                    std::memcpy(body.data() + at + 0x17, e.name.data(), 9);
                    body[at + 0x17 + 8] = 0;
                    std::memcpy(body.data() + at + 0x20, e.date.data(), 6);
                    body[at + 0x20 + 5] = 0;
                    body[at + 0x26] = static_cast<std::uint8_t>(e.continues);
                }
            }
        }
    }
    const ScoreData* data = in.data;
    if (data) {
        for (std::size_t s = 0; s < kShots; ++s) {
            const std::size_t at = body.size();
            recordHeader(body, "CLRD", kLenClrd);
            std::memcpy(body.data() + at + 0x0C, data->clears[s].noContinue.data(), 6);
            std::memcpy(body.data() + at + 0x12, data->clears[s].reached.data(), 6);
            body[at + 0x18] = static_cast<std::uint8_t>(s);
        }
        for (std::size_t c = 0; c < kSpellCards; ++c) {
            const SpellRecord& sp = data->spells[c];
            if (!sp.present) continue;
            const std::size_t at = body.size();
            recordHeader(body, "CATK", kLenCatk);
            for (std::size_t i = 0; i < 7; ++i) {
                wr32(body, at + 0x0C + 4 * i, sp.bestBonus[i]);
                wr16(body, at + 0x5C + 2 * i, sp.attempts[i]);
                wr16(body, at + 0x6A + 2 * i, sp.captures[i]);
            }
            wr16(body, at + 0x28, static_cast<std::uint16_t>(c));
            body[at + 0x2A] = sp.sum;
            std::memcpy(body.data() + at + 0x2B, sp.name.data(), sp.name.size());
        }
        for (int s = 0; s < kShots; ++s) {
            for (int st = 0; st < kPracticeStages; ++st) {
                for (int d = 0; d < kPracticeDifficulties; ++d) {
                    const PracticeRecord& p = data->practiceAt(s, st, d);
                    if (p.score == 0) continue;
                    const std::size_t at = body.size();
                    recordHeader(body, "PSCR", kLenPscr);
                    wr32(body, at + 0x0C, static_cast<std::uint32_t>(p.plays));
                    wr32(body, at + 0x10, static_cast<std::uint32_t>(p.score));
                    body[at + 0x14] = static_cast<std::uint8_t>(s);
                    body[at + 0x15] = static_cast<std::uint8_t>(d);
                    body[at + 0x16] = static_cast<std::uint8_t>(st);
                }
            }
        }
    }
    {
        const std::size_t at = body.size();
        recordHeader(body, "LSNM", kLenLsnm);
        std::memcpy(body.data() + at + 0x0C, in.lastName.data(), 12);
        body[at + 0x0C + 8] = 0;
    }
    if (data) {
        const std::size_t at = body.size();
        recordHeader(body, "PLST", kLenPlst);
        writeTime(body, at + 0x0C, data->stats.running);
        writeTime(body, at + 0x1C, data->stats.play);
        for (std::size_t r = 0; r < 7; ++r) {
            const std::size_t o = at + 0x2C + 0x2C * r;
            const PlayStats::Row& row = data->stats.rows[r];
            wr32(body, o, row.plays);
            for (std::size_t s = 0; s < 6; ++s) wr32(body, o + 4 + 4 * s, row.playsPerShot[s]);
            wr32(body, o + 0x1C, row.retries);
            wr32(body, o + 0x20, row.clears);
            wr32(body, o + 0x24, row.continues);
            wr32(body, o + 0x28, row.practices);
        }
    }
    {
        const std::size_t at = body.size();
        recordHeader(body, "VRSM", kLenVrsm);
        std::memcpy(body.data() + at + 0x0C, "0100b", 6);
        wr32(body, at + 0x14, static_cast<std::uint32_t>(in.exe.size));
        wr32(body, at + 0x18, static_cast<std::uint32_t>(in.exe.checksum));
    }

    const std::vector<std::uint8_t> packed = lzss::compress(body);
    std::vector<std::uint8_t> out(kHeaderSize, 0);
    if (in.previousHeader) std::memcpy(out.data(), in.previousHeader->data(), kHeaderSize);
    wr32(out, 0x14, static_cast<std::uint32_t>(body.size()));
    wr32(out, 0x10, static_cast<std::uint32_t>(kHeaderSize + body.size()));
    wr32(out, 0x18, static_cast<std::uint32_t>(packed.size()));
    out.insert(out.end(), packed.begin(), packed.end());
    wr32(out, 0x08, kHeaderSize);
    wr16(out, 0x02, 0);
    out[1] = static_cast<std::uint8_t>(rng.u16InRange(256));
    out[6] = static_cast<std::uint8_t>(rng.u16InRange(256));
    wr16(out, 0x04, kVersion);
    obfuscate(out);
    return out;
}

std::array<char, 6> todayDate() {
    std::array<char, 6> d{'-', '-', '/', '-', '-', 0};
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    d[0] = static_cast<char>('0' + (tm.tm_mon + 1) / 10);
    d[1] = static_cast<char>('0' + (tm.tm_mon + 1) % 10);
    d[3] = static_cast<char>('0' + tm.tm_mday / 10);
    d[4] = static_cast<char>('0' + tm.tm_mday % 10);
    return d;
}

}  // namespace touhou::th07
