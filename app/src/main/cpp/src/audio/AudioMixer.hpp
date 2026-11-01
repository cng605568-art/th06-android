#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL.h>

#include "audio/SoundPlayer.hpp"

namespace th06 {

// Sound-effect mixer on an SDL audio device (AAudio/OpenSL ES on Android). WAV files are
// converted once to the device format at load time; the audio callback only sums int16
// samples and never allocates. One voice per SoundIdx; replaying a voice restarts it.
class AudioMixer final : public SampleBackend {
  public:
    static constexpr int kMaxSamples = 32;
    static constexpr int kMaxVoices = 32;

    ~AudioMixer() override;

    bool open(std::string* error);
    void close();
    bool loadSample(int sampleIndex, const std::uint8_t* wav, std::size_t size) override;
    void playSample(int voice, int sampleIndex, float gain) override;
    void setPaused(bool paused);
    bool isOpen() const { return device_ != 0; }

  private:
    static void SDLCALL callback(void* userdata, Uint8* stream, int len);
    void mix(std::int16_t* out, std::size_t samples);

    struct Voice {
        int sample = -1;
        std::size_t position = 0;
        std::int32_t gainQ15 = 0;  // gain in 1/32768 units
        bool active = false;
    };

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec spec_{};
    std::array<std::vector<std::int16_t>, kMaxSamples> samples_;  // interleaved stereo
    std::array<Voice, kMaxVoices> voices_{};
    std::vector<std::int32_t> scratch_;
};

}  // namespace th06
