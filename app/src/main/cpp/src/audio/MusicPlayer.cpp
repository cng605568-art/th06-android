#include "audio/MusicPlayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Log.hpp"
#include "game/World.hpp"
#include "io/MappedFile.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

template <class T>
T rd(std::span<const std::uint8_t> b, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

}  // namespace

bool MusicStream::open(std::span<const std::uint8_t> wav, std::int64_t loopStart, std::int64_t loopEnd, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return fail("not a RIFF/WAVE file");
    }
    bool haveFormat = false;
    std::size_t dataAt = 0, dataSize = 0;
    int bits = 0;
    for (std::size_t at = 12; at + 8 <= wav.size();) {
        const std::size_t size = rd<std::uint32_t>(wav, at + 4);
        if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && size >= 16) {
            const auto tag = rd<std::uint16_t>(wav, at + 8);
            channels_ = rd<std::uint16_t>(wav, at + 10);
            rate_ = static_cast<int>(rd<std::uint32_t>(wav, at + 12));
            bits = rd<std::uint16_t>(wav, at + 22);
            if (tag != 1 && tag != 0xfffe) return fail("music must be PCM");
            haveFormat = true;
        } else if (std::memcmp(wav.data() + at, "data", 4) == 0) {
            dataAt = at + 8;
            dataSize = std::min(size, wav.size() - dataAt);
            break;
        }
        at += 8 + size + (size & 1);
    }
    if (!haveFormat || dataSize == 0) return fail("no format or no data");
    if (bits != 16 || channels_ < 1 || channels_ > 2 || rate_ < 8000 || rate_ > 192000) {
        return fail("music must be 16-bit mono or stereo PCM");
    }
    data_ = wav.data() + dataAt;
    totalFrames_ = dataSize / (2 * static_cast<std::size_t>(channels_));
    if (totalFrames_ == 0) return fail("empty data");
    // Loop end is absolute; past the data it is clamped; an end at or before the start loops
    // the whole song (the original would run off the data there).
    loopEnd_ = loopEnd > 0 ? std::min<std::size_t>(static_cast<std::size_t>(loopEnd), totalFrames_) : totalFrames_;
    loopStart_ = loopStart > 0 ? static_cast<std::size_t>(loopStart) : 0;
    if (loopStart_ >= loopEnd_) loopStart_ = 0;
    pos_ = 0;
    return true;
}

void MusicStream::mixInto(std::int32_t* acc, std::size_t frames, int outRate) {
    if (!data_) return;
    const std::uint64_t step = (static_cast<std::uint64_t>(rate_) << 32) / static_cast<std::uint64_t>(std::max(outRate, 1));
    const auto* pcm = reinterpret_cast<const std::uint8_t*>(data_);
    auto sample = [&](std::size_t frame, int ch) {
        std::int16_t v;
        std::memcpy(&v, pcm + (frame * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(ch)) * 2, 2);
        return static_cast<std::int32_t>(v);
    };
    for (std::size_t i = 0; i < frames; ++i) {
        std::size_t f = static_cast<std::size_t>(pos_ >> 32);
        if (f >= loopEnd_) {  // end of a pass: on to the loop start, sample-exact
            pos_ -= static_cast<std::uint64_t>(loopEnd_ - loopStart_) << 32;
            f = static_cast<std::size_t>(pos_ >> 32);
        }
        const std::int32_t l = sample(f, 0), r = channels_ == 2 ? sample(f, 1) : l;
        acc[2 * i] += (l * gainQ15_) >> 15;
        acc[2 * i + 1] += (r * gainQ15_) >> 15;
        pos_ += step;
    }
}

std::string MusicPlayer::withExtension(std::string_view path, std::string_view ext) {
    std::string out(path);
    const auto dot = out.find_last_of('.');
    if (dot == std::string::npos) return out + "." + std::string(ext);
    for (std::size_t i = 0; i < 3 && i < ext.size(); ++i) {
        if (dot + 1 + i < out.size()) out[dot + 1 + i] = ext[i];
        else out.push_back(ext[i]);
    }
    return out;
}

MusicPlayer::MusicPlayer(const Vfs* vfs, MusicBackend* backend, UnportedFeatures* unported)
    : vfs_(vfs), backend_(backend), unported_(unported) {}

MusicPlayer::~MusicPlayer() { stop(); }

bool MusicPlayer::play(std::string_view songPath) {
    stop();
    override_.reset();  // (the stream reading it is gone)
    current_ = std::string(songPath);
    if (!enabled_ || !backend_ || songPath.empty()) return false;
    std::span<const std::uint8_t> wav;
    std::int64_t loopStart = 0, loopEnd = 0;
    // A replacement from the launcher comes first: <override>/th06_NN.wav (+ .pos).
    if (!overrideDir_.empty()) {
        std::string name = withExtension(songPath, "wav");
        if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name.erase(0, slash + 1);
        std::string err;
        if (auto file = MappedFile::open(overrideDir_ + name, &err)) {
            log::info("Music: {} (your replacement)", name);
            override_ = std::move(file);
            wav = override_->bytes();
            if (auto pos = MappedFile::open(overrideDir_ + withExtension(name, "pos"), &err); pos && pos->bytes().size() >= 8) {
                loopStart = rd<std::int32_t>(pos->bytes(), 0);
                loopEnd = rd<std::int32_t>(pos->bytes(), 4);
            }
        }
    }
    if (wav.empty()) {
        if (!vfs_) return false;
        wav = vfs_->looseFile(withExtension(songPath, "wav"));
        if (wav.empty()) {
            if (unported_) unported_->hit("background music (needs the bgm folder with th06_NN.wav)");
            return false;
        }
        if (auto pos = vfs_->read(withExtension(songPath, "pos")); pos && pos->size() >= 8) {
            loopStart = rd<std::int32_t>(*pos, 0);
            loopEnd = rd<std::int32_t>(*pos, 4);
        }
    }
    auto stream = std::make_unique<MusicStream>();
    std::string err;
    if (!stream->open(wav, loopStart, loopEnd, &err)) {
        log::warn("Music {}: {}", songPath, err);
        override_.reset();
        return false;
    }
    gain_ = 1.0f;
    backend_->setMusicGain(1.0f);
    backend_->startMusic(std::move(stream));
    playing_ = true;
    return true;
}

void MusicPlayer::stop() {
    fadeFrames_ = fadeLeft_ = 0;
    if (playing_ && backend_) backend_->stopMusic();
    playing_ = false;
}

void MusicPlayer::fadeOut(float seconds) {
    if (!playing_) return;
    fadeFrames_ = static_cast<std::int32_t>(60.0f * seconds);
    fadeLeft_ = fadeFrames_;
    if (fadeFrames_ <= 0) stop();
}

void MusicPlayer::tick() {
    if (fadeFrames_ <= 0 || !playing_) return;
    if (--fadeLeft_ <= 0) {  // faded out: the song stops
        stop();
        return;
    }
    // Linear in decibels: from 0 dB down to -50 dB (DirectSound hundredths of a dB).
    const float centiBels = 5000.0f * static_cast<float>(fadeLeft_) / static_cast<float>(fadeFrames_) - 5000.0f;
    gain_ = std::pow(10.0f, centiBels / 2000.0f);
    if (backend_) backend_->setMusicGain(gain_);
}

}  // namespace th06
