#include "audio/AudioMixer.hpp"

#include <algorithm>
#include <cstring>

#include "core/Log.hpp"

namespace touhou {

AudioMixer::~AudioMixer() { close(); }

bool AudioMixer::open(std::string* error) {
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;  // ~11.6 ms per callback
    want.callback = &AudioMixer::callback;
    want.userdata = this;
    // allowed_changes = 0: SDL converts internally if the hardware wants something else.
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &spec_, 0);
    if (device_ == 0) {
        if (error) *error = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
        return false;
    }
    scratch_.assign(static_cast<std::size_t>(spec_.samples) * spec_.channels * 4, 0);
    SDL_PauseAudioDevice(device_, 0);
    log::info("Audio: {} Hz, {} ch, {} samples per buffer", spec_.freq, spec_.channels, spec_.samples);
    return true;
}

void AudioMixer::close() {
    if (device_) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
}

bool AudioMixer::loadSample(int sampleIndex, const std::uint8_t* wav, std::size_t size) {
    if (!device_ || sampleIndex < 0 || sampleIndex >= kMaxSamples) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(wav, static_cast<int>(size));
    SDL_AudioSpec src{};
    Uint8* buffer = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV_RW(rw, 1, &src, &buffer, &length)) {
        log::warn("Sample {}: {}", sampleIndex, SDL_GetError());
        return false;
    }
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, src.format, src.channels, src.freq, AUDIO_S16SYS, 2, spec_.freq) < 0) {
        SDL_FreeWAV(buffer);
        return false;
    }
    std::vector<Uint8> work(static_cast<std::size_t>(length) * static_cast<std::size_t>(std::max(1, cvt.len_mult)));
    std::memcpy(work.data(), buffer, length);
    SDL_FreeWAV(buffer);
    cvt.buf = work.data();
    cvt.len = static_cast<int>(length);
    if (cvt.needed && SDL_ConvertAudio(&cvt) < 0) return false;
    const std::size_t bytes = cvt.needed ? static_cast<std::size_t>(cvt.len_cvt) : length;
    std::vector<std::int16_t> pcm(bytes / sizeof(std::int16_t));
    std::memcpy(pcm.data(), work.data(), pcm.size() * sizeof(std::int16_t));

    SDL_LockAudioDevice(device_);
    samples_[static_cast<std::size_t>(sampleIndex)] = std::move(pcm);
    SDL_UnlockAudioDevice(device_);
    return true;
}

void AudioMixer::playSample(int voice, int sampleIndex, float gain) {
    if (!device_ || voice < 0 || voice >= kMaxVoices || sampleIndex < 0 || sampleIndex >= kMaxSamples) return;
    SDL_LockAudioDevice(device_);
    if (!samples_[static_cast<std::size_t>(sampleIndex)].empty()) {
        voices_[static_cast<std::size_t>(voice)] =
            Voice{sampleIndex, 0, static_cast<std::int32_t>(std::clamp(gain, 0.0f, 1.0f) * 32768.0f), true};
    }
    SDL_UnlockAudioDevice(device_);
}

void AudioMixer::startMusic(std::unique_ptr<MusicStream> stream) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    music_.swap(stream);
    musicPaused_ = false;
    SDL_UnlockAudioDevice(device_);
    // The previous song (now in `stream`) is released here, outside the audio lock.
}

void AudioMixer::stopMusic() { startMusic(nullptr); }

void AudioMixer::setMusicGain(float gain) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    if (music_) music_->setGain(std::clamp(gain, 0.0f, 1.0f));
    SDL_UnlockAudioDevice(device_);
}

void AudioMixer::pauseMusic(bool paused) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    musicPaused_ = paused;
    SDL_UnlockAudioDevice(device_);
}

void AudioMixer::setPaused(bool paused) {
    if (device_) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}

void SDLCALL AudioMixer::callback(void* userdata, Uint8* stream, int len) {
    auto* self = static_cast<AudioMixer*>(userdata);
    self->mix(reinterpret_cast<std::int16_t*>(stream), static_cast<std::size_t>(len) / sizeof(std::int16_t));
}

void AudioMixer::mix(std::int16_t* out, std::size_t samples) {
    if (samples > scratch_.size()) samples = scratch_.size();  // never allocate here
    std::fill_n(scratch_.begin(), samples, 0);
    if (music_ && !musicPaused_ && spec_.channels == 2) music_->mixInto(scratch_.data(), samples / 2, spec_.freq);
    for (Voice& v : voices_) {
        if (!v.active) continue;
        const auto& pcm = samples_[static_cast<std::size_t>(v.sample)];
        const std::size_t n = std::min(samples, pcm.size() - std::min(pcm.size(), v.position));
        for (std::size_t k = 0; k < n; ++k) scratch_[k] += (pcm[v.position + k] * v.gainQ15) >> 15;
        v.position += n;
        if (v.position >= pcm.size()) v.active = false;
    }
    for (std::size_t k = 0; k < samples; ++k) {
        out[k] = static_cast<std::int16_t>(std::clamp<std::int32_t>(scratch_[k], -32768, 32767));
    }
}

}  // namespace touhou
