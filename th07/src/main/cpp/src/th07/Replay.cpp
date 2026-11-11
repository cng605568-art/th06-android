#include "th07/Replay.hpp"

#include <algorithm>
#include <cstring>

#include "io/Lzss.hpp"
#include "th07/GameConfig.hpp"
#include "th07/World.hpp"

namespace touhou::th07 {

namespace {

constexpr std::size_t kMaxImage = 16u << 20;  // far above 7 full stage buffers

std::uint32_t rd32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
std::uint16_t rd16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}
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

bool fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}

void putBlockHeader(std::vector<std::uint8_t>& img, const ReplayStage& s) {
    const std::size_t at = img.size();
    img.resize(at + kReplayBlockHeaderSize, 0);
    std::span<std::uint8_t> b(img);
    wr32(b, at + 0x00, s.endScore);
    wr32(b, at + 0x04, static_cast<std::uint32_t>(s.extendPoints));
    wr32(b, at + 0x08, static_cast<std::uint32_t>(s.cherry));
    wr32(b, at + 0x0C, static_cast<std::uint32_t>(s.cherryMax));
    wr32(b, at + 0x10, static_cast<std::uint32_t>(s.cherryPlus));
    wr32(b, at + 0x14, static_cast<std::uint32_t>(s.totalGraze));
    wr32(b, at + 0x18, static_cast<std::uint32_t>(s.extendsGranted));
    wr32(b, at + 0x1C, static_cast<std::uint32_t>(s.nextExtend));
    wr16(b, at + 0x20, s.seed);
    b[at + 0x22] = s.power;
    b[at + 0x23] = s.lives;
    b[at + 0x24] = s.bombs;
    b[at + 0x25] = s.rank;
    b[at + 0x26] = s.chain;
    b[at + 0x27] = s.spells;
    wr32(b, at + 0x28, static_cast<std::uint32_t>(s.unused));
}

void readBlockHeader(std::span<const std::uint8_t> b, std::size_t at, ReplayStage& s) {
    s.endScore = rd32(b, at + 0x00);
    s.extendPoints = static_cast<std::int32_t>(rd32(b, at + 0x04));
    s.cherry = static_cast<std::int32_t>(rd32(b, at + 0x08));
    s.cherryMax = static_cast<std::int32_t>(rd32(b, at + 0x0C));
    s.cherryPlus = static_cast<std::int32_t>(rd32(b, at + 0x10));
    s.totalGraze = static_cast<std::int32_t>(rd32(b, at + 0x14));
    s.extendsGranted = static_cast<std::int32_t>(rd32(b, at + 0x18));
    s.nextExtend = static_cast<std::int32_t>(rd32(b, at + 0x1C));
    s.seed = rd16(b, at + 0x20);
    s.power = b[at + 0x22];
    s.lives = b[at + 0x23];
    s.bombs = b[at + 0x24];
    s.rank = b[at + 0x25];
    s.chain = b[at + 0x26];
    s.spells = b[at + 0x27];
    s.unused = static_cast<std::int32_t>(rd32(b, at + 0x28));
}

}  // namespace

// ---- summary --------------------------------------------------------------------------------

std::string ReplaySummary::date() const {
    std::string s;
    for (std::size_t i = 0x04; i < 0x04 + 5 && bytes[i] != 0; ++i) s.push_back(static_cast<char>(bytes[i]));
    return s;
}

std::string ReplaySummary::name() const {
    std::string s;
    for (std::size_t i = 0x0A; i < 0x0A + 8 && bytes[i] != 0; ++i) s.push_back(static_cast<char>(bytes[i]));
    return s;
}

std::uint32_t ReplaySummary::score() const { return rd32(bytes, 0x18); }
float ReplaySummary::slowdown() const { return rdF(bytes, 0x78); }
std::int32_t ReplaySummary::exeSize() const { return static_cast<std::int32_t>(rd32(bytes, 0x84)); }
std::int32_t ReplaySummary::exeChecksum() const { return static_cast<std::int32_t>(rd32(bytes, 0x88)); }

std::string ReplaySummary::versionTag() const {
    std::string s;
    for (std::size_t i = 0x8C; i < 0x8C + 5 && bytes[i] != 0; ++i) s.push_back(static_cast<char>(bytes[i]));
    return s;
}

void ReplaySummary::setDate(std::string_view mmdd) {
    for (std::size_t i = 0; i < 6; ++i) bytes[0x04 + i] = i < 5 && i < mmdd.size() ? static_cast<std::uint8_t>(mmdd[i]) : 0;
}

void ReplaySummary::setName(std::string_view name) {
    for (std::size_t i = 0; i < 12; ++i) {
        std::uint8_t c = 0;
        if (i < 8) c = i < name.size() && name[i] != 0 ? static_cast<std::uint8_t>(name[i]) : ' ';
        bytes[0x0A + i] = c;
    }
}

void ReplaySummary::setScore(std::uint32_t score) { wr32(bytes, 0x18, score); }

void ReplaySummary::setConfig(std::span<const std::uint8_t> config56) {
    std::memcpy(bytes.data() + 0x1C, config56.data(), std::min<std::size_t>(config56.size(), 0x38));
}

void ReplaySummary::setSlowdown(float percent) {
    wrF(bytes, 0x74, percent + 1.12f);
    wrF(bytes, 0x78, percent);
    wrF(bytes, 0x7C, percent + 2.34f);
    wr32(bytes, 0x80, 30);
}

void ReplaySummary::setExe(const ExeIdentity& exe, std::string_view tag) {
    wr32(bytes, 0x84, static_cast<std::uint32_t>(exe.size));
    wr32(bytes, 0x88, static_cast<std::uint32_t>(exe.checksum));
    // A 4-byte field: 'b' and the NUL spill into the next two bytes [QUIRK].
    for (std::size_t i = 0; i < 6; ++i) bytes[0x8C + i] = i < tag.size() ? static_cast<std::uint8_t>(tag[i]) : 0;
}

// ---- the file -------------------------------------------------------------------------------

int ReplayFile::firstStage() const {
    for (int i = 0; i < kReplayStages; ++i) {
        if (stages[static_cast<std::size_t>(i)].present) return i;
    }
    return -1;
}

std::uint8_t ReplayFile::stageMask() const {
    std::uint8_t m = 0;
    for (int i = 0; i < kReplayStages; ++i) {
        if (stages[static_cast<std::size_t>(i)].present) m = static_cast<std::uint8_t>(m | (1u << i));
    }
    return m;
}

void ReplayFile::obfuscate(std::span<std::uint8_t> file) {
    if (file.size() <= 0x10) return;
    std::uint8_t k = file[0x0D];
    for (std::size_t i = 0x10; i < file.size(); ++i) {
        file[i] = static_cast<std::uint8_t>(file[i] + k);
        k = static_cast<std::uint8_t>(k + 7);
    }
}

void ReplayFile::deobfuscate(std::span<std::uint8_t> file) {
    if (file.size() <= 0x10) return;
    std::uint8_t k = file[0x0D];
    for (std::size_t i = 0x10; i < file.size(); ++i) {
        file[i] = static_cast<std::uint8_t>(file[i] - k);
        k = static_cast<std::uint8_t>(k + 7);
    }
}

std::uint32_t ReplayFile::checksum(std::span<const std::uint8_t> plainFile) {
    std::uint32_t sum = kReplayChecksumBase;
    for (std::size_t i = 0x0D; i < plainFile.size(); ++i) sum += plainFile[i];
    return sum;
}

bool replayIntegrityOk(std::span<const std::uint8_t> verFile, std::string_view tag, std::int32_t exeSize, std::int32_t exeChecksum) {
    if (verFile.empty() || tag == "debug") return true;
    const auto line = findVersionLine(verFile, tag);
    return line && line->size == exeSize && line->checksum == exeChecksum;
}

std::optional<ReplayFile> ReplayFile::parse(std::span<const std::uint8_t> file, std::span<const std::uint8_t> verFile, std::string* error,
                                            bool headerOnly) {
    if (file.size() < kReplayHeaderSize) return fail(error, "too short"), std::nullopt;
    if (std::memcmp(file.data(), "T7RP", 4) != 0) return fail(error, "not a th07 replay"), std::nullopt;
    if (rd16(file, 0x04) != kReplayVersion) return fail(error, "wrong version"), std::nullopt;
    std::vector<std::uint8_t> plain(file.begin(), file.end());
    deobfuscate(plain);
    if (checksum(plain) != rd32(plain, 0x08)) return fail(error, "checksum mismatch"), std::nullopt;
    const std::uint32_t imageSize = rd32(plain, 0x10);
    const std::uint32_t packedSize = rd32(plain, 0x14);
    const std::uint32_t bodySize = rd32(plain, 0x18);
    if (bodySize < kReplaySummarySize || bodySize > kMaxImage || imageSize != kReplayHeaderSize + bodySize) {
        return fail(error, "bad sizes"), std::nullopt;
    }
    const std::size_t avail = plain.size() - kReplayHeaderSize;
    const std::span<const std::uint8_t> packed(plain.data() + kReplayHeaderSize, std::min<std::size_t>(packedSize, avail));
    std::vector<std::uint8_t> img(imageSize, 0);
    std::memcpy(img.data(), plain.data(), kReplayHeaderSize);
    if (lzss::decompress(packed, std::span(img).subspan(kReplayHeaderSize)) != bodySize) {
        return fail(error, "body does not decompress"), std::nullopt;
    }
    ReplayFile r;
    r.headerFiller = plain[0x0C];
    r.key = plain[0x0D];
    std::memcpy(r.summary.bytes.data(), img.data() + kReplayHeaderSize, kReplaySummarySize);
    if (r.summary.slowModeRecorded()) return fail(error, "recorded in slow mode"), std::nullopt;
    if (!replayIntegrityOk(verFile, r.summary.versionTag(), r.summary.exeSize(), r.summary.exeChecksum())) {
        return fail(error, "recorded by another version"), std::nullopt;
    }
    std::array<std::uint32_t, kReplayStages> stageAt{}, fpsAt{};
    std::vector<std::uint32_t> bounds;
    for (int i = 0; i < kReplayStages; ++i) {
        stageAt[static_cast<std::size_t>(i)] = rd32(img, 0x1C + 4 * static_cast<std::size_t>(i));
        fpsAt[static_cast<std::size_t>(i)] = rd32(img, 0x38 + 4 * static_cast<std::size_t>(i));
        for (std::uint32_t o : {stageAt[static_cast<std::size_t>(i)], fpsAt[static_cast<std::size_t>(i)]}) {
            if (o != 0 && (o < kReplayFirstBlock || o > imageSize)) return fail(error, "bad block offset"), std::nullopt;
            if (o != 0) bounds.push_back(o);
        }
    }
    bounds.push_back(imageSize);
    // A block runs to the next block of either kind (or the image's end): playback stops
    // there instead of reading into the following data.
    auto boundAfter = [&](std::uint32_t at) {
        std::uint32_t b = imageSize;
        for (std::uint32_t o : bounds) {
            if (o > at && o < b) b = o;
        }
        return b;
    };
    for (int i = 0; i < kReplayStages; ++i) {
        ReplayStage& s = r.stages[static_cast<std::size_t>(i)];
        const std::uint32_t at = stageAt[static_cast<std::size_t>(i)];
        if (at == 0) continue;
        if (at + kReplayBlockHeaderSize > imageSize) return fail(error, "stage block outside the image"), std::nullopt;
        s.present = true;
        readBlockHeader(img, at, s);
        if (headerOnly) continue;
        const std::uint32_t end = std::max<std::uint32_t>(boundAfter(at), at + kReplayBlockHeaderSize);
        const std::size_t n = (end - at - kReplayBlockHeaderSize) / 4;
        s.entries.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t e = at + kReplayBlockHeaderSize + 4 * k;
            s.entries[k] = {rd16(img, e), rd16(img, e + 2)};
        }
        const std::uint32_t f = fpsAt[static_cast<std::size_t>(i)];
        if (f != 0) s.fps.assign(img.begin() + f, img.begin() + boundAfter(f));
    }
    return r;
}

std::vector<std::uint8_t> ReplayFile::serialize(ZunRng& rng) {
    std::vector<std::uint8_t> img(kReplayHeaderSize, 0);
    std::array<std::uint32_t, kReplayStages> stageAt{}, fpsAt{};
    img.insert(img.end(), summary.bytes.begin(), summary.bytes.end());
    for (int i = 0; i < kReplayStages; ++i) {
        const ReplayStage& s = stages[static_cast<std::size_t>(i)];
        if (!s.present) continue;
        stageAt[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(img.size());
        putBlockHeader(img, s);
        for (const ReplayEntry& e : s.entries) {
            const std::size_t at = img.size();
            img.resize(at + 4);
            wr16(img, at, e.input);
            wr16(img, at + 2, e.flags);
        }
    }
    for (int i = 0; i < kReplayStages; ++i) {
        const ReplayStage& s = stages[static_cast<std::size_t>(i)];
        if (!s.present) continue;
        fpsAt[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(img.size());
        img.insert(img.end(), s.fps.begin(), s.fps.end());
    }
    // The key, then the summary's filler, then the header's.
    key = static_cast<std::uint8_t>(rng.u16InRange(128) + 64);
    summary.bytes[0x00] = static_cast<std::uint8_t>(rng.u16InRange(256));
    headerFiller = static_cast<std::uint8_t>(rng.u16InRange(256));
    img[kReplayHeaderSize] = summary.bytes[0x00];

    const std::uint32_t bodySize = static_cast<std::uint32_t>(img.size() - kReplayHeaderSize);
    const std::vector<std::uint8_t> packed = lzss::compress(std::span(img).subspan(kReplayHeaderSize));
    std::vector<std::uint8_t> out(kReplayHeaderSize, 0);
    std::memcpy(out.data(), "T7RP", 4);
    wr16(out, 0x04, kReplayVersion);
    out[0x0C] = headerFiller;
    out[0x0D] = key;
    wr32(out, 0x10, static_cast<std::uint32_t>(img.size()));
    wr32(out, 0x14, static_cast<std::uint32_t>(packed.size()));
    wr32(out, 0x18, bodySize);
    for (int i = 0; i < kReplayStages; ++i) {
        wr32(out, 0x1C + 4 * static_cast<std::size_t>(i), stageAt[static_cast<std::size_t>(i)]);
        wr32(out, 0x38 + 4 * static_cast<std::size_t>(i), fpsAt[static_cast<std::size_t>(i)]);
    }
    out.insert(out.end(), packed.begin(), packed.end());
    wr32(out, 0x08, checksum(out));
    obfuscate(out);
    return out;
}

// ---- the stage restore ----------------------------------------------------------------------

void restoreReplayStage(const World& w, const ReplayFile& file, int index, GameConfig& config) {
    if (!file.hasStage(index)) return;
    GameState& g = *w.state;
    const ReplayStage& s = file.stages[static_cast<std::size_t>(index)];
    g.extendPoints = static_cast<std::uint32_t>(s.extendPoints);
    g.rank = s.rank;  // set directly; the minimum and maximum stay the difficulty's
    w.setLives(static_cast<float>(s.lives));
    w.setBombs(static_cast<float>(s.bombs));
    w.setPower(static_cast<float>(s.power));
    g.totalGraze = static_cast<std::uint32_t>(s.totalGraze);
    g.fullPowerChain = s.chain;
    g.cherry = s.cherry;
    g.cherryMax = s.cherryMax;
    // (At or above the full value it is clamped; the player's stage start then readies the
    // border, as the original's second check does.)
    g.cherryPlus = std::min(s.cherryPlus, GameState::kCherryPlusFull);
    const std::span<const std::uint8_t> cfg = file.summary.config();
    // (load() would rebuild an out-of-range copy: the recorded life count and slow mode are
    // what the game uses.)
    config.load(cfg, true);
    config.lives = cfg[0x1C];
    config.slowMode = cfg[0x25];
    g.lifeCount = config.lives;
    g.slowMode = config.slowMode != 0;
    g.spellsCaptured = s.spells;
    g.extendsGranted = s.extendsGranted;
    g.nextExtend = s.nextExtend;
    if (index >= 1 && index <= 5 && file.hasStage(index - 1)) {
        g.score = g.guiScore = file.stages[static_cast<std::size_t>(index - 1)].endScore;
    }
    w.rng->seed(s.seed);
}

// ---- recording ------------------------------------------------------------------------------

void ReplayRecorder::create(int shot, int difficulty, const GameConfig& configCopy, const ExeIdentity& exe) {
    file_ = ReplayFile{};
    ReplaySummary& s = file_.summary;
    s.bytes[0x01] = 0x62;  // 'b'
    s.setShot(shot);
    s.setDifficulty(difficulty);
    std::memcpy(s.bytes.data() + 0x0A, "NO N", 4);  // unterminated while recording [QUIRK]
    wr16(s.bytes, 0x16, 0x0100);
    s.setConfig(configCopy.save());
    s.setExe(exe);
    created_ = true;
    current_ = -1;
    frame_ = 0;
    fpsCursor_ = 0;
}

void ReplayRecorder::startBlock(int index, const StageStart& v) {
    if (!created_ || index < 0 || index >= kReplayStages) return;
    frame_ = 0;
    if (current_ >= 0) file_.stages[static_cast<std::size_t>(current_)].endScore = v.score;
    ReplayStage& s = file_.stages[static_cast<std::size_t>(index)];
    s = ReplayStage{};
    s.present = true;
    s.extendPoints = v.extendPoints;
    s.cherry = v.cherry;
    s.cherryMax = v.cherryMax;
    s.cherryPlus = v.cherryPlus;
    s.totalGraze = v.totalGraze;
    s.extendsGranted = v.extendsGranted;
    s.nextExtend = v.nextExtend;
    s.seed = v.seed;
    s.power = static_cast<std::uint8_t>(v.power);
    s.lives = static_cast<std::uint8_t>(v.lives);
    s.bombs = static_cast<std::uint8_t>(v.bombs);
    s.rank = static_cast<std::uint8_t>(v.rank);
    s.chain = static_cast<std::uint8_t>(v.chain);
    s.spells = static_cast<std::uint8_t>(v.spells);
    s.entries.reserve(4096);
    s.entries.push_back({0, 0});  // entry 0
    fpsCursor_ = 0;
    current_ = index;
}

void ReplayRecorder::write(std::uint16_t raw, std::uint16_t flags, int fps, bool timingErrors) {
    if (!created_ || current_ < 0) return;
    ReplayStage& s = file_.stages[static_cast<std::size_t>(current_)];
    // (The original's buffer is not checked: the port stops at its size.)
    if (s.entries.size() + 3 > kReplayMaxEntries) return;
    s.entries.push_back({raw, flags});
    if (frame_ % 30 == 0) {
        if (s.fps.size() < fpsCursor_ + 2) s.fps.resize(fpsCursor_ + 2);
        s.fps[fpsCursor_] = static_cast<std::uint8_t>((fps & 0x7F) | (timingErrors ? 0x80 : 0));
        s.fps[fpsCursor_ + 1] = static_cast<std::uint8_t>(fps & 0x7F);
        ++fpsCursor_;
    }
    ++frame_;
}

void ReplayRecorder::closeBlock() {
    if (!created_ || current_ < 0) return;
    file_.stages[static_cast<std::size_t>(current_)].entries.push_back({0, 0});
}

std::vector<std::uint8_t> ReplayRecorder::save(std::uint32_t score, std::uint32_t guiScore, float slowdown, std::string_view name,
                                               std::string_view date, ZunRng& rng) {
    if (!created_) return {};
    ReplayFile f = file_;  // a working copy
    if (current_ >= 0) {
        ReplayStage& last = f.stages[static_cast<std::size_t>(current_)];
        last.entries.push_back({0, 0});  // the second terminator [QUIRK]
        last.endScore = score;
    }
    f.summary.setScore(guiScore);
    f.summary.setSlowdown(slowdown);
    f.summary.setName(name);
    f.summary.setDate(date);
    return f.serialize(rng);
}

void ReplayRecorder::drop() {
    file_ = ReplayFile{};
    created_ = false;
    current_ = -1;
    frame_ = 0;
    fpsCursor_ = 0;
}

// ---- playback -------------------------------------------------------------------------------

void ReplayFeed::start(const ReplayStage* stage) {
    stage_ = stage;
    cursor_ = 0;
    frame_ = 0;
    fps_ = 60;
    fpsBad_ = false;
    pastEnd_ = false;
    last_ = {};
}

std::uint16_t ReplayFeed::next() {
    if (!stage_) return 0;
    if (cursor_ < stage_->entries.size()) {
        last_ = stage_->entries[cursor_++];
    } else {
        last_ = {};
        pastEnd_ = true;
    }
    if (frame_ % 30 == 0) {
        // The next sample's byte, one sample later than recorded [QUIRK].
        const std::size_t at = static_cast<std::size_t>(frame_ / 30) + 1;
        if (at < stage_->fps.size()) {
            fps_ = stage_->fps[at] & 0x7F;
            fpsBad_ = (stage_->fps[at] & 0x80) != 0;
        }
    }
    ++frame_;
    return last_.input;
}

}  // namespace touhou::th07
