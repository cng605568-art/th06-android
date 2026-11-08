// Host tests for Perfect Cherry Blossom's game code (th07_game) and the engine pieces it
// added: the PBG4 archive, thbgm.dat music, the version file. Synthetic data only; with
// TH07_TEST_DUMP=<dir> the synthetic th07.dat and thbgm.dat are also written there, to run
// the desktop build on.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "audio/MusicPlayer.hpp"
#include "io/Archive.hpp"
#include "io/MappedFile.hpp"
#include "io/Vfs.hpp"
#include "th07/Bgm.hpp"
#include "th07/ExeTables.hpp"
#include "th07/SoundPlayer.hpp"
#include "th07/Version.hpp"

using namespace touhou;
using namespace touhou::th07;
using Bytes = std::vector<std::uint8_t>;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

namespace {

// LZSS with literals only (flag 1 + 8 bits per byte), ended by a reference to position 0.
Bytes lzssLiterals(const Bytes& in) {
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    auto put = [&](std::uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++bits == 8) {
                out.push_back(static_cast<std::uint8_t>(acc));
                acc = 0;
                bits = 0;
            }
        }
    };
    for (std::uint8_t b : in) {
        put(1, 1);
        put(b, 8);
    }
    put(0, 1);
    put(0, 13);
    if (bits) put(0, 8 - bits);
    return out;
}

void put32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void set32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

struct Member {
    std::string name;
    Bytes data;
};

// A PBG4 archive: header, LZSS members, LZSS table at the end.
Bytes pbg4(const std::vector<Member>& members) {
    Bytes out(16, 0);
    std::memcpy(out.data(), "PBG4", 4);
    Bytes table;
    for (const Member& m : members) {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        const Bytes packed = lzssLiterals(m.data);
        out.insert(out.end(), packed.begin(), packed.end());
        table.insert(table.end(), m.name.begin(), m.name.end());
        table.push_back(0);
        put32(table, offset);
        put32(table, static_cast<std::uint32_t>(m.data.size()));
        put32(table, 0);
    }
    const std::uint32_t tableOffset = static_cast<std::uint32_t>(out.size());
    const Bytes packed = lzssLiterals(table);
    out.insert(out.end(), packed.begin(), packed.end());
    set32(out, 4, static_cast<std::uint32_t>(members.size()));
    set32(out, 8, tableOffset);
    set32(out, 12, static_cast<std::uint32_t>(table.size()));
    return out;
}

std::unique_ptr<MappedFile> mem(const Bytes& b) { return MappedFile::fromMemory(b); }

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

// thbgm.fmt record (52 bytes).
void fmtRecord(Bytes& fmt, const std::string& name, std::int32_t start, std::int32_t intro, std::int32_t total) {
    Bytes r(0x34, 0);
    std::memcpy(r.data(), name.data(), std::min<std::size_t>(name.size(), 16));
    set32(r, 0x10, static_cast<std::uint32_t>(start));
    set32(r, 0x14, static_cast<std::uint32_t>(total));
    set32(r, 0x18, static_cast<std::uint32_t>(intro));
    set32(r, 0x1C, static_cast<std::uint32_t>(total));
    r[0x20] = 1;   // PCM
    r[0x22] = 2;   // stereo
    set32(r, 0x24, 44100);
    set32(r, 0x28, 44100 * 4);
    r[0x2C] = 4;   // block align
    r[0x2E] = 16;  // bits
    fmt.insert(fmt.end(), r.begin(), r.end());
}

// thbgm.dat with two songs: a quiet 220 Hz tone (2 s) and a 330 Hz tone (1 s).
struct Music {
    Bytes thbgm, fmt;
};
Music syntheticMusic() {
    Music m;
    m.thbgm = text("ZWAV");
    put32(m.thbgm, 1);
    put32(m.thbgm, 0x700);
    put32(m.thbgm, 0);
    auto tone = [&](double hz, int frames) {
        const std::size_t start = m.thbgm.size();
        for (int i = 0; i < frames; ++i) {
            const auto v = static_cast<std::int16_t>(std::lround(std::sin(i * hz * 2 * M_PI / 44100.0) * 3000.0));
            put16(m.thbgm, static_cast<std::uint16_t>(v));
            put16(m.thbgm, static_cast<std::uint16_t>(v));
        }
        return start;
    };
    const std::size_t a = tone(220, 88200), b = tone(330, 44100);
    fmtRecord(m.fmt, "th07_01.wav", static_cast<std::int32_t>(a), 44100 * 4, 88200 * 4);
    fmtRecord(m.fmt, "th07_02.wav", static_cast<std::int32_t>(b), 0, 44100 * 4);
    m.fmt.resize(m.fmt.size() + 0x34, 0);  // terminator
    return m;
}

struct RecordingMusic final : MusicBackend {
    std::unique_ptr<MusicStream> stream;
    float gain = 1.0f;
    bool paused = false;
    int starts = 0, stops = 0;
    void startMusic(std::unique_ptr<MusicStream> s) override {
        stream = std::move(s);
        paused = false;
        ++starts;
    }
    void stopMusic() override {
        stream.reset();
        ++stops;
    }
    void setMusicGain(float g) override { gain = g; }
    void pauseMusic(bool p) override { paused = p; }
};

// A minimal PE32 image with one section mapping VA 0x49E000..0x4A0000, holding test tables
// at th07.exe 1.00b's addresses (sound id i: sample i % 30, volume -10 * i).
Bytes syntheticExe() {
    constexpr std::uint32_t kRva = 0x9E000, kRaw = 0x400, kSize = 0x2000;
    Bytes exe(kRaw + kSize, 0);
    exe[0] = 'M';
    exe[1] = 'Z';
    set32(exe, 0x3c, 0x40);
    std::memcpy(exe.data() + 0x40, "PE\0\0", 4);
    exe[0x46] = 1;     // one section
    exe[0x54] = 0xE0;  // optional header size
    exe[0x58] = 0x0b;  // PE32 magic 0x10b
    exe[0x59] = 0x01;
    set32(exe, 0x58 + 28, 0x400000);  // image base
    const std::size_t sec = 0x58 + 0xE0;
    std::memcpy(exe.data() + sec, ".data", 5);
    set32(exe, sec + 8, kSize);
    set32(exe, sec + 12, kRva);
    set32(exe, sec + 16, kSize);
    set32(exe, sec + 20, kRaw);
    auto off = [&](std::uint32_t va) { return kRaw + (va - 0x400000 - kRva); };
    for (int i = 0; i < 38; ++i) {
        set32(exe, off(0x0049EA88) + 8 * i, static_cast<std::uint32_t>(i % 30));
        exe[off(0x0049EA88) + 8 * i + 4] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(-10 * i));
        exe[off(0x0049EA88) + 8 * i + 5] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(-10 * i) >> 8);
    }
    std::uint32_t strVa = 0x0049F800;
    for (int i = 0; i < 30; ++i) {
        const std::string name = "data/wav/se_test" + std::to_string(i) + ".wav";
        std::memcpy(exe.data() + off(strVa), name.data(), name.size());
        set32(exe, off(0x0049EBB8) + 4 * i, strVa);
        strVa += static_cast<std::uint32_t>(name.size()) + 1;
    }
    for (int i = 0; i < 31; ++i) set32(exe, off(0x0049ECF8) + 4 * i, static_cast<std::uint32_t>(10 * (i + 1)));
    for (int i = 0; i < 28; ++i) set32(exe, off(0x0049EEC8) + 4 * i, 0xFF000000u + static_cast<std::uint32_t>(i));
    for (int i = 0; i < 32; ++i) exe[off(0x0049EFA0) + i] = static_cast<std::uint8_t>(i % 3);
    for (int i = 0; i < 141; ++i) set32(exe, off(0x0049F1B8) + 4 * i, static_cast<std::uint32_t>(1000 * i));
    return exe;
}

// A tiny valid WAV (16-bit mono silence).
Bytes tinyWav() {
    Bytes w = text("RIFF");
    put32(w, 36 + 8);
    const Bytes rest = text("WAVEfmt ");
    w.insert(w.end(), rest.begin(), rest.end());
    put32(w, 16);
    put16(w, 1);
    put16(w, 1);
    put32(w, 22050);
    put32(w, 44100);
    put16(w, 2);
    put16(w, 16);
    const Bytes data = text("data");
    w.insert(w.end(), data.begin(), data.end());
    put32(w, 8);
    w.resize(w.size() + 8, 0);
    return w;
}

struct RecordingSamples final : SampleBackend {
    int loads = 0;
    struct Play {
        int voice, sample;
        float gain;
    };
    std::vector<Play> plays;
    bool loadSample(int, const std::uint8_t*, std::size_t) override {
        ++loads;
        return true;
    }
    void playSample(int voice, int sample, float gain) override { plays.push_back({voice, sample, gain}); }
};

}  // namespace

static void testExeTablesAndSound() {
    const Bytes exe = syntheticExe();
    ExeTables t;
    std::string err;
    CHECK(!loadExeTables(exe, t, &err));  // not the real 1.00b executable
    CHECK(loadExeTables(exe, t, &err, false));
    if (!err.empty() && t.sampleFiles[0].empty()) std::fprintf(stderr, "  %s\n", err.c_str());
    CHECK(t.sounds[37].sample == 7 && t.sounds[37].volume == -370);
    CHECK(t.sampleFiles[29] == "data/wav/se_test29.wav");
    CHECK(t.fullPowerChain[30] == 310 && t.colorsPale[27] == 0xFF00001Bu && t.dropCycle[31] == 1);
    CHECK(t.spellBonus[140] == 140000);
    CHECK(!loadExeTables(Bytes(exe.begin(), exe.begin() + 0x500), t, &err, false));  // tables cut off

    // Sound effects: samples by name from the archive; a 5-entry queue, repeats ignored,
    // more dropped; each id on its own voice with its own volume.
    std::vector<Member> members;
    for (int i = 0; i < 30; ++i) members.push_back({"se_test" + std::to_string(i) + ".wav", tinyWav()});
    Vfs vfs;
    vfs.mount(openArchive("th07.dat", mem(pbg4(members)), &err));
    RecordingSamples backend;
    SoundPlayer sound(&backend, &t);
    CHECK(sound.loadSamples(vfs) == 30 && backend.loads == 30);
    for (int id : {3, 3, 37, 1, 2, 4, 5, 6, 99}) sound.playSound(id);
    sound.flush();
    CHECK(backend.plays.size() == 5 && backend.plays[0].voice == 3 && backend.plays[1].voice == 37 &&
          backend.plays[1].sample == 7 && backend.plays[4].voice == 4);
    CHECK(std::fabs(backend.plays[1].gain - std::pow(10.0f, -370.0f / 2000.0f)) < 1e-6f);
    // Switched off: the queue is kept (full) and plays once effects are back on.
    backend.plays.clear();
    sound.setEnabled(false);
    for (int id = 10; id < 20; ++id) sound.playSound(id);
    sound.flush();
    CHECK(backend.plays.empty());
    sound.setEnabled(true);
    sound.flush();
    CHECK(backend.plays.size() == 5 && backend.plays[4].voice == 14);
}

static void testPbg4() {
    const Bytes ver = text("0100a 123 456\n0100b 466944 -1234567\n");
    const Bytes stage = text("STD file bytes");
    Bytes big(5000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7);
    const Bytes arc = pbg4({{"th07_0100b.ver", ver},
                            {"stage1.std", stage},
                            {"empty.bin", {}},
                            {"Stage1.STD", text("second copy")},
                            {"big.bin", big}});
    std::string err;
    auto archive = openArchive("th07.dat", mem(arc), &err);
    CHECK(archive && archive->format() == "PBG4");
    if (!archive) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return;
    }
    CHECK(archive->entries().size() == 3);  // the empty member and the second "stage1.std" are left out
    Vfs vfs;
    vfs.mount(std::move(archive));
    CHECK(vfs.read("data/STAGE1.std") == stage);  // case-insensitive, by base name; the first copy wins
    CHECK(vfs.read("big.bin") == big);
    CHECK(!vfs.exists("empty.bin"));
    const auto v = vfs.read(kVersionFile);
    CHECK(v && *v == ver);

    // Corrupt archives are refused.
    Bytes bad = arc;
    set32(bad, 4, 0);
    CHECK(!openArchive("th07.dat", mem(bad), &err));
    bad = arc;
    set32(bad, 8, static_cast<std::uint32_t>(arc.size()));
    CHECK(!openArchive("th07.dat", mem(bad), &err));
    bad = arc;
    set32(bad, 12, static_cast<std::uint32_t>(arc.size() * 10));  // table shorter than declared
    CHECK(!openArchive("th07.dat", mem(bad), &err));
    bad = Bytes(arc.begin(), arc.begin() + 100);
    CHECK(!openArchive("th07.dat", mem(bad), &err));
}

static void testVersionFile() {
    const Bytes ver = text("0100a 1 2\r\n0100b\t466944   -1234567\n");
    const auto b = findVersionLine(ver, kVersionTag);
    CHECK(b && b->size == 466944 && b->checksum == -1234567);
    const auto a = findVersionLine(ver, "0100a");
    CHECK(a && a->size == 1 && a->checksum == 2);
    CHECK(!findVersionLine(ver, "0099z"));
    CHECK(!findVersionLine(text("0100b x y"), kVersionTag));
    CHECK(!findVersionLine(text("0100b"), kVersionTag));
    // Checksum: the first floor(size/4) - 1 words; the last whole word and the tail are left out.
    Bytes exe;
    put32(exe, 0x80000000u);
    put32(exe, 0x80000001u);
    put32(exe, 5);
    put32(exe, 0xFFFFFFFFu);  // last whole word: not counted
    exe.push_back(9);         // tail: not counted
    CHECK(exeChecksum(exe) == 6);
    CHECK(exeChecksum(Bytes(3, 1)) == 0);
}

static void testBgm() {
    const Music m = syntheticMusic();
    CHECK(validThbgmHeader(m.thbgm));
    Bytes wrong = m.thbgm;
    wrong[9] = 0;  // 0x700 -> 0
    CHECK(!validThbgmHeader(wrong));
    const auto records = parseThbgmFmt(m.fmt);
    CHECK(records.size() == 2 && records[0].name == "th07_01.wav" && records[1].intro == 0);
    CHECK(records[0].channels == 2 && records[0].sampleRate == 44100 && records[0].blockAlign == 4);
    Bytes full = m.fmt;
    std::memcpy(full.data(), "abcdefghijklmnop", 16);  // a 16-byte name has no terminator
    CHECK(parseThbgmFmt(full)[0].name == "abcdefghijklmnop");

    RecordingMusic backend;
    BgmPlayer bgm(&backend);
    bgm.setData(records, m.thbgm);
    CHECK(bgm.resolve("bgm/th07_02.wav")->name == "th07_02.wav");
    CHECK(bgm.resolve("bgm/missing.wav")->name == "th07_01.wav");  // unknown names play the first song
    CHECK(bgm.resolve("BGM/TH07_02.WAV")->name == "th07_01.wav");  // names match case-sensitively

    // A song becomes audible on the 5th pump after its request; the command leaves on the 8th.
    bgm.load(0, "bgm/th07_02.mid");
    bgm.playLoaded(0);
    for (int i = 1; i <= 4; ++i) {
        bgm.pump();
        CHECK(backend.starts == 0);
    }
    bgm.pump();
    CHECK(backend.starts == 1 && bgm.playing() && bgm.current() == "th07_02.wav" && bgm.volume() == 0);
    CHECK(backend.stream && backend.stream->loopStart() == 0 && backend.stream->loopEnd() == 44100);
    bgm.pump();
    bgm.pump();
    CHECK(bgm.queued() == 1);
    bgm.pump();
    CHECK(bgm.queued() == 0);

    // Loop points come from the fmt (bytes / block align).
    bgm.play("bgm/th07_01.mid");
    bgm.drain();
    CHECK(backend.stream && backend.stream->loopStart() == 44100 && backend.stream->loopEnd() == 88200);

    // A slot that was never filled plays the first song.
    bgm.playLoaded(5);
    bgm.drain();
    CHECK(bgm.current() == "th07_01.wav");

    // Fades: whole seconds, linear in decibels, -21 on the first frame of a 4 s fade and
    // -4980 on its last; then the output halts (resumable).
    bgm.fadeOut(4.7f);
    bgm.pump();
    bgm.fadeStep();
    CHECK(bgm.volume() == -21);
    for (int i = 0; i < 238; ++i) bgm.fadeStep();
    CHECK(bgm.volume() == -4980);
    CHECK(std::fabs(backend.gain - static_cast<float>(std::pow(10.0, -4980 / 2000.0))) < 1e-6f);
    CHECK(!backend.paused);
    bgm.fadeStep();
    CHECK(backend.paused && bgm.playing());
    // Slow motion stretches a fade (3 s at half speed: 6 s).
    bgm.play("bgm/th07_01.mid");
    bgm.drain();
    CHECK(!backend.paused && bgm.volume() == 0);
    bgm.fadeOut(3.0f, 0.5f);
    bgm.pump();
    bgm.fadeStep();
    CHECK(bgm.volume() == 359 * 5000 / 360 - 5000);

    // Pause and resume hold and continue the song; the fade keeps counting meanwhile.
    bgm.pause();
    bgm.pump();
    CHECK(backend.paused && bgm.paused());
    bgm.fadeStep();
    CHECK(bgm.volume() == 358 * 5000 / 360 - 5000);
    bgm.resume();
    bgm.pump();
    CHECK(!backend.paused && !bgm.paused());

    // Stop, then the queue limit (31 commands; more are dropped).
    bgm.stop();
    bgm.drain();
    CHECK(!bgm.playing() && !backend.stream);
    for (int i = 0; i < 40; ++i) bgm.stop();
    CHECK(bgm.queued() == BgmPlayer::kQueueCapacity);
    bgm.drain();

    // Off mode ignores every request; MIDI mode plays the same songs from thbgm.dat.
    bgm.setMode(BgmPlayer::Off);
    bgm.play("bgm/th07_02.mid");
    CHECK(bgm.queued() == 0);
    bgm.setMode(BgmPlayer::Midi);
    bgm.play("bgm/th07_02.mid");
    bgm.drain();
    CHECK(bgm.playing() && bgm.current() == "th07_02.wav");
    bgm.pause();  // MIDI music played on through the menus
    CHECK(bgm.queued() == 0);

    // Without thbgm.dat every request is dropped, silently.
    BgmPlayer silent(&backend);
    silent.setData(records, {});
    const int starts = backend.starts;
    silent.play("bgm/th07_01.mid");
    silent.pause();
    silent.drain();
    CHECK(backend.starts == starts && !silent.playing());
}

static void dumpSynthetic(const char* dir) {
    const Music m = syntheticMusic();
    const Bytes arc = pbg4({{"th07_0100b.ver", text("0100b 466944 0\n")}, {"thbgm.fmt", m.fmt}});
    std::filesystem::create_directories(dir);
    std::ofstream(std::filesystem::path(dir) / "th07.dat", std::ios::binary)
        .write(reinterpret_cast<const char*>(arc.data()), static_cast<std::streamsize>(arc.size()));
    std::ofstream(std::filesystem::path(dir) / "thbgm.dat", std::ios::binary)
        .write(reinterpret_cast<const char*>(m.thbgm.data()), static_cast<std::streamsize>(m.thbgm.size()));
    std::printf("synthetic th07.dat and thbgm.dat written to %s\n", dir);
}

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"PBG4 archive (th07.dat)", testPbg4},
        {"version file and executable checksum", testVersionFile},
        {"music: thbgm.fmt, queue timing, fades, pause", testBgm},
        {"th07.exe tables, sound effects", testExeTablesAndSound},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-55s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    if (const char* dir = std::getenv("TH07_TEST_DUMP")) dumpSynthetic(dir);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
