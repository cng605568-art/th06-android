#pragma once

// Sound effects as the 2002 engine plays them: a SoundIdx (0..31) names one of 26 wav
// files in the archives plus a DirectSound volume; requests are queued during a frame,
// a SoundIdx already queued that frame is ignored, and the queue is played once per frame.
// (Tables from the th06 decompilation, SoundPlayer.cpp.)

#include <array>
#include <cstdint>

#include "game/World.hpp"

namespace th06 {

class Vfs;

class SampleBackend {
  public:
    virtual ~SampleBackend() = default;
    virtual bool loadSample(int sampleIndex, const std::uint8_t* wav, std::size_t size) = 0;
    // Restarts `voice` (one per SoundIdx, like the original's duplicate DirectSound buffers).
    virtual void playSample(int voice, int sampleIndex, float gain) = 0;
};

class SoundPlayer final : public SoundSink {
  public:
    static constexpr int kSamples = 26;
    static constexpr int kSoundIds = 32;
    static constexpr int kQueueLength = 3;  // SFX_QUEUE_LENGTH

    explicit SoundPlayer(SampleBackend* backend) : backend_(backend) { queue_.fill(-1); }

    // Loads data/wav/*.wav from the archives; returns how many were found.
    int loadSamples(const Vfs& vfs);

    void playSound(int soundIdx) override;  // queue (PlaySoundByIdx)
    void flush();                           // play the frame's queue (PlaySounds)

    static const char* sampleFile(int sampleIndex);
    static int sampleForSound(int soundIdx);
    static float gainForSound(int soundIdx);  // linear gain from the DirectSound millibels

  private:
    SampleBackend* backend_;
    std::array<int, kQueueLength> queue_{};
};

}  // namespace th06
