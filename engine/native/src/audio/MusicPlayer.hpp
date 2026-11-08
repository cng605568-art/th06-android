#pragma once

// Background music, as the 2002 game plays it in WAV mode: a song path from the stage file
// ("bgm/th06_02.mid") becomes the loose file bgm/th06_02.wav next to the game (the player's
// own copy) and the loop file th06_02.pos from the archives. Songs always loop: the first
// pass plays [0, loop end), every later pass [loop start, loop end), gaplessly. Every play
// request restarts the song. Fades are linear in decibels down to -50 dB, then the song
// stops. Music keeps playing through the pause menu.
//
// MusicStream is the sample source the audio thread pulls from; MusicPlayer is the game
// side (requests, fades) and owns no audio device.
//
// Replaced music (the launcher's "Replace music"): a th06_NN.wav in the override folder
// (the app's storage) is played instead of the game folder's, with the loop points of a
// th06_NN.pos next to it (none: the whole file loops).

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace touhou {

class MappedFile;
class Vfs;
class UnportedFeatures;

class MusicStream {
  public:
    // `wav` must stay valid while the stream plays (memory-mapped from the game folder).
    // Loop points are in sample frames from the start of the data chunk; 0 = not set.
    bool open(std::span<const std::uint8_t> wav, std::int64_t loopStart, std::int64_t loopEnd, std::string* error);
    // Adds `frames` stereo frames at `outRate` into `acc` (interleaved L R), scaled by gain.
    void mixInto(std::int32_t* acc, std::size_t frames, int outRate);
    void setGain(float gain) { gainQ15_ = static_cast<std::int32_t>(gain * 32768.0f); }

    std::size_t totalFrames() const { return totalFrames_; }
    std::size_t loopStart() const { return loopStart_; }
    std::size_t loopEnd() const { return loopEnd_; }
    std::uint64_t position() const { return pos_ >> 32; }  // current frame
    int sampleRate() const { return rate_; }
    int channels() const { return channels_; }

  private:
    const std::uint8_t* data_ = nullptr;
    std::size_t totalFrames_ = 0, loopStart_ = 0, loopEnd_ = 0;
    int channels_ = 2, rate_ = 44100;
    std::uint64_t pos_ = 0;  // 32.32 fixed point, in source frames
    std::int32_t gainQ15_ = 32768;
};

// Where the stream goes (the audio mixer). Calls come from the game thread.
class MusicBackend {
  public:
    virtual ~MusicBackend() = default;
    virtual void startMusic(std::unique_ptr<MusicStream> stream) = 0;  // replaces any playing song
    virtual void stopMusic() = 0;
    virtual void setMusicGain(float gain) = 0;
};

class MusicPlayer {
  public:
    MusicPlayer(const Vfs* vfs, MusicBackend* backend, UnportedFeatures* unported = nullptr);
    ~MusicPlayer();  // stops the song (it may be reading a replacement file)

    // Where replacement songs are looked for first (ends with '/'); empty: none.
    void setOverrideDir(std::string dir) { overrideDir_ = std::move(dir); }
    // The song currently playing is a replacement.
    bool playingOverride() const { return playing_ && override_ != nullptr; }

    // Starts the song (from its beginning, even if it is already playing). False if its
    // WAV is not in the game folder: the music stops.
    bool play(std::string_view songPath);
    void stop();
    void fadeOut(float seconds);
    // Once per logic frame (also while paused): advances a fade.
    void tick();

    const std::string& current() const { return current_; }
    bool playing() const { return playing_; }
    bool fading() const { return fadeFrames_ > 0; }
    float gain() const { return gain_; }
    void setUnported(UnportedFeatures* u) { unported_ = u; }
    // What a missing song is reported as (UnportedFeatures, shown on th06's HUD).
    void setMissingMessage(std::string message) { missingMessage_ = std::move(message); }
    // The Option menu's music switch: while off, play requests are ignored (the music
    // stops). MIDI mode plays the WAV files: the port has no MIDI synthesiser.
    void setEnabled(bool on) {
        enabled_ = on;
        if (!on) stop();
    }
    bool enabled() const { return enabled_; }

    // "bgm/th06_02.mid" -> "bgm/th06_02.wav" (the three characters after the last dot).
    static std::string withExtension(std::string_view path, std::string_view ext);

  private:
    const Vfs* vfs_;
    MusicBackend* backend_;
    UnportedFeatures* unported_;
    std::string current_;
    bool playing_ = false;
    std::int32_t fadeFrames_ = 0, fadeLeft_ = 0;
    float gain_ = 1.0f;
    bool enabled_ = true;
    std::string overrideDir_;
    std::string missingMessage_ = "background music (its WAV file is not in the game folder)";
    std::unique_ptr<MappedFile> override_;  // the replacement file being played
};

}  // namespace touhou
