#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL.h>

#include "audio/MusicPlayer.hpp"
#include "audio/Sound.hpp"

namespace touhou {

// Sound-effect and music mixer on an SDL audio device (AAudio/OpenSL ES on Android). Effect
// WAVs are converted once to the device format at load time; music streams from the
// memory-mapped WAV in the game folder. The audio callback only sums samples and never
// allocates. One voice per sound id; replaying a voice restarts it.
class AudioMixer final : public SampleBackend, public MusicBackend {
  public:
    static constexpr int kMaxSamples = 32;
    static constexpr int kMaxVoices = 32;

    ~AudioMixer() override;

    bool open(std::string* error);
    void close();
    bool loadSample(int sampleIndex, const std::uint8_t* wav, std::size_t size) override;
    void playSample(int voice, int sampleIndex, float gain) override;
    void setPaused(bool paused);
    void startMusic(std::unique_ptr<MusicStream> stream) override;
    void stopMusic() override;
    void setMusicGain(float gain) override;
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
    std::unique_ptr<MusicStream> music_;
};

}  // namespace touhou
