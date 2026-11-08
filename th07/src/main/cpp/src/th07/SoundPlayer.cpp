#include "th07/SoundPlayer.hpp"

#include <cmath>

#include "io/Vfs.hpp"

namespace touhou::th07 {

int SoundPlayer::loadSamples(const Vfs& vfs) {
    if (!backend_ || !tables_) return 0;
    int loaded = 0;
    for (int i = 0; i < ExeTables::kSamples; ++i) {
        const auto bytes = vfs.read(tables_->sampleFiles[static_cast<std::size_t>(i)]);
        if (bytes && backend_->loadSample(i, bytes->data(), bytes->size())) ++loaded;
    }
    return loaded;
}

float SoundPlayer::gainForSound(int idx) const {
    if (!tables_ || idx < 0 || idx >= ExeTables::kSoundIds) return 0.0f;
    return std::pow(10.0f, static_cast<float>(tables_->sounds[static_cast<std::size_t>(idx)].volume) / 2000.0f);
}

void SoundPlayer::playSound(int idx) {
    if (idx < 0 || idx >= ExeTables::kSoundIds) return;  // ids from data past the table are dropped
    for (int& q : queue_) {
        if (q < 0) {
            q = idx;
            return;
        }
        if (q == idx) return;
    }
}

void SoundPlayer::flush() {
    if (!enabled_) return;
    for (int& q : queue_) {
        if (q < 0) break;
        if (backend_ && tables_) backend_->playSample(q, tables_->sounds[static_cast<std::size_t>(q)].sample, gainForSound(q));
        q = -1;
    }
}

}  // namespace touhou::th07
