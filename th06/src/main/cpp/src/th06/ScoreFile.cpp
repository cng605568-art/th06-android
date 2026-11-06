#include "th06/ScoreFile.hpp"

#include <algorithm>
#include <cstring>

namespace touhou::th06 {
namespace {

constexpr std::size_t kHeader = 0x14;
constexpr std::uint8_t kVersion = 0x10;
constexpr std::uint16_t kLenHscr = 0x1C, kLenClrd = 0x18, kLenCatk = 0x40, kLenPscr = 0x14, kLenTh6k = 0x0C;

template <class T>
T rd(std::span<const std::uint8_t> b, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}
template <class T>
void put(std::vector<std::uint8_t>& b, T v) {
    const auto at = b.size();
    b.resize(at + sizeof(T));
    std::memcpy(b.data() + at, &v, sizeof(T));
}
template <class T>
void poke(std::vector<std::uint8_t>& b, std::size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(T));
}

void chunkHeader(std::vector<std::uint8_t>& b, const char* tag, std::uint16_t len) {
    b.insert(b.end(), tag, tag + 4);
    put<std::uint16_t>(b, len);
    put<std::uint16_t>(b, len);
    b.push_back(kVersion);
    b.push_back(0);
    put<std::uint16_t>(b, 0);
}

bool tagIs(std::span<const std::uint8_t> b, std::size_t at, const char* tag) {
    return at + 4 <= b.size() && std::memcmp(b.data() + at, tag, 4) == 0;
}

// Walks the chunks: fn(offset, length) returns false to stop. A zero length ends the walk
// (the original would loop forever); offsets past the data are not read.
template <class Fn>
void walk(std::span<const std::uint8_t> b, Fn fn) {
    const std::size_t end = std::min<std::size_t>(rd<std::uint32_t>(b, 0x10), b.size());
    std::size_t at = rd<std::uint32_t>(b, 0x08);
    while (at < end && at + 12 <= b.size()) {
        const std::uint16_t len = rd<std::uint16_t>(b, at + 4);
        if (len == 0) break;
        if (!fn(at, len)) break;
        at += len;
    }
}

constexpr char kNanashi[8] = {'N', 'a', 'n', 'a', 's', 'h', 'i', ' '};

}  // namespace

ScoreFile::ScoreFile() { reset(); }

void ScoreFile::reset() {
    for (int d = 0; d < kDifficulties; ++d) {
        for (int t = 0; t < kShotTypes; ++t) {
            Table& tb = tables_[index(d, t)];
            tb.clear();
            for (int k = 0; k < 10; ++k) {
                Entry e;
                e.score = static_cast<std::uint32_t>(1'000'000 - 100'000 * k);
                e.shotType = static_cast<std::uint8_t>(t);
                e.difficulty = static_cast<std::uint8_t>(d);
                e.stage = 1;
                std::memcpy(e.name.data(), kNanashi, 8);
                e.placeholder = true;
                insert(tb, e);
            }
        }
    }
    clears_ = {};
    practice_ = {};
    spells_ = {};
    loaded_ = false;
}

std::optional<std::vector<std::uint8_t>> ScoreFile::decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeader) return std::nullopt;
    std::vector<std::uint8_t> plain(bytes.begin(), bytes.end());
    std::uint8_t k = bytes[1];
    for (std::size_t i = 2; i < bytes.size(); ++i) {
        k = static_cast<std::uint8_t>((k << 3) | (k >> 5));
        plain[i] = static_cast<std::uint8_t>(bytes[i] ^ k);
        k = static_cast<std::uint8_t>(k + plain[i]);
    }
    std::uint16_t sum = 0;
    for (std::size_t i = 4; i < plain.size(); ++i) sum = static_cast<std::uint16_t>(sum + plain[i]);
    if (sum != rd<std::uint16_t>(plain, 2)) return std::nullopt;
    return plain;
}

void ScoreFile::encode(std::vector<std::uint8_t>& b) {
    std::uint16_t sum = 0;
    for (std::size_t i = 4; i < b.size(); ++i) sum = static_cast<std::uint16_t>(sum + b[i]);
    poke<std::uint16_t>(b, 2, sum);
    std::uint8_t k = b[1];
    for (std::size_t i = 2; i < b.size(); ++i) {
        k = static_cast<std::uint8_t>((k << 3) | (k >> 5));
        const std::uint8_t p = b[i];
        b[i] = static_cast<std::uint8_t>(p ^ k);
        k = static_cast<std::uint8_t>(k + p);
    }
}

bool ScoreFile::load(std::span<const std::uint8_t> bytes) {
    reset();
    auto decoded = decode(bytes);
    if (!decoded) return false;
    const std::span<const std::uint8_t> b(*decoded);
    bool marker = false;
    walk(b, [&](std::size_t at, std::uint16_t) {
        marker = tagIs(b, at, "TH6K");
        return !marker;
    });
    if (!marker) return false;

    // High scores: every matching entry goes into its table, in file order.
    walk(b, [&](std::size_t at, std::uint16_t) {
        if (!tagIs(b, at, "HSCR") || b[at + 8] != kVersion || at + kLenHscr > b.size()) return true;
        Entry e;
        e.score = rd<std::uint32_t>(b, at + 0x0C);
        e.shotType = b[at + 0x10];
        e.difficulty = b[at + 0x11];
        e.stage = b[at + 0x12];
        std::memcpy(e.name.data(), b.data() + at + 0x13, 8);
        if (e.shotType < kShotTypes && e.difficulty < kDifficulties) insert(table(e.difficulty, e.shotType), e);
        return true;
    });
    // Spell history: a bad index stops the walk.
    walk(b, [&](std::size_t at, std::uint16_t) {
        if (!tagIs(b, at, "CATK") || b[at + 8] != kVersion || at + kLenCatk > b.size()) return true;
        const std::uint16_t i = rd<std::uint16_t>(b, at + 0x10);
        if (i >= kSpells) return false;
        Spell& s = spells_[i];
        s.present = true;
        s.captureBonus = rd<std::uint32_t>(b, at + 0x0C);
        s.nameSum = b[at + 0x12];
        std::memcpy(s.capturedBy.data(), b.data() + at + 0x13, 5);
        std::memcpy(s.name.data(), b.data() + at + 0x18, 34);
        s.attempts = rd<std::uint16_t>(b, at + 0x3C);
        s.captures = rd<std::uint16_t>(b, at + 0x3E);
        return true;
    });
    // Clear progress: a bad shot type stops the walk.
    walk(b, [&](std::size_t at, std::uint16_t) {
        if (!tagIs(b, at, "CLRD") || b[at + 8] != kVersion || at + kLenClrd > b.size()) return true;
        const std::uint8_t t = b[at + 0x16];
        if (t >= kShotTypes) return false;
        std::memcpy(clears_[t].noContinue.data(), b.data() + at + 0x0C, 5);
        std::memcpy(clears_[t].cleared.data(), b.data() + at + 0x11, 5);
        return true;
    });
    // Practice bests: out-of-range keys stop the walk; Extra or a 7th stage (which the
    // original stores out of bounds) are skipped.
    walk(b, [&](std::size_t at, std::uint16_t) {
        if (!tagIs(b, at, "PSCR") || b[at + 8] != kVersion || at + kLenPscr > b.size()) return true;
        const std::uint8_t t = b[at + 0x10], d = b[at + 0x11], st = b[at + 0x12];
        if (t >= kShotTypes || d >= 5 || st >= 7) return false;
        if (d < 4 && st < kStages) practice(t, st, d) = rd<std::uint32_t>(b, at + 0x0C);
        return true;
    });
    loaded_ = true;
    return true;
}

std::vector<std::uint8_t> ScoreFile::save(ZunRng& rng) const {
    std::vector<std::uint8_t> b(kHeader, 0);
    chunkHeader(b, "TH6K", kLenTh6k);
    for (int d = 0; d < kDifficulties; ++d) {
        for (int t = 0; t < kShotTypes; ++t) {
            const Table& tb = table(d, t);
            for (std::size_t r = 0; r < tb.size() && r < 10; ++r) {
                const Entry& e = tb[r];
                if (e.placeholder) continue;
                chunkHeader(b, "HSCR", kLenHscr);
                put<std::uint32_t>(b, e.score);
                b.push_back(static_cast<std::uint8_t>(t));
                b.push_back(static_cast<std::uint8_t>(d));
                b.push_back(e.stage);
                b.insert(b.end(), e.name.begin(), e.name.end());
                b.push_back(0);
            }
        }
    }
    for (int t = 0; t < kShotTypes; ++t) {
        chunkHeader(b, "CLRD", kLenClrd);
        b.insert(b.end(), clears_[static_cast<std::size_t>(t)].noContinue.begin(), clears_[static_cast<std::size_t>(t)].noContinue.end());
        b.insert(b.end(), clears_[static_cast<std::size_t>(t)].cleared.begin(), clears_[static_cast<std::size_t>(t)].cleared.end());
        b.push_back(static_cast<std::uint8_t>(t));
        b.push_back(0);
    }
    for (int i = 0; i < kSpells; ++i) {
        const Spell& s = spells_[static_cast<std::size_t>(i)];
        if (!s.present) continue;
        chunkHeader(b, "CATK", kLenCatk);
        put<std::uint32_t>(b, s.captureBonus);
        put<std::uint16_t>(b, static_cast<std::uint16_t>(i));
        b.push_back(s.nameSum);
        b.insert(b.end(), s.capturedBy.begin(), s.capturedBy.end());
        b.insert(b.end(), s.name.begin(), s.name.end());
        put<std::uint16_t>(b, 0);
        put<std::uint16_t>(b, s.attempts);
        put<std::uint16_t>(b, s.captures);
    }
    for (int t = 0; t < kShotTypes; ++t) {
        for (int st = 0; st < kStages; ++st) {
            for (int d = 0; d < 4; ++d) {
                const std::uint32_t score = practice(t, st, d);
                if (score == 0) continue;
                chunkHeader(b, "PSCR", kLenPscr);
                put<std::uint32_t>(b, score);
                b.push_back(static_cast<std::uint8_t>(t));
                b.push_back(static_cast<std::uint8_t>(d));
                b.push_back(static_cast<std::uint8_t>(st));
                b.push_back(0);
            }
        }
    }
    poke<std::uint16_t>(b, 0x04, 0x0010);
    poke<std::uint32_t>(b, 0x08, static_cast<std::uint32_t>(kHeader));
    poke<std::uint32_t>(b, 0x10, static_cast<std::uint32_t>(b.size()));
    b[1] = static_cast<std::uint8_t>(rng.u16InRange(256));
    b[6] = static_cast<std::uint8_t>(rng.u16InRange(256));
    encode(b);
    return b;
}

int ScoreFile::insert(Table& table, const Entry& e) {
    std::size_t r = 0;
    while (r < table.size() && table[r].score > e.score) ++r;
    table.insert(table.begin() + static_cast<std::ptrdiff_t>(r), e);
    return static_cast<int>(r);
}

std::uint32_t ScoreFile::hiScore(int difficulty, int shotType) const {
    std::uint32_t best = 0;
    for (const Entry& e : table(difficulty, shotType)) {
        if (!e.placeholder) best = std::max(best, e.score);
    }
    return std::max<std::uint32_t>(best, 1'000'000);
}

void ScoreFile::stageEntered(int shotType, int difficulty, int stage, bool continued) {
    if (difficulty < 0 || difficulty > 4 || stage < 1) return;
    Clear& c = clear(shotType);
    const auto s = static_cast<std::uint8_t>(std::min(stage - 1, 255));
    auto& cleared = c.cleared[static_cast<std::size_t>(difficulty)];
    cleared = std::max(cleared, s);
    if (!continued) {
        auto& clean = c.noContinue[static_cast<std::size_t>(difficulty)];
        clean = std::max(clean, s);
    }
}

void ScoreFile::gameCleared(int shotType, int difficulty, bool continued) {
    if (difficulty < 0 || difficulty > 4) return;
    Clear& c = clear(shotType);
    c.cleared[static_cast<std::size_t>(difficulty)] = kStageCleared;
    if (!continued) c.noContinue[static_cast<std::size_t>(difficulty)] = kStageCleared;
}

void ScoreFile::spellDeclared(int i, std::span<const std::uint8_t> name, std::uint32_t captureBonus) {
    if (i < 0 || i >= kSpells) return;
    Spell& s = spells_[static_cast<std::size_t>(i)];
    s.present = true;
    s.name = {};
    std::uint8_t sum = 0;
    for (std::size_t k = 0; k < name.size() && k + 1 < s.name.size() && name[k] != 0; ++k) {
        s.name[k] = name[k];
        sum = static_cast<std::uint8_t>(sum + name[k]);
    }
    if (sum != s.nameSum) {
        s.attempts = s.captures = 0;
        s.nameSum = sum;
    }
    s.captureBonus = captureBonus;
    if (s.attempts < 9999) ++s.attempts;
}

void ScoreFile::spellCaptured(int i, int shotType) {
    if (i < 0 || i >= kSpells) return;
    Spell& s = spells_[static_cast<std::size_t>(i)];
    ++s.captures;
    for (int k = 4; k > 0; --k) s.capturedBy[static_cast<std::size_t>(k)] = s.capturedBy[static_cast<std::size_t>(k - 1)];
    s.capturedBy[0] = static_cast<std::uint8_t>(shotType & 3);
}

}  // namespace touhou::th06
