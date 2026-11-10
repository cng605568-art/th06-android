#pragma once

// Imperishable Night's sound effects: a sound id (0..45) names a sample (one of 36 WAVs in
// th08.dat) and a DirectSound volume, both from th08.exe (ExeTables), scaled by the Option
// menu's SE volume (0-100).
// Requests are queued during a frame (five entries; a repeat of an id already queued is
// ignored, more are dropped), and the queue is played once per frame, each id on its own
// voice. While effects are switched off the queue is kept, full, as in the original.

#include <array>

#include "audio/Sound.hpp"
#include "th08/ExeTables.hpp"

namespace touhou {
class Vfs;
}  // namespace touhou

namespace touhou::th08 {

class SoundPlayer final : public SoundSink {
  public:
    static constexpr int kQueueLength = 5;

    SoundPlayer(SampleBackend* backend, const ExeTables* tables) : backend_(backend), tables_(tables) { queue_.fill(-1); }

    // Loads the 36 samples from the archive; returns how many were found.
    int loadSamples(const Vfs& vfs);

    void playSound(int soundIdx) override;  // queue
    void flush();                           // play the frame's queue
    void setEnabled(bool on) { enabled_ = on; }
    void setVolumePercent(int percent) { volumePercent_ = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
    bool enabled() const { return enabled_; }
    float gainForSound(int soundIdx) const;  // linear gain from the DirectSound volume

  private:
    SampleBackend* backend_;
    const ExeTables* tables_;
    std::array<int, kQueueLength> queue_{};
    bool enabled_ = true;
    int volumePercent_ = 80;
};

}  // namespace touhou::th08
