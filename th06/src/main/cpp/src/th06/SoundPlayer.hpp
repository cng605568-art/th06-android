#pragma once

// Sound effects as the 2002 engine plays them: a SoundIdx (0..31) names one of 26 wav
// files in the archives plus a DirectSound volume; requests are queued during a frame,
// a SoundIdx already queued that frame is ignored, and the queue is played once per frame.
// (Tables from the th06 decompilation, SoundPlayer.cpp.)

#include <array>
#include <cstdint>

#include "audio/Sound.hpp"

namespace touhou {
class Vfs;
}  // namespace touhou

namespace touhou::th06 {

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
    // The Option menu's sound-effect switch. While it is off the queue is kept (and stays
    // full at three), so what was queued plays on the first frame after it is back on.
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    static const char* sampleFile(int sampleIndex);
    static int sampleForSound(int soundIdx);
    static float gainForSound(int soundIdx);  // linear gain from the DirectSound millibels

  private:
    SampleBackend* backend_;
    std::array<int, kQueueLength> queue_{};
    bool enabled_ = true;
};

}  // namespace touhou::th06
