#pragma once

// Sound effects between a game and the audio device. A game names its effects by its own
// index (th06's SoundIdx 0..31) and keeps the table of files and volumes; the device side
// only stores decoded samples and plays them on voices.

#include <cstddef>
#include <cstdint>

namespace touhou {

// Where a game's simulation sends sound requests (the game's sound player, or a test).
class SoundSink {
  public:
    virtual ~SoundSink() = default;
    virtual void playSound(int soundIdx) = 0;
};

// The audio device's side: decoded samples played on voices.
class SampleBackend {
  public:
    virtual ~SampleBackend() = default;
    virtual bool loadSample(int sampleIndex, const std::uint8_t* wav, std::size_t size) = 0;
    // Restarts `voice` (one per sound id, like the 2002 engines' duplicate DirectSound buffers).
    virtual void playSample(int voice, int sampleIndex, float gain) = 0;
};

}  // namespace touhou
