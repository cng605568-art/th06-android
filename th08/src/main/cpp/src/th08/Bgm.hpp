#pragma once

// Imperishable Night's background music (as TH07's: claude/th08-specs/spec_files.md §3.7), in WAV mode: every song
// is a run of raw PCM inside the player's thbgm.dat ("ZWAV" header), found by name in the
// archive's bgm/thbgm.fmt. Requests go through a small command queue that the game runs one
// step per frame (a song becomes audible on the 5th step after its request reaches the
// head); fades are whole seconds, linear in decibels down to -50 dB, and keep counting
// while the music is paused (th08 pauses music in its pause and continue menus).
// MIDI mode plays the same songs from thbgm.dat: the port has no MIDI synthesiser.

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace touhou {
class MappedFile;
class MusicBackend;
class MusicStream;
}  // namespace touhou

namespace touhou::th08 {

// One 52-byte thbgm.fmt record. Offsets and lengths are in bytes; the start is absolute in
// thbgm.dat, the intro (loop point) and total (loop end) are relative to it.
struct BgmRecord {
    std::string name;  // e.g. "th08_01.wav"
    std::int32_t start = 0;
    std::uint32_t preloadSize = 0;
    std::int32_t intro = 0;
    std::int32_t total = 0;
    std::uint16_t formatTag = 0, channels = 0;
    std::uint32_t sampleRate = 0, bytesPerSecond = 0;
    std::uint16_t blockAlign = 0, bitsPerSample = 0;
};

// The records up to the terminator (a record whose name starts with NUL) or the end (TH08's
// thbgm.fmt ends with a truncated terminator: 17 zero bytes).
std::vector<BgmRecord> parseThbgmFmt(std::span<const std::uint8_t> fmt);
// thbgm.dat's 16-byte header: "ZWAV", 1, 0x800 (TH07: 0x700; the 4th word is not checked).
bool validThbgmHeader(std::span<const std::uint8_t> thbgm);

class BgmPlayer {
  public:
    enum Mode : int { Off = 0, Wav = 1, Midi = 2 };
    static constexpr int kSlots = 16;
    static constexpr std::size_t kQueueCapacity = 31;

    explicit BgmPlayer(MusicBackend* backend);
    ~BgmPlayer();

    // The song table and the mapped thbgm.dat (empty: no streamer; requests are dropped and
    // the game runs silent, as on PC when thbgm.dat is missing).
    void setData(std::vector<BgmRecord> records, std::span<const std::uint8_t> thbgm);
    bool hasStreamer() const { return !thbgm_.empty() && !records_.empty(); }
    void setMode(int mode) { mode_ = mode; }
    // The Option menu's BGM volume (0-100), on top of the fades.
    void setVolumePercent(int percent);
    // The launcher's replaced music: <dir>/th08_NN.wav (+ th08_NN.pos loop points, as the th06
    // app writes them) plays in place of that thbgm.dat record.
    void setOverrideDir(std::string dir) { overrideDir_ = std::move(dir); }
    int mode() const { return mode_; }

    // The game's requests. Paths are song paths as the data names them ("bgm/th08_02.mid").
    void load(int slot, std::string_view path);
    void playLoaded(int slot);
    void play(std::string_view path);
    void stop();
    // `speed`: the game-speed multiplier (1, or 1/k while an enemy script slows the game).
    void fadeOut(float seconds, float speed = 1.0f);
    void pause();
    void resume();
    void shutdown();

    // At the very start of every frame (also in menus and while paused): one fade step.
    void fadeStep();
    // Once per frame after the game's update: one step of the head command. True while
    // commands remain.
    bool pump();
    void drain();  // pump until the queue is empty

    // State, for the HUD and tests.
    const std::string& current() const { return current_; }  // the playing record's name
    bool playing() const { return playing_; }
    bool paused() const { return paused_; }
    int volume() const { return volume_; }  // hundredths of a dB (0 = full)
    std::size_t queued() const { return queue_.size(); }
    // The record a song path resolves to (record 0 when its name is not in the table).
    const BgmRecord* resolve(std::string_view path) const;

  private:
    enum class Op { Start, Stop, Shutdown, Fade, Pause, Resume };
    struct Command {
        explicit Command(Op o, int a = 0, std::string p = {}) : op(o), arg(a), path(std::move(p)) {}
        Op op;
        int arg = 0;       // slot (-1: the path below), or seconds
        std::string path;  // START with slot -1
        int step = 0;
    };
    void push(Command c);
    void startSong(const std::string& path);
    void setVolume(int centibels);
    bool playsWav() const { return mode_ == Wav || mode_ == Midi; }

    MusicBackend* backend_;
    std::vector<BgmRecord> records_;
    std::span<const std::uint8_t> thbgm_;
    int mode_ = Wav;
    std::string slots_[kSlots];
    std::deque<Command> queue_;
    std::string current_;
    bool playing_ = false, paused_ = false;
    int volume_ = 0;
    int volumePercent_ = 100;
    bool fading_ = false;
    int fadeLeft_ = 0, fadeTotal_ = 0;
    std::unique_ptr<MusicStream> prepared_;  // START: made at step 3, played at step 4
    // A replacement file stays mapped while its stream is prepared and while it plays.
    std::string overrideDir_;
    std::unique_ptr<MappedFile> preparedOverride_, playingOverride_;
};

}  // namespace touhou::th08
