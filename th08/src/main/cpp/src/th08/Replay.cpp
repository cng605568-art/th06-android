#include "th08/Replay.hpp"

#include <algorithm>
#include <cstring>

#include "io/Lzss.hpp"

namespace touhou::th08 {

namespace {

constexpr std::size_t kMaxImage = 64u << 20;

std::uint32_t rd32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
std::uint16_t rd16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }
void wr32(std::span<std::uint8_t> b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void wr16(std::span<std::uint8_t> b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
float rdF(std::span<const std::uint8_t> b, std::size_t at) {
    const std::uint32_t u = rd32(b, at);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void wrF(std::span<std::uint8_t> b, std::size_t at, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    wr32(b, at, u);
}

void putBlockHeader(std::vector<std::uint8_t>& img, const ReplayStage& s) {
    const std::size_t at = img.size();
    img.resize(at + kReplayBlockHeaderSize, 0);
    std::span<std::uint8_t> b(img);
    wr32(b, at + 0x00, s.carriedScore);
    wr32(b, at + 0x04, static_cast<std::uint32_t>(s.pointItems));
    wr32(b, at + 0x08, static_cast<std::uint32_t>(s.graze));
    wr32(b, at + 0x0C, static_cast<std::uint32_t>(s.extends));
    wr32(b, at + 0x10, static_cast<std::uint32_t>(s.nextExtend));
    wr32(b, at + 0x14, static_cast<std::uint32_t>(s.pointValue));
    wr16(b, at + 0x18, static_cast<std::uint16_t>(s.gauge));
    wr16(b, at + 0x1A, s.seed);
    b[at + 0x1C] = s.power;
    b[at + 0x1D] = s.lives;
    b[at + 0x1E] = s.bombs;
    b[at + 0x1F] = s.rank;
    b[at + 0x20] = s.fullPowerByte;
    b[at + 0x21] = s.cardsCaptured;
    b[at + 0x22] = static_cast<std::uint8_t>(s.nightSteps);
    b[at + 0x23] = s.garbage;
}

void readBlockHeader(std::span<const std::uint8_t> b, std::size_t at, ReplayStage& s) {
    s.carriedScore = rd32(b, at + 0x00);
    s.pointItems = static_cast<std::int32_t>(rd32(b, at + 0x04));
    s.graze = static_cast<std::int32_t>(rd32(b, at + 0x08));
    s.extends = static_cast<std::int32_t>(rd32(b, at + 0x0C));
    s.nextExtend = static_cast<std::int32_t>(rd32(b, at + 0x10));
    s.pointValue = static_cast<std::int32_t>(rd32(b, at + 0x14));
    s.gauge = static_cast<std::int16_t>(rd16(b, at + 0x18));
    s.seed = rd16(b, at + 0x1A);
    s.power = b[at + 0x1C];
    s.lives = b[at + 0x1D];
    s.bombs = b[at + 0x1E];
    s.rank = b[at + 0x1F];
    s.fullPowerByte = b[at + 0x20];
    s.cardsCaptured = b[at + 0x21];
    s.nightSteps = static_cast<std::int8_t>(b[at + 0x22]);
    s.garbage = b[at + 0x23];
}

std::optional<ReplayFile> fail(std::string* error, const char* why) {
    if (error) *error = why;
    return std::nullopt;
}

std::string text(std::span<const std::uint8_t> b, std::size_t at, std::size_t n) {
    std::string s;
    for (std::size_t i = at; i < at + n && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
    return s;
}

}  // namespace

// ---- summary --------------------------------------------------------------------------------

std::string ReplaySummary::date() const { return text(bytes, 0x04, 5); }
std::string ReplaySummary::name() const { return text(bytes, 0x0A, 8); }
std::string ReplaySummary::cardName() const { return text(bytes, 0x16, 48); }
std::uint32_t ReplaySummary::score() const { return rd32(bytes, 0x48); }
float ReplaySummary::slowdown() const { return rdF(bytes, 0xB0); }
std::int32_t ReplaySummary::exeSize() const { return static_cast<std::int32_t>(rd32(bytes, 0xBC)); }
std::int32_t ReplaySummary::exeChecksum() const { return static_cast<std::int32_t>(rd32(bytes, 0xC0)); }
std::string ReplaySummary::versionTag() const { return text(bytes, 0xC4, 5); }

void ReplaySummary::setDate(std::string_view mmdd) {
    for (std::size_t i = 0; i < 6; ++i) bytes[0x04 + i] = i < 5 && i < mmdd.size() ? static_cast<std::uint8_t>(mmdd[i]) : 0;
}

void ReplaySummary::setName(std::string_view name) {
    for (std::size_t i = 0; i < 9; ++i) {
        std::uint8_t c = 0;
        if (i < 8) c = i < name.size() && name[i] != 0 ? static_cast<std::uint8_t>(name[i]) : ' ';
        bytes[0x0A + i] = c;
    }
}

void ReplaySummary::setCard(int card) { wr16(bytes, 0x14, static_cast<std::uint16_t>(static_cast<std::int16_t>(card))); }

void ReplaySummary::setCardName(std::span<const std::uint8_t> name48) {
    std::memset(bytes.data() + 0x16, 0, 48);
    std::memcpy(bytes.data() + 0x16, name48.data(), std::min<std::size_t>(name48.size(), 48));
}

void ReplaySummary::setScore(std::uint32_t score) { wr32(bytes, 0x48, score); }

void ReplaySummary::setConfig(std::span<const std::uint8_t> config60) {
    std::memcpy(bytes.data() + 0x4C, config60.data(), std::min<std::size_t>(config60.size(), 60));
}

void ReplaySummary::setSlowdown(float percent) {
    wrF(bytes, 0xAC, percent + 1.12f);
    wrF(bytes, 0xB0, percent);
    wr32(bytes, 0xB8, 30);
}

void ReplaySummary::setExe(std::int32_t size, std::int32_t checksum, std::string_view tag) {
    wr32(bytes, 0xBC, static_cast<std::uint32_t>(size));
    wr32(bytes, 0xC0, static_cast<std::uint32_t>(checksum));
    for (std::size_t i = 0; i < 8; ++i) bytes[0xC4 + i] = i < tag.size() ? static_cast<std::uint8_t>(tag[i]) : 0;
}

// ---- the file -------------------------------------------------------------------------------

int ReplayFile::firstStage() const {
    for (int i = 0; i < kReplayStages; ++i) {
        if (stages[static_cast<std::size_t>(i)].present) return i;
    }
    return -1;
}

int ReplayFile::nextStage(int index) const {
    for (int i = std::max(index + 1, 0); i < kReplayStages; ++i) {
        if (stages[static_cast<std::size_t>(i)].present) return i;
    }
    return -1;
}

void ReplayFile::obfuscate(std::span<std::uint8_t> file) {
    if (file.size() < kReplayHeaderSize) return;
    const std::size_t end = std::min<std::size_t>(rd32(file, 0x0C), file.size());
    std::uint8_t k = file[0x15];
    for (std::size_t i = 0x18; i < end; ++i) {
        file[i] = static_cast<std::uint8_t>(file[i] + k);
        k = static_cast<std::uint8_t>(k + 7);
    }
}

void ReplayFile::deobfuscate(std::span<std::uint8_t> file) {
    if (file.size() < kReplayHeaderSize) return;
    const std::size_t end = std::min<std::size_t>(rd32(file, 0x0C), file.size());
    std::uint8_t k = file[0x15];
    for (std::size_t i = 0x18; i < end; ++i) {
        file[i] = static_cast<std::uint8_t>(file[i] - k);
        k = static_cast<std::uint8_t>(k + 7);
    }
}

std::uint32_t ReplayFile::checksum(std::span<const std::uint8_t> plainFile) {
    std::uint32_t sum = kReplayChecksumBase;
    if (plainFile.size() < kReplayHeaderSize) return sum;
    const std::size_t end = std::min<std::size_t>(rd32(plainFile, 0x0C), plainFile.size());
    for (std::size_t i = 0x15; i < end; ++i) sum += plainFile[i];
    return sum;
}

std::optional<ReplayFile> ReplayFile::parse(std::span<const std::uint8_t> file, std::string* error, bool headerOnly) {
    if (file.size() < kReplayHeaderSize) return fail(error, "too short");
    if (std::memcmp(file.data(), "T8RP", 4) != 0) return fail(error, "not a th08 replay");
    if (rd16(file, 0x04) != kReplayVersion) return fail(error, "wrong version");
    const std::uint32_t end = rd32(file, 0x0C);
    if (end < kReplayHeaderSize || end > file.size()) return fail(error, "bad data length");
    std::vector<std::uint8_t> plain(file.begin(), file.begin() + end);
    deobfuscate(plain);
    if (checksum(plain) != rd32(plain, 0x10)) return fail(error, "checksum mismatch");
    const std::uint32_t packedSize = rd32(plain, 0x18);
    const std::uint32_t bodySize = rd32(plain, 0x1C);
    if (bodySize < kReplaySummarySize || bodySize > kMaxImage) return fail(error, "bad sizes");
    const std::size_t imageSize = kReplayHeaderSize + bodySize;
    const std::span<const std::uint8_t> packed(plain.data() + kReplayHeaderSize, std::min<std::size_t>(packedSize, end - kReplayHeaderSize));
    std::vector<std::uint8_t> img(imageSize, 0);
    std::memcpy(img.data(), plain.data(), kReplayHeaderSize);
    if (lzss::decompress(packed, std::span(img).subspan(kReplayHeaderSize)) != bodySize) return fail(error, "body does not decompress");
    ReplayFile r;
    r.headerFiller = plain[0x14];
    r.key = plain[0x15];
    std::memcpy(r.summary.bytes.data(), img.data() + kReplayHeaderSize, kReplaySummarySize);
    if (r.summary.slowModeRecorded()) return fail(error, "recorded in slow mode");
    r.user.assign(file.begin() + end, file.end());
    std::array<std::uint32_t, kReplayStages> stageAt{}, fpsAt{};
    std::vector<std::uint32_t> bounds;
    for (int i = 0; i < kReplayStages; ++i) {
        stageAt[static_cast<std::size_t>(i)] = rd32(img, 0x20 + 4 * static_cast<std::size_t>(i));
        fpsAt[static_cast<std::size_t>(i)] = rd32(img, 0x44 + 4 * static_cast<std::size_t>(i));
        for (const std::uint32_t o : {stageAt[static_cast<std::size_t>(i)], fpsAt[static_cast<std::size_t>(i)]}) {
            if (o != 0 && (o < kReplayFirstBlock || o > imageSize)) return fail(error, "bad block offset");
            if (o != 0) bounds.push_back(o);
        }
    }
    // A block runs to the next block of either kind (or the image's end).
    auto boundAfter = [&](std::uint32_t at) {
        std::uint32_t b = static_cast<std::uint32_t>(imageSize);
        for (const std::uint32_t o : bounds) {
            if (o > at && o < b) b = o;
        }
        return b;
    };
    for (int i = 0; i < kReplayStages; ++i) {
        ReplayStage& s = r.stages[static_cast<std::size_t>(i)];
        const std::uint32_t at = stageAt[static_cast<std::size_t>(i)];
        if (at == 0) continue;
        if (at + kReplayBlockHeaderSize > imageSize) return fail(error, "stage block outside the image");
        s.present = true;
        readBlockHeader(img, at, s);
        if (headerOnly) continue;
        const std::uint32_t stop = std::max<std::uint32_t>(boundAfter(at), static_cast<std::uint32_t>(at + kReplayBlockHeaderSize));
        const std::size_t n = (stop - at - kReplayBlockHeaderSize) / 2;
        s.entries.resize(n);
        for (std::size_t k = 0; k < n; ++k) s.entries[k] = rd16(img, at + kReplayBlockHeaderSize + 2 * k);
        const std::uint32_t f = fpsAt[static_cast<std::size_t>(i)];
        if (f != 0) s.fps.assign(img.begin() + f, img.begin() + boundAfter(f));
    }
    return r;
}

std::vector<std::uint8_t> ReplayFile::serialize(ZunRng& rng, std::string_view userText) {
    std::vector<std::uint8_t> img(kReplayHeaderSize, 0);
    std::array<std::uint32_t, kReplayStages> stageAt{}, fpsAt{};
    img.insert(img.end(), summary.bytes.begin(), summary.bytes.end());
    for (int i = 0; i < kReplayStages; ++i) {
        const ReplayStage& s = stages[static_cast<std::size_t>(i)];
        if (!s.present) continue;
        stageAt[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(img.size());
        putBlockHeader(img, s);
        for (const std::uint16_t e : s.entries) {
            const std::size_t at = img.size();
            img.resize(at + 2);
            wr16(img, at, e);
        }
    }
    for (int i = 0; i < kReplayStages; ++i) {
        const ReplayStage& s = stages[static_cast<std::size_t>(i)];
        if (!s.present) continue;
        fpsAt[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(img.size());
        img.insert(img.end(), s.fps.begin(), s.fps.end());
    }
    // The key, then the summary's filler, then the header's (spec_replays 4.1 step 7).
    key = static_cast<std::uint8_t>(rng.u16InRange(128) + 64);
    summary.bytes[0x00] = static_cast<std::uint8_t>(rng.u16InRange(256));
    headerFiller = static_cast<std::uint8_t>(rng.u16InRange(256));
    img[kReplayHeaderSize] = summary.bytes[0x00];

    const std::uint32_t bodySize = static_cast<std::uint32_t>(img.size() - kReplayHeaderSize);
    const std::vector<std::uint8_t> packed = lzss::compress(std::span(img).subspan(kReplayHeaderSize));
    std::vector<std::uint8_t> out(kReplayHeaderSize, 0);
    std::memcpy(out.data(), "T8RP", 4);
    wr16(out, 0x04, kReplayVersion);
    out[0x06] = 0;
    out[0x07] = 1;
    wr32(out, 0x0C, static_cast<std::uint32_t>(kReplayHeaderSize + packed.size()));
    out[0x14] = headerFiller;
    out[0x15] = key;
    wr32(out, 0x18, static_cast<std::uint32_t>(packed.size()));
    wr32(out, 0x1C, bodySize);
    for (int i = 0; i < kReplayStages; ++i) {
        wr32(out, 0x20 + 4 * static_cast<std::size_t>(i), stageAt[static_cast<std::size_t>(i)]);
        wr32(out, 0x44 + 4 * static_cast<std::size_t>(i), fpsAt[static_cast<std::size_t>(i)]);
    }
    out.insert(out.end(), packed.begin(), packed.end());
    wr32(out, 0x10, checksum(out));
    obfuscate(out);
    if (!userText.empty()) {
        // {"USER", length (text + 12, rounded up to even), type 0, 3 bytes}, then the text.
        const std::uint32_t len = static_cast<std::uint32_t>((userText.size() + 12 + 1) & ~std::size_t{1});
        const std::size_t at = out.size();
        out.resize(at + len, 0);
        std::memcpy(out.data() + at, "USER", 4);
        wr32(out, at + 4, len);
        std::memcpy(out.data() + at + 12, userText.data(), userText.size());
    }
    return out;
}

}  // namespace touhou::th08
