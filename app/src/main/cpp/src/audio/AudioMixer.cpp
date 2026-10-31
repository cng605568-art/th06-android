#include "audio/AudioMixer.hpp"

#include <algorithm>
#include <cstring>

#include "core/Log.hpp"

namespace th06 {

AudioMixer::~AudioMixer() { close(); }

bool AudioMixer::open(std::string* error) {
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;  // ~11.6 ms per callback
    want.callback = &AudioMixer::callback;
    want.userdata = this;
    // allowed_changes = 0: SDL converts internally if the hardware wants something else,
    // so the mixer can always assume S16 stereo at 44.1 kHz.
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

bool AudioMixer::load(Sfx sfx, std::span<const std::uint8_t> wavBytes, std::string* error) {
    if (!device_) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(wavBytes.data(), static_cast<int>(wavBytes.size()));
    SDL_AudioSpec src{};
    Uint8* buffer = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV_RW(rw, 1, &src, &buffer, &length)) {
        if (error) *error = std::string("SDL_LoadWAV: ") + SDL_GetError();
        return false;
    }

    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, src.format, src.channels, src.freq, AUDIO_S16SYS, 2, spec_.freq) < 0) {
        SDL_FreeWAV(buffer);
        if (error) *error = std::string("SDL_BuildAudioCVT: ") + SDL_GetError();
        return false;
    }
    std::vector<Uint8> work(static_cast<std::size_t>(length) * std::max(1, cvt.len_mult));
    std::memcpy(work.data(), buffer, length);
    SDL_FreeWAV(buffer);
    cvt.buf = work.data();
    cvt.len = static_cast<int>(length);
    if (cvt.needed && SDL_ConvertAudio(&cvt) < 0) {
        if (error) *error = std::string("SDL_ConvertAudio: ") + SDL_GetError();
        return false;
    }
    const std::size_t bytes = cvt.needed ? static_cast<std::size_t>(cvt.len_cvt) : length;

    std::vector<std::int16_t> pcm(bytes / sizeof(std::int16_t));
    std::memcpy(pcm.data(), work.data(), pcm.size() * sizeof(std::int16_t));

    const auto index = static_cast<std::size_t>(sfx);
    SDL_LockAudioDevice(device_);
    sounds_[index] = std::move(pcm);
    voices_[index] = Voice{};
    SDL_UnlockAudioDevice(device_);
    return true;
}

void AudioMixer::play(Sfx sfx) {
    const auto index = static_cast<std::size_t>(sfx);
    if (!device_ || index >= kCount) return;
    SDL_LockAudioDevice(device_);
    if (!sounds_[index].empty()) voices_[index] = Voice{0, true};
    SDL_UnlockAudioDevice(device_);
}

void AudioMixer::setPaused(bool paused) {
    if (device_) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}

void SDLCALL AudioMixer::callback(void* userdata, Uint8* stream, int len) {
    auto* self = static_cast<AudioMixer*>(userdata);
    self->mix(reinterpret_cast<std::int16_t*>(stream),
              static_cast<std::size_t>(len) / sizeof(std::int16_t));
}

void AudioMixer::mix(std::int16_t* out, std::size_t samples) {
    if (samples > scratch_.size()) samples = scratch_.size();  // never allocate here
    std::fill_n(scratch_.begin(), samples, 0);
    for (std::size_t i = 0; i < kCount; ++i) {
        Voice& v = voices_[i];
        if (!v.active) continue;
        const auto& pcm = sounds_[i];
        const std::size_t n = std::min(samples, pcm.size() - v.position);
        for (std::size_t k = 0; k < n; ++k) scratch_[k] += pcm[v.position + k];
        v.position += n;
        if (v.position >= pcm.size()) v.active = false;
    }
    for (std::size_t k = 0; k < samples; ++k) {
        // Effects at full scale; a light 3/4 gain leaves headroom when several overlap.
        const std::int32_t s = scratch_[k] * 3 / 4;
        out[k] = static_cast<std::int16_t>(std::clamp<std::int32_t>(s, -32768, 32767));
    }
}

}  // namespace th06
