#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <SDL.h>

#include "game/Sound.hpp"

namespace th06 {

// Minimal sound-effect mixer on an SDL audio device (AAudio/OpenSL ES on Android).
// WAV files from the archives are converted once to the device format at load time;
// the audio callback only sums int16 samples and never allocates.
// Each effect has one voice and replaying restarts it, as in the original game, which
// keeps rapid-fire effects (graze, shots) from stacking into distortion.
class AudioMixer final : public SoundSink {
  public:
    ~AudioMixer() override;

    bool open(std::string* error);
    void close();
    bool load(Sfx sfx, std::span<const std::uint8_t> wavBytes, std::string* error);
    void play(Sfx sfx) override;
    void setPaused(bool paused);
    bool isOpen() const { return device_ != 0; }

  private:
    static void SDLCALL callback(void* userdata, Uint8* stream, int len);
    void mix(std::int16_t* out, std::size_t samples);

    static constexpr std::size_t kCount = static_cast<std::size_t>(Sfx::Count);
    struct Voice {
        std::size_t position = 0;
        bool active = false;
    };

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec spec_{};
    std::array<std::vector<std::int16_t>, kCount> sounds_;  // interleaved stereo
    std::array<Voice, kCount> voices_{};
    std::vector<std::int32_t> scratch_;
};

}  // namespace th06
