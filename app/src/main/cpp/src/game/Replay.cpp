#include "game/Replay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace th06 {
namespace {

constexpr std::size_t kHeaderSize = 0x50, kBlockHeader = 0x10, kRecordSize = 8, kDragSize = 12;
constexpr std::size_t kOffsetKey = 0x0E, kOffsetObfuscated = 0x0F, kOffsetStages = 0x34;
constexpr std::uint16_t kVersion = 0x0102;
constexpr std::uint32_t kChecksumBase = 0x3F000318u;
constexpr char kMagic[4] = {'T', '6', 'R', 'P'};
constexpr char kExtensionMagic[4] = {'T', '6', 'X', 'D'};
constexpr std::uint16_t kExtensionVersion = 1;

std::uint32_t readU32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
std::uint16_t readU16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}
float readF32(std::span<const std::uint8_t> b, std::size_t at) {
    const std::uint32_t u = readU32(b, at);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

void putU32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
void putU16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void putF32(std::vector<std::uint8_t>& b, std::size_t at, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    putU32(b, at, u);
}
void appendU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.resize(b.size() + 4);
    putU32(b, b.size() - 4, v);
}
void appendRecord(std::vector<std::uint8_t>& b, std::int32_t frame, std::uint16_t buttons) {
    appendU32(b, static_cast<std::uint32_t>(frame));
    b.push_back(static_cast<std::uint8_t>(buttons));
    b.push_back(static_cast<std::uint8_t>(buttons >> 8));
    b.push_back(0);
    b.push_back(0);
}

std::uint32_t checksum(std::span<const std::uint8_t> plain) {
    std::uint32_t sum = kChecksumBase;
    for (std::size_t i = kOffsetKey; i < plain.size(); ++i) sum += plain[i];
    return sum;
}

// The running additive key: K at 0x0F, +7 for every following byte.
void obfuscate(std::vector<std::uint8_t>& b, std::uint8_t key, bool decode) {
    std::uint8_t k = key;
    for (std::size_t i = kOffsetObfuscated; i < b.size(); ++i) {
        b[i] = static_cast<std::uint8_t>(decode ? b[i] - k : b[i] + k);
        k = static_cast<std::uint8_t>(k + 7);
    }
}

}  // namespace

std::uint16_t replayButtons(const InputState& in) {
    return static_cast<std::uint16_t>((in.shoot ? kReplayShoot : 0) | (in.bomb ? kReplayBomb : 0) | (in.focus ? kReplayFocus : 0) |
                                      (in.up ? kReplayUp : 0) | (in.down ? kReplayDown : 0) | (in.left ? kReplayLeft : 0) |
                                      (in.right ? kReplayRight : 0) | (in.skip ? kReplaySkip : 0));
}

void applyReplayButtons(InputState& in, std::uint16_t b) {
    in.shoot = (b & kReplayShoot) != 0;
    in.bomb = (b & kReplayBomb) != 0;
    in.focus = (b & kReplayFocus) != 0;
    in.up = (b & kReplayUp) != 0;
    in.down = (b & kReplayDown) != 0;
    in.left = (b & kReplayLeft) != 0;
    in.right = (b & kReplayRight) != 0;
    in.skip = (b & kReplaySkip) != 0;
}

void quantizeDrag(InputState& in) {
    if (!in.drag) return;
    auto q = [](float v) {
        const float scaled = std::clamp(std::round(v * kReplayDragScale), -32768.0f, 32767.0f);
        return scaled / kReplayDragScale;
    };
    in.dragX = q(in.dragX);
    in.dragY = q(in.dragY);
}

// ---- the file ---------------------------------------------------------------------------

std::optional<Replay> Replay::parse(std::span<const std::uint8_t> bytes, std::string* error) {
    auto bad = [error](const char* why) {
        if (error) *error = why;
        return std::optional<Replay>{};
    };
    const std::size_t size = bytes.size();
    if (size < kHeaderSize) return bad("too short");
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return bad("not a replay");
    std::vector<std::uint8_t> b(bytes.begin(), bytes.end());
    obfuscate(b, b[kOffsetKey], true);
    if (checksum(b) != readU32(b, 0x08)) return bad("checksum mismatch");
    if (readU16(b, 0x04) != kVersion) return bad("another game version");
    Replay r;
    r.shotType = b[0x06];
    r.difficulty = b[0x07];
    if (r.shotType >= 4 || r.difficulty >= 5) return bad("bad shot type or difficulty");
    for (std::size_t i = 0x10; i < 0x18 && b[i] != 0; ++i) r.date.push_back(static_cast<char>(b[i]));
    r.name.assign(reinterpret_cast<const char*>(&b[0x19]), 8);
    r.score = readU32(b, 0x24);
    r.slowdown = readF32(b, 0x2C);
    if (!std::isfinite(r.slowdown)) r.slowdown = 0.0f;

    // The port's drag chunk, if any, ends the data.
    std::size_t dataEnd = size, extension = 0;
    if (size >= kHeaderSize + 8 && std::memcmp(&b[size - 4], kExtensionMagic, 4) == 0) {
        const std::size_t at = readU32(b, size - 8);
        if (at >= kHeaderSize && at + 4 <= size - 8) {
            extension = at;
            dataEnd = at;
        }
    }
    std::array<std::size_t, kReplayStages> offsets{};
    for (int s = 0; s < kReplayStages; ++s) {
        const std::size_t o = readU32(b, kOffsetStages + 4 * static_cast<std::size_t>(s));
        if (o == 0) continue;
        if (o < kHeaderSize || o + kBlockHeader + kRecordSize > dataEnd) return bad("bad stage offset");
        offsets[static_cast<std::size_t>(s)] = o;
    }
    bool any = false;
    for (int s = 0; s < kReplayStages; ++s) {
        const std::size_t o = offsets[static_cast<std::size_t>(s)];
        if (o == 0) continue;
        std::size_t end = dataEnd;  // up to the next block (blocks carry no length)
        for (std::size_t other : offsets) {
            if (other > o) end = std::min(end, other);
        }
        ReplayStage& st = r.stages[static_cast<std::size_t>(s)];
        st.present = true;
        st.endScore = static_cast<std::int32_t>(readU32(b, o));
        st.seed = readU16(b, o + 4);
        st.pointItems = static_cast<std::int16_t>(readU16(b, o + 6));
        st.power = b[o + 8];
        st.lives = static_cast<std::int8_t>(b[o + 9]);
        st.bombs = static_cast<std::int8_t>(b[o + 10]);
        st.rank = b[o + 11];
        st.powerItemStep = static_cast<std::int8_t>(b[o + 12]);
        for (std::size_t at = o + kBlockHeader; at + kRecordSize <= end; at += kRecordSize) {
            const auto frame = static_cast<std::int32_t>(readU32(b, at));
            if (frame == kReplaySentinel) break;
            st.records.push_back({frame, static_cast<std::uint16_t>(readU16(b, at + 4) & kReplayMask)});
        }
        // The terminator {F, 0} before the sentinel gives the length.
        if (st.records.size() >= 2 && st.records.back().buttons == 0) {
            st.frames = st.records.back().frame;
            st.records.pop_back();
        } else {
            st.frames = st.records.empty() ? 0 : st.records.back().frame + 1;
        }
        if (st.records.empty() || st.records.front().frame != 0) st.records.insert(st.records.begin(), {0, 0});
        any = true;
    }
    if (!any) return bad("no stages");

    if (extension != 0) {
        std::size_t at = extension;
        const std::uint16_t version = readU16(b, at);
        const std::uint16_t count = readU16(b, at + 2);
        at += 4;
        for (int n = 0; version == kExtensionVersion && n < count && at + 8 <= size - 8; ++n) {
            const int s = b[at];
            const std::uint32_t records = readU32(b, at + 4);
            at += 8;
            if (s >= kReplayStages || records > (size - 8 - at) / kDragSize) break;
            ReplayStage& st = r.stages[static_cast<std::size_t>(s)];
            for (std::uint32_t i = 0; i < records; ++i, at += kDragSize) {
                ReplayDrag d;
                d.frame = static_cast<std::int32_t>(readU32(b, at));
                d.active = (b[at + 4] & 1) != 0;
                d.newTouch = (b[at + 4] & 2) != 0;
                d.x = static_cast<std::int16_t>(readU16(b, at + 6));
                d.y = static_cast<std::int16_t>(readU16(b, at + 8));
                if (st.present) st.drags.push_back(d);
            }
        }
    }
    return r;
}

std::vector<std::uint8_t> Replay::serialize(ZunRng& rng) const {
    std::vector<std::uint8_t> b(kHeaderSize, 0);
    std::memcpy(b.data(), kMagic, 4);
    putU16(b, 0x04, kVersion);
    b[0x06] = static_cast<std::uint8_t>(shotType);
    b[0x07] = static_cast<std::uint8_t>(difficulty);
    for (std::size_t i = 0; i < 8 && i < date.size(); ++i) b[0x10 + i] = static_cast<std::uint8_t>(date[i]);
    for (std::size_t i = 0; i < 8; ++i) b[0x19 + i] = static_cast<std::uint8_t>(i < name.size() ? name[i] : ' ');
    putU32(b, 0x24, score);
    putF32(b, 0x28, slowdown + 1.12f);
    putF32(b, 0x2C, slowdown);
    putF32(b, 0x30, slowdown + 2.34f);
    const int last = lastStage();
    bool drags = false;
    for (int s = 0; s < kReplayStages; ++s) {
        const ReplayStage& st = stages[static_cast<std::size_t>(s)];
        if (!st.present) continue;
        drags |= !st.drags.empty();
        putU32(b, kOffsetStages + 4 * static_cast<std::size_t>(s), static_cast<std::uint32_t>(b.size()));
        const std::size_t o = b.size();
        b.resize(o + kBlockHeader, 0);
        putU32(b, o, static_cast<std::uint32_t>(st.endScore));
        putU16(b, o + 4, st.seed);
        putU16(b, o + 6, static_cast<std::uint16_t>(st.pointItems));
        b[o + 8] = st.power;
        b[o + 9] = static_cast<std::uint8_t>(st.lives);
        b[o + 10] = static_cast<std::uint8_t>(st.bombs);
        b[o + 11] = st.rank;
        b[o + 12] = static_cast<std::uint8_t>(st.powerItemStep);
        if (st.records.empty() || st.records.front().frame != 0) appendRecord(b, 0, 0);
        for (const ReplayRecord& rec : st.records) appendRecord(b, rec.frame, rec.buttons);
        for (int pair = 0; pair < (s == last ? 2 : 1); ++pair) {  // the last stage's terminator is doubled
            appendRecord(b, st.frames, 0);
            appendRecord(b, kReplaySentinel, 0);
        }
    }
    if (drags) {
        const std::size_t at = b.size();
        int count = 0;
        for (const ReplayStage& st : stages) count += st.present && !st.drags.empty() ? 1 : 0;
        b.resize(at + 4, 0);
        putU16(b, at, kExtensionVersion);
        putU16(b, at + 2, static_cast<std::uint16_t>(count));
        for (int s = 0; s < kReplayStages; ++s) {
            const ReplayStage& st = stages[static_cast<std::size_t>(s)];
            if (!st.present || st.drags.empty()) continue;
            const std::size_t h = b.size();
            b.resize(h + 8, 0);
            b[h] = static_cast<std::uint8_t>(s);
            putU32(b, h + 4, static_cast<std::uint32_t>(st.drags.size()));
            for (const ReplayDrag& d : st.drags) {
                const std::size_t r = b.size();
                b.resize(r + kDragSize, 0);
                putU32(b, r, static_cast<std::uint32_t>(d.frame));
                b[r + 4] = static_cast<std::uint8_t>((d.active ? 1 : 0) | (d.newTouch ? 2 : 0));
                putU16(b, r + 6, static_cast<std::uint16_t>(d.x));
                putU16(b, r + 8, static_cast<std::uint16_t>(d.y));
            }
        }
        appendU32(b, static_cast<std::uint32_t>(at));
        b.insert(b.end(), kExtensionMagic, kExtensionMagic + 4);
    }
    // The key and the three fillers, in the original's order of draws.
    const auto key = static_cast<std::uint8_t>(rng.u16() % 128 + 64);
    b[kOffsetKey] = key;
    b[0x0F] = static_cast<std::uint8_t>(rng.u16() % 256);
    b[0x0C] = static_cast<std::uint8_t>(rng.u16() % 256);
    b[0x0D] = static_cast<std::uint8_t>(rng.u16() % 256);
    putU32(b, 0x08, checksum(b));
    obfuscate(b, key, false);
    return b;
}

int Replay::firstStage() const {
    for (int s = 0; s < kReplayStages; ++s) {
        if (stages[static_cast<std::size_t>(s)].present) return s;
    }
    return -1;
}

int Replay::lastStage() const {
    for (int s = kReplayStages - 1; s >= 0; --s) {
        if (stages[static_cast<std::size_t>(s)].present) return s;
    }
    return -1;
}

const char* Replay::shotName(int shotType) {
    static constexpr const char* kNames[4] = {"ReimuA ", "ReimuB ", "MarisaA", "MarisaB"};
    return kNames[std::clamp(shotType, 0, 3)];
}

const char* Replay::difficultyName(int difficulty) {
    static constexpr const char* kNames[5] = {"Easy   ", "Normal ", "Hard   ", "Lunatic", "Extra  "};
    return kNames[std::clamp(difficulty, 0, 4)];
}

// ---- playback and recording ---------------------------------------------------------

void ReplayCursor::start(const ReplayStage* stage) {
    stage_ = stage;
    record_ = drag_ = 0;
    frame_ = 0;
    current_ = ReplayDrag{};
    dragId_ = 0;
}

InputState ReplayCursor::next(const InputState& live) {
    InputState out = live;
    out.drag = false;
    out.dragId = 0;
    out.dragX = out.dragY = 0.0f;
    out.saveState = out.loadState = false;
    if (!stage_ || stage_->records.empty()) {
        applyReplayButtons(out, 0);
        ++frame_;
        return out;
    }
    const std::vector<ReplayRecord>& r = stage_->records;
    while (record_ + 1 < r.size() && r[record_ + 1].frame <= frame_) ++record_;
    const std::vector<ReplayDrag>& d = stage_->drags;
    while (drag_ < d.size() && d[drag_].frame <= frame_) {
        current_ = d[drag_];
        if (current_.newTouch) ++dragId_;
        ++drag_;
    }
    if (frame_ < stage_->frames) {
        applyReplayButtons(out, r[record_].buttons);
        if (current_.active) {
            out.drag = true;
            out.dragId = dragId_;
            out.dragX = static_cast<float>(current_.x) / kReplayDragScale;
            out.dragY = static_cast<float>(current_.y) / kReplayDragScale;
        }
    } else {
        applyReplayButtons(out, 0);  // past the recorded end: nothing held
    }
    ++frame_;
    return out;
}

void ReplayRecorder::start(ReplayStage* stage) {
    stage_ = stage;
    frame_ = 0;
    last_ = 0;
    lastDrag_ = ReplayDrag{};
    lastDragId_ = 0;
    if (!stage_) return;
    stage_->records.assign(1, ReplayRecord{0, 0});
    stage_->drags.clear();
    stage_->frames = 0;
}

void ReplayRecorder::sample(const InputState& in) {
    if (!stage_) return;
    const std::uint16_t b = replayButtons(in);
    if (b != last_) {
        stage_->records.push_back({frame_, b});
        last_ = b;
    }
    ReplayDrag d;
    d.frame = frame_;
    d.active = in.drag;
    if (in.drag) {
        d.newTouch = in.dragId != lastDragId_;
        d.x = static_cast<std::int16_t>(std::lround(std::clamp(in.dragX * kReplayDragScale, -32768.0f, 32767.0f)));
        d.y = static_cast<std::int16_t>(std::lround(std::clamp(in.dragY * kReplayDragScale, -32768.0f, 32767.0f)));
        lastDragId_ = in.dragId;
    }
    if (d.newTouch || d.active != lastDrag_.active || (d.active && (d.x != lastDrag_.x || d.y != lastDrag_.y))) {
        stage_->drags.push_back(d);
        lastDrag_ = d;
    }
    ++frame_;
}

void ReplayRecorder::stop() {
    if (stage_) stage_->frames = frame_;
}

}  // namespace th06
