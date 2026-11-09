#include "th07/Bgm.hpp"

#include <cmath>
#include <cstring>

#include "audio/MusicPlayer.hpp"
#include "core/Log.hpp"

namespace touhou::th07 {
namespace {

constexpr std::size_t kRecordSize = 0x34;

std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}
std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

// The name part of a song path: after the last '/', else after the last '\', else all.
std::string_view songName(std::string_view path) {
    std::size_t cut = path.rfind('/');
    if (cut == std::string_view::npos) cut = path.rfind('\\');
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

}  // namespace

std::vector<BgmRecord> parseThbgmFmt(std::span<const std::uint8_t> fmt) {
    std::vector<BgmRecord> out;
    for (std::size_t at = 0; at + kRecordSize <= fmt.size(); at += kRecordSize) {
        if (fmt[at] == 0) break;
        BgmRecord r;
        std::size_t n = 0;
        while (n < 16 && fmt[at + n] != 0) ++n;  // a name filling all 16 bytes has no terminator
        r.name.assign(reinterpret_cast<const char*>(fmt.data() + at), n);
        r.start = static_cast<std::int32_t>(le32(fmt, at + 0x10));
        r.preloadSize = le32(fmt, at + 0x14);
        r.intro = static_cast<std::int32_t>(le32(fmt, at + 0x18));
        r.total = static_cast<std::int32_t>(le32(fmt, at + 0x1C));
        r.formatTag = le16(fmt, at + 0x20);
        r.channels = le16(fmt, at + 0x22);
        r.sampleRate = le32(fmt, at + 0x24);
        r.bytesPerSecond = le32(fmt, at + 0x28);
        r.blockAlign = le16(fmt, at + 0x2C);
        r.bitsPerSample = le16(fmt, at + 0x2E);
        out.push_back(std::move(r));
    }
    return out;
}

bool validThbgmHeader(std::span<const std::uint8_t> thbgm) {
    return thbgm.size() >= 16 && std::memcmp(thbgm.data(), "ZWAV", 4) == 0 && le32(thbgm, 4) == 1 &&
           le32(thbgm, 8) == 0x700;
}

BgmPlayer::BgmPlayer(MusicBackend* backend) : backend_(backend) {}

BgmPlayer::~BgmPlayer() {
    if (backend_ && playing_) backend_->stopMusic();
}

void BgmPlayer::setData(std::vector<BgmRecord> records, std::span<const std::uint8_t> thbgm) {
    records_ = std::move(records);
    thbgm_ = thbgm;
    if (records_.empty()) return;
    // Every song plays in record 0's format (the original sets its buffer up from it once).
    const BgmRecord& r0 = records_.front();
    for (const BgmRecord& r : records_) {
        if (r.channels != r0.channels || r.sampleRate != r0.sampleRate || r.blockAlign != r0.blockAlign ||
            r.bitsPerSample != r0.bitsPerSample) {
            log::warn("thbgm.fmt: {} is not in {}'s format; it plays in that format, as on PC", r.name, r0.name);
        }
    }
}

const BgmRecord* BgmPlayer::resolve(std::string_view path) const {
    if (records_.empty()) return nullptr;
    const std::string_view name = songName(path);
    for (const BgmRecord& r : records_) {
        if (r.name == name) return &r;
    }
    return &records_.front();  // a name not in the table plays the first song
}

void BgmPlayer::push(Command c) {
    if (queue_.size() < kQueueCapacity) queue_.push_back(std::move(c));
}

void BgmPlayer::load(int slot, std::string_view path) {
    if (mode_ == Off || slot < 0 || slot >= kSlots) return;
    slots_[slot] = MusicPlayer::withExtension(path, "wav");
}

void BgmPlayer::playLoaded(int slot) {
    if (mode_ == Off || slot < 0 || slot >= kSlots) return;
    push(Command(Op::Start, slot));
}

void BgmPlayer::play(std::string_view path) {
    if (mode_ == Off) return;
    push(Command(Op::Start, -1, MusicPlayer::withExtension(path, "wav")));
}

void BgmPlayer::stop() {
    if (mode_ != Off) push(Command(Op::Stop));
}

void BgmPlayer::shutdown() {
    if (mode_ != Off) push(Command(Op::Shutdown));
}

void BgmPlayer::fadeOut(float seconds, float speed) {
    if (mode_ == Off) return;
    // Slow motion stretches the fade; the length is then truncated to whole seconds.
    const float s = speed == 0.0f || speed > 1.0f ? seconds : seconds / speed;
    push(Command(Op::Fade, static_cast<int>(s)));
}

// The original pauses only in WAV mode; its MIDI music played on through the menus.
void BgmPlayer::pause() {
    if (mode_ == Wav) push(Command(Op::Pause));
}

void BgmPlayer::resume() {
    if (mode_ == Wav) push(Command(Op::Resume));
}

void BgmPlayer::setVolume(int centibels) {
    volume_ = centibels;
    if (backend_) backend_->setMusicGain(static_cast<float>(std::pow(10.0, centibels / 2000.0)));
}

void BgmPlayer::fadeStep() {
    if (!fading_) return;
    if (--fadeLeft_ <= 0) {
        // The fade ends by halting the output; the song is not marked stopped, so a later
        // resume continues it (at the last, all but silent, volume).
        fading_ = false;
        if (backend_) backend_->pauseMusic(true);
        return;
    }
    setVolume(fadeLeft_ * 5000 / fadeTotal_ - 5000);
}

void BgmPlayer::startSong(const std::string& path) {
    prepared_.reset();
    const BgmRecord* rec = resolve(path);
    if (!rec) return;
    const BgmRecord& r0 = records_.front();
    const int align = r0.blockAlign > 0 ? r0.blockAlign : 4;
    if (r0.bitsPerSample != 16 || rec->start < 0 || rec->total <= 0 ||
        static_cast<std::uint64_t>(rec->start) + static_cast<std::uint64_t>(rec->total) > thbgm_.size()) {
        log::warn("thbgm: {} cannot be played (outside thbgm.dat, or not 16-bit)", rec->name);
        return;
    }
    auto stream = std::make_unique<MusicStream>();
    std::string err;
    const std::int64_t loopStart = rec->intro > 0 ? rec->intro / align : 0;
    if (!stream->openPcm(thbgm_.subspan(static_cast<std::size_t>(rec->start), static_cast<std::size_t>(rec->total)),
                         r0.channels, static_cast<int>(r0.sampleRate), loopStart, rec->total / align, &err)) {
        log::warn("thbgm: {}: {}", rec->name, err);
        return;
    }
    current_ = rec->name;
    prepared_ = std::move(stream);
    log::debug("thbgm: {} (offset {}, intro {} bytes, {} bytes)", rec->name, rec->start, rec->intro, rec->total);
}

bool BgmPlayer::pump() {
    if (queue_.empty()) return false;
    Command& c = queue_.front();
    bool done = false;
    switch (c.op) {
        case Op::Start:
            if (!hasStreamer()) {
                done = true;
                break;
            }
            switch (c.step) {
                case 0:  // stop what is playing
                    if (backend_ && playing_) backend_->stopMusic();
                    playing_ = paused_ = false;
                    break;
                case 2:  // point the reader at the song
                    if (c.arg >= 0) c.path = slots_[c.arg];
                    break;
                case 3:  // prefill
                    startSong(c.path);
                    break;
                case 4:  // play, at full volume; a fade in progress is cancelled
                    fading_ = false;
                    if (prepared_ && backend_) {
                        backend_->startMusic(std::move(prepared_));
                        playing_ = true;
                    }
                    setVolume(0);
                    break;
                case 7:
                    done = true;
                    break;
                default:
                    break;
            }
            break;
        case Op::Stop:
            if (!hasStreamer() || c.step == 1) {
                done = true;
            } else {
                if (backend_ && playing_) backend_->stopMusic();
                playing_ = paused_ = false;
            }
            break;
        case Op::Shutdown:
            if (!hasStreamer() || c.step == 10) {
                done = true;
            } else if (c.step == 0) {
                if (backend_ && playing_) backend_->stopMusic();
                playing_ = paused_ = false;
            }
            break;
        case Op::Fade:
            fadeTotal_ = fadeLeft_ = c.arg * 60;
            fading_ = true;
            done = true;
            break;
        case Op::Pause:
        case Op::Resume:
            if (hasStreamer() && backend_) {
                paused_ = c.op == Op::Pause;
                backend_->pauseMusic(paused_);
            }
            done = true;
            break;
    }
    if (done) queue_.pop_front();
    else ++c.step;
    return !queue_.empty();
}

void BgmPlayer::drain() {
    while (pump()) {
    }
}

}  // namespace touhou::th07
