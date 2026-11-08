#pragma once

// Perfect Cherry Blossom's sound effects: a sound id (0..37) names a sample (one of 30 WAVs
// under data/wav/ in th07.dat) and a DirectSound volume, both from th07.exe (ExeTables).
// Requests are queued during a frame (five entries; a repeat of an id already queued is
// ignored, more are dropped), and the queue is played once per frame, each id on its own
// voice. While effects are switched off the queue is kept, full, as in the original.

#include <array>

#include "audio/Sound.hpp"
#include "th07/ExeTables.hpp"

namespace touhou {
class Vfs;
}  // namespace touhou

namespace touhou::th07 {

class SoundPlayer final : public SoundSink {
  public:
    static constexpr int kQueueLength = 5;

    SoundPlayer(SampleBackend* backend, const ExeTables* tables) : backend_(backend), tables_(tables) { queue_.fill(-1); }

    // Loads the 30 samples from the archive; returns how many were found.
    int loadSamples(const Vfs& vfs);

    void playSound(int soundIdx) override;  // queue
    void flush();                           // play the frame's queue
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    float gainForSound(int soundIdx) const;  // linear gain from the DirectSound volume

  private:
    SampleBackend* backend_;
    const ExeTables* tables_;
    std::array<int, kQueueLength> queue_{};
    bool enabled_ = true;
};

}  // namespace touhou::th07
