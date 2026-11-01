#include "audio/SoundPlayer.hpp"

#include <cmath>

#include "io/Vfs.hpp"

namespace th06 {
namespace {

struct SoundEffectData {
    int sample;
    int volumeMillibels;
    int priority;
};

constexpr SoundEffectData kSoundTable[SoundPlayer::kSoundIds] = {
    {0, -1500, 0},   {0, -2000, 0},   {1, -1200, 5},   {1, -1400, 5},  {2, -1000, 100}, {3, -500, 100},
    {4, -500, 100},  {5, -1700, 50},  {6, -1700, 50},  {7, -1700, 50}, {8, -1000, 100}, {9, -1000, 100},
    {10, -1900, 10}, {11, -1200, 10}, {12, -900, 100}, {5, -1500, 50}, {13, -900, 50},  {14, -900, 50},
    {15, -600, 100}, {16, -400, 100}, {17, -1100, 0},  {18, -900, 0},  {5, -1800, 20},  {6, -1800, 20},
    {7, -1800, 20},  {19, -300, 50},  {20, -600, 50},  {21, -800, 50}, {22, -100, 140}, {23, -500, 100},
    {24, -1000, 20}, {25, -1000, 90},
};

constexpr const char* kSampleFiles[SoundPlayer::kSamples] = {
    "plst00.wav", "enep00.wav",   "pldead00.wav", "power0.wav",  "power1.wav",  "tan00.wav",   "tan01.wav",
    "tan02.wav",  "ok00.wav",     "cancel00.wav", "select00.wav", "gun00.wav",  "cat00.wav",   "lazer00.wav",
    "lazer01.wav", "enep01.wav",  "nep00.wav",    "damage00.wav", "item00.wav", "kira00.wav",  "kira01.wav",
    "kira02.wav", "extend.wav",   "timeout.wav",  "graze.wav",   "powerup.wav",
};

}  // namespace

const char* SoundPlayer::sampleFile(int i) { return i >= 0 && i < kSamples ? kSampleFiles[i] : ""; }

int SoundPlayer::sampleForSound(int idx) { return idx >= 0 && idx < kSoundIds ? kSoundTable[idx].sample : -1; }

float SoundPlayer::gainForSound(int idx) {
    if (idx < 0 || idx >= kSoundIds) return 0.0f;
    // DirectSound volume is attenuation in hundredths of a decibel.
    return std::pow(10.0f, static_cast<float>(kSoundTable[idx].volumeMillibels) / 2000.0f);
}

int SoundPlayer::loadSamples(const Vfs& vfs) {
    int loaded = 0;
    for (int i = 0; i < kSamples && backend_; ++i) {
        auto bytes = vfs.read(kSampleFiles[i]);
        if (bytes && backend_->loadSample(i, bytes->data(), bytes->size())) ++loaded;
    }
    return loaded;
}

void SoundPlayer::playSound(int idx) {
    if (idx < 0 || idx >= kSoundIds) return;
    for (int& q : queue_) {
        if (q < 0) {
            q = idx;
            return;
        }
        if (q == idx) return;
    }
    // queue full: dropped, as in the original
}

void SoundPlayer::flush() {
    if (!enabled_) return;
    for (int& q : queue_) {
        if (q < 0) break;
        if (backend_) backend_->playSample(q, sampleForSound(q), gainForSound(q));
        q = -1;
    }
}

}  // namespace th06
