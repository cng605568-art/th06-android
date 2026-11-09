// Host tests for Perfect Cherry Blossom's game code (th07_game) and the engine pieces it
// added: the PBG4 archive, thbgm.dat music, the version file, the stage systems (ANM,
// backgrounds, the player, ECL scripts and enemies). Synthetic data only; with
// TH07_TEST_DUMP=<dir> the synthetic th07.dat and thbgm.dat are also written there, to run
// the desktop build on.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "audio/MusicPlayer.hpp"
#include "io/Archive.hpp"
#include "io/MappedFile.hpp"
#include "io/Vfs.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"
#include "th07/Anm.hpp"
#include "th07/Background.hpp"
#include "th07/Bgm.hpp"
#include "th07/Bullets.hpp"
#include "th07/Ecl.hpp"
#include "th07/Effects.hpp"
#include "th07/Enemies.hpp"
#include "th07/ExeTables.hpp"
#include "th07/GameConfig.hpp"
#include "th07/InGameMenu.hpp"
#include "th07/Player.hpp"
#include "th07/World.hpp"
#include "th07/Ending.hpp"
#include "th07/English.hpp"
#include "th07/Flow.hpp"
#include "th07/MainMenu.hpp"
#include "th07/MusicRoom.hpp"
#include "th07/Replay.hpp"
#include "th07/ResultScreen.hpp"
#include "th07/ScoreFile.hpp"
#include "th07/ScreenEffects.hpp"
#include "th07/Supervisor.hpp"
#include "io/Lzss.hpp"
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
    for (int i = 0; i < 34; ++i) set32(exe, off(0x0049EFC0) + 12 * i, static_cast<std::uint32_t>(0x2C0 + i));
    set32(exe, off(0x0049EFC0) + 12 * 3 + 4, 0x0041A4F0);  // id 3: physics, big burst
    set32(exe, off(0x0049EFC0) + 12 * 3 + 8, 0x0041A5A0);
    set32(exe, off(0x0049EFC0) + 12 * 24 + 4, 0x0041ABE0);  // id 24: follow player
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

namespace {

// ---- synthetic version-2 ANM files ---------------------------------------------------------

// One instruction: opcode, time, operand mask, 4-byte argument cells.
struct Ins {
    std::int16_t op;
    std::int16_t time;
    std::vector<std::uint32_t> args;
    std::uint16_t mask = 0;
};
std::uint32_t fbits(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
Bytes encode(const std::vector<Ins>& script) {
    Bytes b;
    for (const Ins& i : script) {
        put16(b, static_cast<std::uint16_t>(i.op));
        put16(b, static_cast<std::uint16_t>(8 + 4 * i.args.size()));
        put16(b, static_cast<std::uint16_t>(i.time));
        put16(b, i.mask);
        for (std::uint32_t a : i.args) put32(b, a);
    }
    return b;
}
struct SpriteDef {
    int id;
    float x, y, w, h;
};
struct EntryDef {
    std::vector<SpriteDef> sprites;
    std::vector<std::pair<int, std::vector<Ins>>> scripts;
    int width = 16, height = 16, format = 1;  // THTX format; 0: blank '@' texture
};
// A chain of entries; each embeds a width x height texture whose pixel (x, y) has the bytes
// (x, y, entry, 255) in format 1.
Bytes anmFile(const std::vector<EntryDef>& entries) {
    Bytes file;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        const EntryDef& d = entries[e];
        Bytes ent(0x40, 0);
        const std::size_t tables = 0x40 + 4 * d.sprites.size() + 8 * d.scripts.size();
        ent.resize(tables, 0);
        set32(ent, 0x00, static_cast<std::uint32_t>(d.sprites.size()));
        set32(ent, 0x04, static_cast<std::uint32_t>(d.scripts.size()));
        set32(ent, 0x0C, static_cast<std::uint32_t>(d.width));
        set32(ent, 0x10, static_cast<std::uint32_t>(d.height));
        set32(ent, 0x14, static_cast<std::uint32_t>(d.format));
        set32(ent, 0x28, 2);
        for (std::size_t i = 0; i < d.sprites.size(); ++i) {
            set32(ent, 0x40 + 4 * i, static_cast<std::uint32_t>(ent.size()));
            put32(ent, static_cast<std::uint32_t>(d.sprites[i].id));
            for (float f : {d.sprites[i].x, d.sprites[i].y, d.sprites[i].w, d.sprites[i].h}) put32(ent, fbits(f));
        }
        for (std::size_t i = 0; i < d.scripts.size(); ++i) {
            const std::size_t pair = 0x40 + 4 * d.sprites.size() + 8 * i;
            set32(ent, pair, static_cast<std::uint32_t>(d.scripts[i].first));
            set32(ent, pair + 4, static_cast<std::uint32_t>(ent.size()));
            const Bytes code = encode(d.scripts[i].second);
            ent.insert(ent.end(), code.begin(), code.end());
        }
        set32(ent, 0x1C, static_cast<std::uint32_t>(ent.size()));
        const std::string name = d.format ? "data/test.png" : "@";
        ent.insert(ent.end(), name.begin(), name.end());
        ent.push_back(0);
        while (ent.size() % 4) ent.push_back(0);
        if (d.format) {
            set32(ent, 0x30, static_cast<std::uint32_t>(ent.size()));
            ent[0x34] = 1;
            const Bytes magic = text("THTX");
            ent.insert(ent.end(), magic.begin(), magic.end());
            put16(ent, 0);
            put16(ent, static_cast<std::uint16_t>(d.format));
            put16(ent, static_cast<std::uint16_t>(d.width));
            put16(ent, static_cast<std::uint16_t>(d.height));
            put32(ent, 0);
            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    if (d.format == 1) {  // bytes B, G, R, A
                        ent.push_back(static_cast<std::uint8_t>(e));
                        ent.push_back(static_cast<std::uint8_t>(y));
                        ent.push_back(static_cast<std::uint8_t>(x));
                        ent.push_back(255);
                    } else if (d.format == 3) {
                        put16(ent, 0xF800);  // pure red
                    } else {
                        put16(ent, 0x80F0);  // A4R4G4B4: alpha 8, red 0, green 15, blue 0
                    }
                }
            }
        }
        if (e + 1 < entries.size()) set32(ent, 0x38, static_cast<std::uint32_t>(ent.size()));
        file.insert(file.end(), ent.begin(), ent.end());
    }
    return file;
}

struct RecordingSink final : SpriteSink {
    struct Item {
        std::uint16_t texture;
        Blend blend;
        SpriteInstance s;
    };
    std::vector<Item> items;
    void draw(std::uint16_t texture, Blend blend, const SpriteInstance& s) override { items.push_back({texture, blend, s}); }
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
    CHECK(t.effects[33].script == 0x2C0 + 33 && t.effects[3].update == ExeTables::EffectUpdate::Physics &&
          t.effects[3].init == ExeTables::EffectInit::BigBurst && t.effects[24].update == ExeTables::EffectUpdate::FollowPlayer &&
          t.effects[24].init == ExeTables::EffectInit::None);
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


static void testAnmLoad() {
    // Two chained entries: entry 0 (sprites 0, 1; script 0) and entry 1 (sprite 0, script 0).
    EntryDef a;
    a.sprites = {{0, 0, 0, 8, 4}, {1, 4, 8, 4, 8}};
    a.scripts = {{0, {{3, 0, {0}}, {2, 0, {}}}}};
    EntryDef b;
    b.width = 32;
    b.height = 8;
    b.format = 3;
    b.sprites = {{0, 8, 0, 16, 8}};
    b.scripts = {{0, {{3, 0, {0}}, {-1, 1, {}}}}};
    EntryDef c;
    c.format = 0;  // a blank '@' texture
    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"test.anm", anmFile({a, b, c})}})), &err));
    TextureLibrary textures;
    AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "data/test.anm", 5, 0x300, &err));
    if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
    CHECK(lib.loaded(5) && lib.loaded(6) && lib.loaded(7) && !lib.loaded(8));
    CHECK(lib.slotIndexOffset(5) == 0x300 && lib.slotIndexOffset(6) == 0x302 && lib.slotIndexOffset(7) == 0x303);
    const auto* s1 = lib.sprite(0x301);
    CHECK(s1 && s1->pixelWidth == 4.0f && s1->pixelHeight == 8.0f && s1->uvStartX == 0.25f && s1->uvEndY == 1.0f);
    const auto* s2 = lib.sprite(0x302);
    CHECK(s2 && s2->uvStartX == 0.25f && s2->uvEndX == 0.75f && s2->textureWidth == 32.0f);
    CHECK(lib.script(0x300) && lib.script(0x302) && lib.scriptEntryOffset(0x302) == 0x302 && !lib.script(0x303));
    // Textures: format 1 (B, G, R, A bytes), format 3 (R5G6B5), and the blank one.
    const auto img0 = textures.build(lib.slotTexture(5));
    CHECK(img0 && img0->width == 16 && img0->pixel(3, 2)[0] == 3 && img0->pixel(3, 2)[1] == 2 && img0->pixel(3, 2)[2] == 0 &&
          img0->pixel(3, 2)[3] == 255);
    const auto img1 = textures.build(lib.slotTexture(6));
    CHECK(img1 && img1->pixel(1, 1)[0] == 255 && img1->pixel(1, 1)[1] == 0 && img1->pixel(1, 1)[3] == 255);
    CHECK(textures.pixels(lib.slotTexture(7)) != nullptr);
    std::vector<std::uint8_t> rgba;
    const Bytes px4444 = {0xF0, 0x80};
    CHECK(decodeThtx(px4444, 5, 1, 1, rgba) && rgba[0] == 0 && rgba[1] == 255 && rgba[2] == 0 && rgba[3] == 136);
    CHECK(!decodeThtx(px4444, 0, 1, 1, rgba) && !decodeThtx(px4444, 1, 1, 1, rgba));
    // Releasing the first slot releases the chain.
    lib.release(5);
    CHECK(!lib.loaded(5) && !lib.loaded(6) && !lib.loaded(7) && !lib.sprite(0x301) && !lib.script(0x302));
    // Version 0 (th06) files are refused.
    Bytes old = anmFile({a});
    set32(old, 0x28, 0);
    Vfs vfs2;
    vfs2.mount(openArchive("th07.dat", mem(pbg4({{"old.anm", old}})), &err));
    CHECK(!lib.load(vfs2, textures, "old.anm", 5, 0, &err));
}

static void testAnmVm() {
    g_timing = Timing{};
    const float f10 = 10.0f;
    EntryDef e;
    e.sprites = {{0, 0, 0, 8, 8}, {1, 8, 8, 8, 8}};
    // 0: timing, alpha, stop, interrupt labels.
    e.scripts.push_back({0, {{3, 0, {0}},
                             {6, 0, {fbits(10), fbits(20), fbits(0)}},
                             {8, 5, {128}},
                             {20, 10, {}},
                             {21, 10, {3}},
                             {3, 10, {1}},
                             {20, 10, {}},
                             {21, 10, {0xFFFFFFFFu}},
                             {9, 10, {0x123456}},
                             {20, 10, {}}}});
    // 1: eased move, wait, then finish keeping the last frame.
    e.scripts.push_back({1, {{3, 0, {0}},
                             {32, 0, {4, 4, fbits(100), fbits(0), fbits(0)}},
                             {79, 0, {3}},
                             {9, 0, {0xABCDEF}},
                             {2, 1, {}}}});
    // 2: registers, a register loop, a literal-counter loop (self-modifying), RNG.
    e.scripts.push_back({2, {{37, 0, {10000, 3}, 1},          // I0 = 3
                             {39, 0, {10001, 1}, 1},          // I1 += 1   (offset 16)
                             {5, 0, {10000, 16, 0}, 1},       // loop on I0
                             {39, 0, {10002, 1}, 1},          // I2 += 1   (offset 52)
                             {5, 0, {3, 52, 0}},              // loop on the literal 3
                             {59, 0, {10003, 0}, 1},          // I3 = random [0, 0): no draw
                             {59, 0, {10008, 100}, 1},        // J0 = random [0, 100): two draws
                             {60, 0, {fbits(10004), fbits(f10)}, 1},  // F0 = random float * 10: two draws
                             {38, 0, {fbits(10005), fbits(2.5f)}, 1},  // F1 = 2.5
                             {44, 0, {fbits(10005), fbits(10005)}, 3},  // F1 *= F1
                             {67, 0, {10002, 99, 0, 0}, 1},   // if I2 == 99 restart (never)
                             {2, 0, {}}}});
    // 3: colour (0x00BBGGRR cell), alpha and scale channels, flip.
    e.scripts.push_back({3, {{3, 0, {0}},
                             {9, 0, {0x000000}},
                             {33, 0, {2, 0, 0x00FF8040u}},
                             {34, 0, {4, 1, 0}},
                             {10, 0, {}},
                             {36, 0, {2, 0, fbits(3), fbits(3)}},
                             {20, 1, {}}}});
    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"t.anm", anmFile({e})}})), &err));
    TextureLibrary textures;
    AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "t.anm", 10, 0x400, &err));
    ZunRng rng(1);

    // Starting runs the time-0 instructions at once and leaves the clock at 1.
    th07::AnmVm vm;
    vm.start(lib, rng, 0x400);
    CHECK(vm.clock.current == 1 && vm.activeSprite == 0x400 && vm.position.x == 10.0f && vm.has(th07::AnmVm::Visible));
    for (int i = 0; i < 4; ++i) vm.tick(lib, rng);
    CHECK((vm.color >> 24) == 0xFF && vm.clock.current == 5);
    vm.tick(lib, rng);
    CHECK((vm.color >> 24) == 128);
    for (int i = 0; i < 6; ++i) vm.tick(lib, rng);
    CHECK(vm.has(th07::AnmVm::Stopped) && vm.clock.current == 10);  // held at the stop
    vm.tick(lib, rng);
    CHECK(vm.clock.current == 10);
    // Interrupt 3: jumps past its label (sprite 1), visible again, then stops again.
    vm.interrupt(3);
    vm.tick(lib, rng);
    CHECK(vm.activeSprite == 0x401 && !vm.pendingInterrupt);
    // An unknown interrupt falls back to the last -1 label (colour 0x123456).
    vm.interrupt(9);
    vm.tick(lib, rng);
    CHECK((vm.color & 0xFFFFFF) == 0x123456);

    // Eased move (mode 4, 4 ticks): the first tick after the set-up already uses t = 1/4.
    th07::AnmVm mv;
    mv.start(lib, rng, 0x401);
    auto ease4 = [](float t) {
        float u = 1.0f - t;
        u = u * u;
        return 1.0f - u;
    };
    CHECK(mv.position.x == 100.0f * ease4(0.25f));
    mv.tick(lib, rng);
    CHECK(mv.position.x == 100.0f * ease4(0.5f));
    // Wait 3: the start loaded it, two ticks count it down, the 3rd following tick continues.
    mv.tick(lib, rng);
    CHECK((mv.color & 0xFFFFFF) == 0xFFFFFF && mv.clock.current == 0);
    mv.tick(lib, rng);
    CHECK(mv.position.x == 100.0f && mv.duration[0].current == 0);
    CHECK((mv.color & 0xFFFFFF) == 0xABCDEF && mv.clock.current == 1);
    CHECK(mv.tick(lib, rng) && mv.finished() && mv.has(th07::AnmVm::Visible));  // opcode 2 keeps the last frame

    // Registers and loops: I0 counts 3 -> 0, I1 = 3; the literal loop runs 3 times the first
    // time; RNG draws: none for range 0, two for each random int and float.
    th07::AnmVm r;
    const std::uint32_t gens = rng.generations();
    r.start(lib, rng, 0x402);
    CHECK(r.intRegs[0] == 0 && r.intRegs[1] == 3 && r.intRegs[2] == 3 && r.intRegs[3] == 0);
    CHECK(rng.generations() == gens + 4 && r.intRegs2[0] >= 0 && r.intRegs2[0] < 100);
    CHECK(r.floatRegs[0] >= 0.0f && r.floatRegs[0] < 10.0f && r.floatRegs[1] == 6.25f);
    // The literal counter was written into the script bytes: the next run loops once.
    th07::AnmVm r2;
    r2.start(lib, rng, 0x402);
    CHECK(r2.intRegs[2] == 1);

    // Colour channel: the target cell is 0x00BBGGRR; alpha fades to 0 with ease 1; scale
    // interpolation does not re-apply the flip.
    th07::AnmVm c;
    c.start(lib, rng, 0x403);
    c.tick(lib, rng);
    CHECK((c.color & 0xFFFFFF) == 0x4080FF);
    CHECK(c.scale.x == 3.0f && c.flip() == 1);
    c.tick(lib, rng);
    c.tick(lib, rng);
    CHECK((c.color >> 24) == 0);

    // Timers under the game-speed multiplier and the force-integer flag.
    g_timing.speed = 0.5f;
    Timer t;
    t.tick();
    CHECK(t.current == 0 && t.sub == 0.5f);
    t.tick();
    CHECK(t.current == 1 && t.sub == 0.0f);
    t.decrement(1);
    CHECK(t.current == 0 && t.sub == 0.5f);
    g_timing = Timing{};
    g_timing.forceInteger = true;
    Timer u;
    u.assign(10);
    u.decrement(3);
    CHECK(u.current == 6);  // v + 1
    u.increment(2);
    CHECK(u.current == 9);
    g_timing = Timing{};
}

static void testAnmDraw() {
    g_timing = Timing{};
    EntryDef e;
    e.width = 64;
    e.height = 64;
    e.sprites = {{0, 0, 0, 33, 16}};
    e.scripts = {{0, {{3, 0, {0}}, {2, 0, {}}}}};
    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"d.anm", anmFile({e})}})), &err));
    TextureLibrary textures;
    AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "d.anm", 0, 0, &err));
    ZunRng rng;
    th07::AnmVm vm;
    vm.start(lib, rng, 0);
    vm.position = {100.25f, 50.0f, 0.0f};
    RecordingSink sink;
    AnmDrawState state;
    // Unrotated: rounded to the pixel grid (half-even). Left 100.25 - 16.5 = 83.75 -> 84, right 116.75 -> 117.
    drawVm(sink, vm, state);
    CHECK(sink.items.size() == 1 && sink.items[0].s.w == 33.0f && sink.items[0].s.x == 100.5f && sink.items[0].s.h == 16.0f &&
          sink.items[0].s.y == 50.0f && sink.items[0].blend == Blend::Alpha);
    CHECK(sink.items[0].s.u1 == 33.0f / 64.0f && sink.items[0].s.v1 == 0.25f);
    // Anchored top-left, additive, shaken by the global offset.
    vm.flags |= th07::AnmVm::AnchorLeft | th07::AnmVm::AnchorTop | th07::AnmVm::Additive;
    state.offsetX = 2.0f;
    drawVm(sink, vm, state);
    CHECK(sink.items[1].s.x == 102.0f + 16.5f && sink.items[1].s.y == 58.0f && sink.items[1].blend == Blend::Additive);
    // Rotated: not rounded, +0.5 for GL's pixel centres.
    vm.flags &= ~(th07::AnmVm::AnchorLeft | th07::AnmVm::AnchorTop);
    state.offsetX = 0;
    vm.rotation.z = 0.5f;
    drawVm(sink, vm, state);
    CHECK(sink.items[2].s.x == 100.75f && sink.items[2].s.y == 50.5f && sink.items[2].s.angle == 0.5f);
    // Colour: alternative colour, then the stage multiplier (0x80 = identity, 0xFF ~ x2).
    vm.altColor = 0x80402010u;
    vm.flags |= th07::AnmVm::UseAltColor;
    state.multiply = true;
    state.multiplyColor = 0x80FF8040u;
    CHECK(effectiveColor(vm, state) == 0x807F2008u);
    // Not drawn when invisible, inactive or with a zero primary alpha.
    vm.color &= 0x00FFFFFFu;
    drawVm(sink, vm, state);
    CHECK(sink.items.size() == 3);
}


static void testGameConfig() {
    GameConfig fresh;
    CHECK(!fresh.load({}, true) && fresh.music == GameConfig::MusicWav && fresh.lives == 2 && fresh.colourMode == 255);
    CHECK(!fresh.load({}, false) && fresh.music == GameConfig::MusicMidi);
    const std::vector<std::uint8_t> bytes = fresh.save();
    CHECK(bytes.size() == 56 && bytes[0x14] == 0x02 && bytes[0x16] == 0x07 && bytes[0x34] == 1 && bytes[0x26] == 1);
    // A valid file round-trips (option bits: bit 0 forced on, bit 7 off).
    GameConfig c = fresh;
    c.lives = 4;
    c.slowMode = 1;
    c.difficulty = 5;
    c.optionBits = 0x2080;
    c.colourMode = 1;
    GameConfig back;
    CHECK(back.load(c.save(), true) && back.lives == 4 && back.difficulty == 5 && back.optionBits == 0x2001 && back.colourMode == 1);
    // Invalid (lives 5): defaults, but the padding, unused bytes and option bits survive.
    std::vector<std::uint8_t> bad = c.save();
    bad[0x1C] = 5;
    bad[0x12] = 0xAA;
    bad[0x30] = 0x55;
    GameConfig re;
    CHECK(!re.load(bad, false) && re.lives == 2 && re.slowMode == 0 && re.music == GameConfig::MusicMidi && re.padding[0] == 0xAA &&
          re.unused[0x30 - 0x27] == 0x55 && re.optionBits == 0x2001);
    // Wrong size or version: rebuilt.
    std::vector<std::uint8_t> shortFile(bad.begin(), bad.begin() + 40);
    CHECK(!re.load(shortFile, true) && re.lives == 2);
    std::vector<std::uint8_t> oldVersion = fresh.save();
    oldVersion[0x14] = 0x01;
    CHECK(!re.load(oldVersion, true));
}

namespace {
struct Sink3D final : SpriteSink {
    struct Quad3D {
        Vertex3D v[4];
        Draw3DState state;
    };
    std::vector<Quad3D> quads;
    struct Clear {
        std::uint32_t color;
        bool colorToo;
    };
    std::vector<Clear> clears;
    void draw(std::uint16_t, Blend, const SpriteInstance&) override {}
    void drawQuad3D(std::uint16_t, Blend, const Vertex3D* c, const Draw3DState& s) override {
        Quad3D q;
        for (int i = 0; i < 4; ++i) q.v[i] = c[i];
        q.state = s;
        quads.push_back(q);
    }
    void clearRect(float, float, float, float, std::uint32_t color, bool colorToo) override { clears.push_back({color, colorToo}); }
};
void putF(Bytes& b, float f) { put32(b, fbits(f)); }
void putVec(Bytes& b, float x, float y, float z) {
    putF(b, x);
    putF(b, y);
    putF(b, z);
}
}  // namespace

static void testBackground() {
    g_timing = Timing{};
    EntryDef e;
    e.sprites = {{0, 0, 0, 8, 8}};
    e.scripts.push_back({0, {{3, 0, {0}}, {31, 0, {1}}, {20, 1, {}}}});  // sprite 0, stage camera, stop
    // stage1.std: one object (layer 0, one 100x100 quad), three instances (one visible, one
    // behind the camera, one too far), and a script.
    Bytes std(0x490, 0);
    const char song[] = "bgm/th07_02.mid";
    std::memcpy(std.data() + 0x290, song, sizeof song);
    put32(std, 0x494);  // object 0
    put16(std, 0);
    std.push_back(0);   // layer 0
    std.push_back(0);
    putVec(std, 0, 0, 0);  // origin
    putVec(std, 0, 0, 0);  // size
    put16(std, 0);         // quad: type 0, 0x1C bytes, script 0, at the origin, 100x100
    put16(std, 0x1C);
    put16(std, 0);
    put16(std, 0);
    putVec(std, 0, 0, 0);
    putF(std, 100);
    putF(std, 100);
    put16(std, 0xFFFF);  // end of quads
    std.resize(std.size() + 0x1A, 0);
    const auto instancesAt = static_cast<std::uint32_t>(std.size());
    for (float z : {0.0f, -400.0f, 2000.0f}) {
        put16(std, 0);
        put16(std, 0);
        putVec(std, 0, 0, z);
    }
    put16(std, 0xFFFF);
    std.resize(std.size() + 14, 0);
    const auto scriptAt = static_cast<std::uint32_t>(std.size());
    auto rec = [&](std::int32_t frame, std::int16_t op, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        put32(std, static_cast<std::uint32_t>(frame));
        put16(std, static_cast<std::uint16_t>(op));
        put16(std, 20);
        put32(std, a);
        put32(std, b);
        put32(std, c);
    };
    rec(0, 5, fbits(0), fbits(0), fbits(-200));  // camera position
    rec(0, 7, fbits(0), fbits(0), fbits(1));     // facing
    rec(0, 9, fbits(0), fbits(1), fbits(0));     // up
    rec(10, 5, fbits(0), fbits(100), fbits(-200));
    rec(10, 6, 10, 0, 0);  // move there over 10 frames, linearly
    rec(20, 3, 0, 0, 0);   // halt
    rec(30, 31, 7, 0, 0);  // label 7
    rec(30, 13, 0xFF102030u, 0, 0);  // clear colour
    rec(-1, -1, 0, 0, 0);
    set32(std, 0, 0x00010001u);  // 1 object, 1 quad
    set32(std, 4, instancesAt);
    set32(std, 8, scriptAt);

    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"stg1bg.anm", anmFile({e})}, {"stage1.std", std}})), &err));
    TextureLibrary textures;
    AnmLibrary lib;
    ZunRng rng(1);
    Background bg;
    CHECK(bg.load(vfs, textures, lib, rng, 1, &err));
    if (!bg.loaded()) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return;
    }
    CHECK(bg.songPath(0) == "bgm/th07_02.mid" && bg.songPath(1).empty());

    bg.update(lib, rng);
    CHECK(bg.cameraPosition().z == -200.0f && bg.normalizedFacing().z == 1.0f && bg.scriptTime() == 1);
    Sink3D sink;
    AnmDrawState state;
    bg.drawHigh(sink, state);
    bg.drawLow(sink, state);
    CHECK(sink.clears.size() == 1 && !sink.clears[0].colorToo);
    CHECK(sink.quads.size() == 1);  // the instances behind and too far away are culled
    if (sink.quads.size() == 1) {
        const auto& q = sink.quads[0];
        // A quad facing the camera 200 units ahead: centred on the playfield (pixel centres at
        // +0.5), symmetric, at eye depth 200 and inside the depth range.
        auto px = [](const Vertex3D& v) { return (v.x / v.w + 1.0f) * 320.0f; };
        auto py = [](const Vertex3D& v) { return (1.0f - v.y / v.w) * 240.0f; };
        const float cx = (px(q.v[0]) + px(q.v[1])) / 2.0f, cy = (py(q.v[0]) + py(q.v[2])) / 2.0f;
        CHECK(std::fabs(cx - 224.5f) < 1e-3f && std::fabs(cy - 240.5f) < 1e-3f);
        const float half = std::tan(kZunPi / 12.0f) * 200.0f;  // half the visible height at that depth
        CHECK(std::fabs(std::fabs(py(q.v[0]) - py(q.v[2])) - 448.0f * 100.0f / (2.0f * half)) < 0.05f);
        CHECK(std::fabs(q.v[0].eyeZ - 200.0f) < 1e-3f && std::fabs(q.v[3].eyeZ - 200.0f) < 1e-3f);
        CHECK(q.v[0].z > -q.v[0].w && q.v[0].z < q.v[0].w && q.state.clipX == 32.0f && q.state.clipH == 448.0f);
    }

    // The camera moves from frame 10 to 19, linearly. Script time freezes as soon as the next
    // pending record is a halt, before the halt's own frame (a quirk of the original).
    for (int k = 1; k <= 10; ++k) bg.update(lib, rng);
    CHECK(std::fabs(bg.cameraPosition().y - 10.0f) < 1e-4f && bg.scriptTime() == 10);
    for (int k = 11; k <= 19; ++k) bg.update(lib, rng);
    CHECK(bg.cameraPosition().y == 100.0f && bg.scriptTime() == 10);
    bg.update(lib, rng);
    bg.update(lib, rng);
    CHECK(bg.scriptTime() == 10 && bg.cameraPosition().y == 100.0f);
    // The ECL resumes the script at label 7: time jumps to its frame, the clear colour is set.
    bg.setResumeLabel(7);
    bg.update(lib, rng);
    CHECK(bg.scriptTime() == 31);
    sink.clears.clear();
    bg.drawHigh(sink, state);
    CHECK(sink.clears.size() == 1 && sink.clears[0].colorToo && sink.clears[0].color == rgba(0x10, 0x20, 0x30, 0xFF));
    bg.release(lib);
    CHECK(!bg.loaded() && !lib.loaded(5));
}

namespace {
// A shot file: one level (threshold 999) firing one shot every 5 frames, straight up.
Bytes shotFile(float speed, float diagonal, std::int32_t window) {
    Bytes b;
    put16(b, 0);
    put16(b, 1);       // levels
    putF(b, 3.0f);     // initial bombs
    put32(b, static_cast<std::uint32_t>(window));
    putF(b, 4.0f);     // hitbox
    putF(b, 40.0f);    // graze box
    putF(b, 8.0f);     // auto-collect speed
    putF(b, 32.0f);    // item box
    putF(b, 0.5f);     // cherry penalty
    putF(b, 128.0f);   // point of collection
    putF(b, speed);
    putF(b, speed / 2.0f);
    putF(b, diagonal);
    putF(b, diagonal / 2.0f);
    put32(b, 0x3C);    // level 0: entries at 0x3C
    put32(b, 999);
    put16(b, 5);       // interval
    put16(b, 0);       // phase
    putF(b, 0.0f);
    putF(b, -10.0f);   // offset
    putF(b, 8.0f);
    putF(b, 8.0f);     // box
    putF(b, -kZunPi / 2.0f);
    putF(b, 10.0f);    // speed
    put16(b, 3);       // damage
    b.push_back(0);    // origin: player
    b.push_back(0);    // ordinary
    put16(b, 0x400 + 64);  // script
    put16(b, 0xFFFF);  // silent
    for (int i = 0; i < 4; ++i) put32(b, 0);
    put16(b, 0xFFFF);  // end of list
    b.resize(b.size() + 0x32, 0);
    return b;
}
}  // namespace

static void testPlayer() {
    g_timing = Timing{};
    EntryDef e;
    e.sprites = {{0, 0, 0, 8, 8}};
    for (int id : {0, 1, 2, 3, 4, 64, 128, 129}) e.scripts.push_back({id, {{3, 0, {0}}, {20, 1, {}}}});
    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"player00.anm", anmFile({e})},
                                                {"ply00a.sht", shotFile(4.0f, 3.0f, 8)},
                                                {"ply00as.sht", shotFile(2.0f, 1.5f, 8)}})),
                          &err));
    TextureLibrary textures;
    AnmLibrary lib;
    ZunRng rng(7);
    GameState g;
    g.setDifficulty(Normal);
    World w;
    w.state = &g;
    w.rng = &rng;
    w.anm = &lib;
    Player p;
    w.player = &p;
    CHECK(p.load(vfs, textures, lib, 0, true, &err));
    if (!lib.loaded(10)) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return;
    }
    CHECK(p.header().initialBombs == 3.0f && p.header().deathbombWindow == 8 && p.shots().focused.speed == 2.0f);
    CHECK(p.shots().unfocused.entriesFor(0).size() == 1 && p.shots().unfocused.entriesFor(0)[0].damage == 3);
    g.lives = 2;
    g.bombs = 3;
    p.stageStart(w);
    CHECK(p.state() == Player::State::Spawning);
    // A stage starts spawning with timer 120: the first spawn-in step makes the player
    // invulnerable for 240 frames at once.
    p.update(w, 0);
    CHECK(p.state() == Player::State::Invulnerable);
    // Movement: 10 frames right at 4 px, then clamped at the edge; diagonals 3 px per axis.
    for (int i = 0; i < 10; ++i) p.update(w, BtnRight);
    CHECK(p.position().x == 232.0f && p.position().y == 384.0f);
    p.update(w, BtnUp | BtnLeft);
    CHECK(p.position().x == 229.0f && p.position().y == 381.0f);
    for (int i = 0; i < 100; ++i) p.update(w, BtnDown);
    CHECK(p.position().y == 432.0f);
    // The fire cycle: one shot every 5 frames while Shot is held (cycle frames 0, 5, ...).
    for (int i = 0; i < 6; ++i) p.update(w, BtnShoot);
    bool bomb = false;
    const int dealt = p.damageQuery(w, {229.0f, 432.0f - 10.0f - 10.0f * 5.0f, 0.0f}, {16, 16, 0}, bomb);
    CHECK(dealt == 3 && !bomb);  // the first shot, 6 frames up
    // Become vulnerable, then die: 20 steps at the hit, 8 at the commit after the deathbomb
    // window, 4 at the respawn move.
    for (int i = 0; i < 300 && p.state() != Player::State::Alive; ++i) p.update(w, 0);
    CHECK(p.state() == Player::State::Alive);
    std::uint32_t before = rng.generations();
    CHECK(p.bulletHit(w, p.position(), {4, 4, 0}) == Player::HitResult::Hit);
    CHECK(p.state() == Player::State::Dead && rng.generations() - before == 20);
    before = rng.generations();
    for (int i = 0; i < 8; ++i) p.update(w, 0);
    CHECK(g.misses == 1.0f && rng.generations() - before == 8);  // committed on the 8th frame
    before = rng.generations();
    int frames = 0;
    while (p.state() == Player::State::Dead && frames < 60) {
        p.update(w, 0);
        ++frames;
    }
    CHECK(p.state() == Player::State::Spawning && g.lives == 1.0f && g.bombs == 3.0f && rng.generations() - before == 4);
    CHECK(p.position().x == 192.0f && p.position().y == 384.0f);
    // A deathbomb: hit, then Bomb inside the window. 8 steps for the bomb; no commit.
    for (int i = 0; i < 400 && p.state() != Player::State::Alive; ++i) p.update(w, 0);
    p.bulletHit(w, p.position(), {4, 4, 0});
    p.update(w, 0);
    before = rng.generations();
    p.update(w, BtnBomb);
    CHECK(p.state() == Player::State::Invulnerable && g.bombs == 2.0f && g.bombsUsed == 1.0f && g.bombActive);
    CHECK(rng.generations() - before == 8);
    for (int i = 0; i < 20; ++i) p.update(w, 0);
    CHECK(g.misses == 1.0f && p.state() == Player::State::Invulnerable);
    lib.release(10);
}

static void testGameState() {
    GameState g;
    g.setDifficulty(Normal);
    // Extend thresholds.
    const std::int32_t expect[] = {50, 125, 200, 300, 450, 800, 1000, 1200};
    bool ok = true;
    for (int n = 0; n < 8; ++n) {
        g.extendsGranted = n;
        ok = ok && g.extendThreshold() == expect[n];
    }
    CHECK(ok);
    g.setDifficulty(Extra);
    g.extendsGranted = 3;
    CHECK(g.extendThreshold() == 1300 && g.minRank == 15 && g.maxRank == 16);
    // Rank: subrank carries, clamped.
    g.setDifficulty(Easy);
    g.increaseSubrank(1000);
    CHECK(g.rank == 20 && g.subRank == 0);
    g.decreaseSubrank(1650);
    CHECK(g.rank == 12 && g.subRank == 50);
    // Score roll: 1/32 of the difference per frame, never slowing down while rolling.
    g.score = 3200;
    g.guiScore = 0;
    g.hiScore = 100;
    g.rollScore();
    CHECK(g.guiScore == 100 && g.rollSpeed == 100 && g.hiScore == 100);
    g.rollScore();
    CHECK(g.guiScore == 200);  // step 96 < the running 100
    for (int i = 0; i < 100; ++i) g.rollScore();
    CHECK(g.guiScore == 3200 && g.rollSpeed == 0 && g.hiScore == 3200);
    // Cherry and extends through World.
    ZunRng rng(3);
    World w;
    w.state = &g;
    w.rng = &rng;
    g.setDifficulty(Normal);
    g.cherry = 0;
    g.cherryMax = 200000;
    g.cherryPlus = 0;
    w.cherryAddWithGauge(300);
    CHECK(g.cherry == 300 && g.cherryPlus == 300);
    w.cherryAdd(1000);
    CHECK(g.cherry == 1300 && g.cherryPlus == 300);
    w.cherryRaise(500000);
    CHECK(g.cherry == 200000);
    w.cherryRaiseMax(20000000);
    CHECK(g.cherryMax == 9999990);
    g.lives = 7;
    g.bombs = 8;
    g.extendsGranted = 0;
    g.extendPoints = 130;
    const std::uint32_t before = rng.generations();
    w.checkPointExtends();
    // Two extends (50, 125): a life (7 -> 8), then nothing to give (lives 8, bombs 8).
    CHECK(g.lives == 8.0f && g.extendsGranted == 2 && g.nextExtend == 200 && rng.generations() - before == 4);
}

// ---- ECL and enemies -------------------------------------------------------------------------

namespace {

// One ECL instruction: time, opcode, argument words, variable mask, difficulty mask.
struct EIns {
    std::int32_t time;
    std::int16_t op;
    std::vector<std::uint32_t> args;
    std::uint16_t mask = 0;
    std::uint8_t diff = 0xFF;
};
// One timeline record.
struct TRec {
    std::int16_t time, arg0, op;
    float x = 0, y = 0, z = 0;
    std::int32_t life = 0, item = 0, score = 0;
};
std::uint32_t fw(float f) { return fbits(f); }
std::uint32_t iw(std::int32_t i) { return static_cast<std::uint32_t>(i); }
void set16(Bytes& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// An .ecl file: header, the subs (each closed by the time -1 / opcode -1 marker), then the
// timelines (each closed by a record of time -1).
Bytes eclFile(const std::vector<std::vector<EIns>>& subs, const std::vector<std::vector<TRec>>& timelines) {
    Bytes b(0x44 + 4 * subs.size(), 0);
    set16(b, 0, static_cast<std::uint16_t>(subs.size()));
    set16(b, 2, static_cast<std::uint16_t>(timelines.size()));
    for (std::size_t s = 0; s < subs.size(); ++s) {
        set32(b, 0x44 + 4 * s, static_cast<std::uint32_t>(b.size()));
        for (const EIns& i : subs[s]) {
            put32(b, static_cast<std::uint32_t>(i.time));
            put16(b, static_cast<std::uint16_t>(i.op));
            put16(b, static_cast<std::uint16_t>(12 + 4 * i.args.size()));
            b.push_back(0);
            b.push_back(i.diff);
            put16(b, i.mask);
            for (std::uint32_t a : i.args) put32(b, a);
        }
        put32(b, 0xFFFFFFFFu);
        put16(b, 0xFFFF);
        put16(b, 12);
        b.push_back(0);
        b.push_back(0xFF);
        put16(b, 0xFF);
    }
    for (std::size_t t = 0; t < timelines.size(); ++t) {
        set32(b, 4 + 4 * t, static_cast<std::uint32_t>(b.size()));
        auto rec = [&](const TRec& r) {
            put16(b, static_cast<std::uint16_t>(r.time));
            put16(b, static_cast<std::uint16_t>(r.arg0));
            put16(b, static_cast<std::uint16_t>(r.op));
            put16(b, 32);
            putF(b, r.x);
            putF(b, r.y);
            putF(b, r.z);
            put32(b, static_cast<std::uint32_t>(r.life));
            put32(b, static_cast<std::uint32_t>(r.item));
            put32(b, static_cast<std::uint32_t>(r.score));
        };
        for (const TRec& r : timelines[t]) rec(r);
        rec(TRec{-1, 0, 0});
    }
    return b;
}

struct RecordingItems final : ItemField {
    struct Item {
        Vec3f pos;
        int type, state;
    };
    std::vector<Item> spawned;
    void spawn(const Vec3f& pos, int type, int state) override { spawned.push_back({pos, type, state}); }
    void collectAll() override {}
    void stopHoming() override {}
};

struct RecordingGui final : StageGui {
    struct Card {
        int number, portrait;
        std::string name;
    };
    std::vector<Card> cards;
    int ends = 0;
    std::vector<std::uint32_t> bonuses, spellBonuses;
    bool wait = false;
    std::vector<int> dialogues;
    void startDialogue(int n) override { dialogues.push_back(n); }
    bool dialogueWait() const override { return wait; }
    void spellCardStart(int card, int portrait, const std::string& name) override { cards.push_back({card, portrait, name}); }
    void spellCardEnd() override { ++ends; }
    void bonus(std::uint32_t p) override { bonuses.push_back(p); }
    void spellCardBonus(std::uint32_t p) override { spellBonuses.push_back(p); }
};

// A stage with a synthetic stg1enm.anm (sprite and script 0: a 16x16 sprite), the given ECL
// as ecldata1.ecl, the synthetic th07.exe tables, and (optionally) a player that shoots
// 3-damage shots straight up.
struct EnemyWorld {
    Vfs vfs;
    TextureLibrary textures;
    AnmLibrary lib;
    ZunRng rng{0x1234};
    GameState g;
    World w;
    ExeTables tables;
    std::unique_ptr<Player> player = std::make_unique<Player>();
    std::unique_ptr<BulletManager> bullets = std::make_unique<BulletManager>();
    std::unique_ptr<EnemyManager> enemies = std::make_unique<EnemyManager>();
    std::unique_ptr<Effects> effects = std::make_unique<Effects>();
    RecordingItems items;
    RecordingGui gui;
    bool ok = false;

    void frame(std::uint16_t input = 0) {
        if (w.player) player->update(w, input);
        enemies->update(w);
    }
};

std::unique_ptr<EnemyWorld> enemyWorld(const Bytes& ecl, bool withPlayer, int difficulty = Normal) {
    g_timing = Timing{};
    g_eclGlobals = EclGlobals{};
    auto ew = std::make_unique<EnemyWorld>();
    EntryDef enm;
    enm.sprites = {{0, 0, 0, 16, 16}};
    enm.scripts = {{0, {{3, 0, {0}}, {20, 1, {}}}}};
    EntryDef ply;
    ply.sprites = {{0, 0, 0, 8, 8}};
    for (int id : {0, 1, 2, 3, 4, 64, 128, 129}) ply.scripts.push_back({id, {{3, 0, {0}}, {20, 1, {}}}});
    // etama.anm section 0: 16 bullet colours (8x8) and the pellet's five scripts.
    EntryDef etama;
    etama.width = etama.height = 128;
    for (int c = 0; c < 16; ++c) etama.sprites.push_back({c, static_cast<float>(8 * c), 0, 8, 8});
    for (int id : {0, 15, 18, 19, 20}) etama.scripts.push_back({id, {{3, 0, {0}}, {20, 1, {}}}});
    std::string err;
    ew->vfs.mount(openArchive("th07.dat",
                              mem(pbg4({{"stg1enm.anm", anmFile({enm})},
                                        {"etama.anm", anmFile({etama})},
                                        {"ecldata1.ecl", ecl},
                                        {"player00.anm", anmFile({ply})},
                                        {"ply00a.sht", shotFile(4.0f, 3.0f, 8)},
                                        {"ply00as.sht", shotFile(2.0f, 1.5f, 8)}})),
                              &err));
    if (!loadExeTables(syntheticExe(), ew->tables, &err, false)) return ew;
    GameState& g = ew->g;
    g.setDifficulty(difficulty);
    g.stage = 1;
    g.shotIndex = 0;
    g.lives = 2;
    g.bombs = 3;
    World& w = ew->w;
    w.state = &g;
    w.rng = &ew->rng;
    w.anm = &ew->lib;
    w.tables = &ew->tables;
    w.items = &ew->items;
    w.gui = &ew->gui;
    w.bullets = ew->bullets.get();
    w.bulletManager = ew->bullets.get();
    w.enemies = ew->enemies.get();
    w.effects = ew->effects.get();
    ew->effects->reset();
    if (withPlayer) {
        w.player = ew->player.get();
        if (!ew->player->load(ew->vfs, ew->textures, ew->lib, 0, true, &err)) return ew;
        ew->player->stageStart(w);
    }
    if (!ew->lib.load(ew->vfs, ew->textures, "data/etama.anm", 11, 0x200, &err)) return ew;
    ew->bullets->stageStart(w);
    if (!ew->enemies->stageStart(w, ew->vfs, ew->textures, 1, &err)) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return ew;
    }
    ew->ok = true;
    return ew;
}

std::int32_t wordAt(const EclFile& f, std::uint32_t off) { return eclI32(f.at(off)); }

}  // namespace

static void testEclFile() {
    const Bytes ecl = eclFile({{{0, 4, {iw(10000), 7}, 1}}, {{5, 0, {}}}}, {{TRec{0, 1, 0, 1, 2, 3}}});
    EclFile f;
    std::string err;
    CHECK(f.load(ecl, &err));
    CHECK(f.subCount() == 2 && f.timelineCount() == 1);
    CHECK(f.subOffset(0) == 0x44 + 8 && f.subOffset(1) == 0x44 + 8 + 20 + 12);
    CHECK(eclI16(f.at(f.subOffset(0)) + 4) == 4 && eclU16(f.at(f.subOffset(0)) + 6) == 20 && f.validInstruction(f.subOffset(0)));
    CHECK(eclI32(f.at(f.subOffset(1))) == 5 && f.validInstruction(f.subOffset(1)));
    CHECK(eclI16(f.at(f.timelineOffset(0)) + 4) == 0 && eclF32(f.at(f.timelineOffset(0)) + 8) == 1.0f);
    CHECK(!f.validInstruction(f.size() - 4));
    // Header and offset validation.
    CHECK(!f.load(Bytes(0x20, 0), &err) && !f.loaded());
    Bytes bad = ecl;
    set32(bad, 0x44, static_cast<std::uint32_t>(ecl.size()));
    CHECK(!f.load(bad, &err));
    bad = ecl;
    set16(bad, 2, 17);
    CHECK(!f.load(bad, &err));
    bad = ecl;
    set32(bad, 4, static_cast<std::uint32_t>(ecl.size() - 4));
    CHECK(!f.load(bad, &err));
    // x86 conversions.
    CHECK(x86ToInt(-2.9f) == -2 && x86ToInt(2147483648.0f) == INT32_MIN && x86ToInt(std::nanf("")) == INT32_MIN);
    CHECK(x86ToInt(-2147483648.0f) == INT32_MIN && x86ToInt(1e10f) == INT32_MIN);
    CHECK(x86ToU32(3000000000.0f) == 3000000000u && x86ToU32(-1.0f) == 0xFFFFFFFFu && x86ToU32(std::nanf("")) == 0);
    CHECK(eclEase(1, 0.5f) == 0.25f && eclEase(4, 0.5f) == 0.75f && eclEase(9, 0.3f) == 0.3f);
}

static void testEclArguments() {
    const float randomVar = 10004.7f;  // truncates to 10004 (float A[0]): float-side ids are floats
    const Bytes ecl = eclFile(
        {
            // Sub 0: variables, literals, unknown ids, self-modifying writes.
            {
                {0, 4, {iw(10000), 7}, 0x1},                   // int A[0] = 7
                {0, 4, {123, 42}},                             // literal destination: writes the script word
                {0, 5, {fw(randomVar), fw(2.5f)}, 0x1},        // float A[0] = 2.5
                {0, 4, {iw(10001), iw(10057)}, 0x3},           // int read of 10057 returns the id
                {0, 5, {fw(10005.0f), fw(99999.0f)}, 0x3},     // unknown float id: the word itself
                {0, 12, {iw(10002), iw(10000), 5}, 0x3},       // int A[2] = A[0] + 5
                {0, 17, {55}},                                 // a literal increments itself
                {0, 5, {fw(10006.0f), fw(10018.0f)}, 0x3},     // float A[2] = position x
                {0, 4, {iw(10003), iw(10000)}, 0x1},           // mask bit 1 clear: the literal 10000
                {0, 5, {fw(10050.0f), fw(3.0f)}, 0x1},         // writable enemy field (orbit centre x)
            },
            // Sub 1: a literal loop counter counts itself down inside the script.
            {
                {0, 4, {iw(10000), 0}, 0x1},
                {1, 17, {iw(10000)}, 0x1},
                {1, 3, {1, iw(-16), 3}},
            },
        },
        {});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    const std::uint32_t sub0 = m.ecl().subOffset(0);
    Enemy& a = m.spawn(0, {100, 50, 0}, 10, 0, 0, false);
    CHECK(a.active && a.life == 10);
    CHECK(a.ctx.args.intA[0] == 7 && a.ctx.args.floatA[0] == 2.5f && a.ctx.args.intA[1] == 10057);
    CHECK(a.ctx.args.floatA[1] == 99999.0f && a.ctx.args.intA[2] == 12 && a.ctx.args.floatA[2] == 100.0f);
    CHECK(a.ctx.args.intA[3] == 10000 && a.moveOrigin.x == 3.0f);
    CHECK(wordAt(m.ecl(), sub0 + 20 + 12) == 42);           // the second instruction's argument 0
    CHECK(wordAt(m.ecl(), sub0 + 5 * 20 + 24 + 12) == 56);  // the increment's literal
    // Variable reads and the writable maps.
    CHECK(m.readInt(a, 10027) == 10 && m.readInt(a, 10018) == 100 && m.readInt(a, 99) == 99);
    CHECK(m.readFloat(a, 10019.9f) == 50.0f && m.readFloat(a, 1.5f) == 1.5f);
    CHECK(EnemyManager::intWritable(10016) && !EnemyManager::intWritable(10055) && !EnemyManager::intWritable(10018));
    CHECK(EnemyManager::floatWritable(10021) && !EnemyManager::floatWritable(10024) && !EnemyManager::floatWritable(10000));
    CHECK(m.writeInt(a, 10070, 300) && a.itemDrop == 44);  // a signed byte field
    // Loops: the first run gets the full count, later runs find the counter used up.
    Enemy& b = m.spawn(1, {0, 0, 0}, 1, 0, 0, false);
    ew->frame();
    CHECK(b.ctx.args.intA[0] == 3);
    Enemy& c = m.spawn(1, {0, 0, 0}, 1, 0, 0, false);
    ew->frame();
    CHECK(c.ctx.args.intA[0] == 1 && wordAt(m.ecl(), m.ecl().subOffset(1) + 20 + 16 + 12 + 8) == -1);
}

static void testEclRandomVariables() {
    const Bytes ecl = eclFile(
        {
            {{0, 4, {iw(10000), iw(10055)}, 0x3}},                // u32: 2 steps
            {{0, 5, {fw(10004.0f), fw(10055.0f)}, 0x3}},          // float in [0, 1]: 2 steps
            {{0, 4, {iw(10000), iw(10056)}, 0x3}},                // u32InRange(G'[0] = 0): no draw
            {{0, 5, {fw(10004.0f), fw(10060.0f)}, 0x3}},          // random angle: 2 steps
            {{0, 4, {iw(10000), iw(10060)}, 0x3}},                // the int read of 10060 is the id: no draw
            {{0, 6, {iw(10000), 0}, 0x1}},                        // u32InRange(0): no draw
            {{0, 8, {fw(10004.0f), fw(10.0f)}, 0x1}},             // floatInRange: 2 steps
            {{0, 10, {iw(10000), 5}, 0x1}},                       // +-1: one u16 step
            {{0, 117, {0, 1, iw(10055)}, 0x4}},                   // colour through the writable map: no draw
        },
        {});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    ZunRng& rng = ew->rng;
    const int expect[] = {2, 2, 0, 2, 0, 0, 2, 1, 0};
    bool counts = true;
    for (int s = 0; s < 9; ++s) {
        ZunRng ref = rng;
        const std::uint32_t before = rng.generations();
        Enemy& e = m.spawn(s, {0, 0, 0}, 1, 0, 0, false);
        counts = counts && rng.generations() - before == static_cast<std::uint32_t>(expect[s]);
        switch (s) {
            case 0: CHECK(e.ctx.args.intA[0] == static_cast<std::int32_t>(ref.u32())); break;
            case 1: CHECK(e.ctx.args.floatA[0] == ref.f32ZeroToOne()); break;
            case 2: CHECK(e.ctx.args.intA[0] == 0); break;
            case 3: CHECK(e.ctx.args.floatA[0] == ref.f32InRange(kZunTwoPi) - kZunPi); break;
            case 4: CHECK(e.ctx.args.intA[0] == 10060); break;
            case 7: CHECK(e.ctx.args.intA[0] == ((ref.u16() & 1) ? 5 : -5)); break;
            default: break;
        }
        e.active = false;
    }
    CHECK(counts);
    CHECK(ew->effects->normal(0).vm.color == 10055u);  // the literal colour word
    // 10056 with bounds: u32InRange(G'[0]) + G'[1] and floatInRange(F'[0]) + F'[1].
    Enemy& e = m.spawn(2, {0, 0, 0}, 1, 0, 0, false);
    e.ctx.args.intG = {10, 1000, 0, 0};
    e.ctx.args.floatG = {2.0f, 5.0f, 0, 0};
    ZunRng ref = rng;
    CHECK(m.readInt(e, 10056) == static_cast<std::int32_t>(ref.u32() % 10) + 1000);
    CHECK(m.readFloat(e, 10056.0f) == ref.f32InRange(2.0f) + 5.0f);
    CHECK(rng.generations() == ref.generations());
}

static void testEclTimelines() {
    const Bytes ecl = eclFile(
        {
            {{0, 95, {0}}, {0, 47, {fw(0.0f), fw(1.0f), fw(0.0f)}}},  // sub 0: sprite, falls 1 px per frame
            {{0, 95, {0}}, {0, 110, {77}}},                          // sub 1: sets its life at time 0
        },
        {{
            TRec{3, 0, 0, 100, 50, 0, 30, 2, 500},
            TRec{3, 0, 2, 200, 50, 0, 40, -1, 700},
            TRec{4, 1, 0, 150, 60, 0, 30, 0, 0},
            TRec{5, 0, 9},                         // wait for the dialogue
            TRec{5, 0, 8},                         // dialogue 3 + character x 10
            TRec{6, 0, 4, -999, 70, 0, 5, 0, 0},   // random x
            TRec{7, 0, 0, 10, 10, 0, 5, 0, 0},     // skipped while a boss is present
        }});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    const std::uint32_t start = ew->rng.generations();
    for (int i = 0; i < 3; ++i) ew->frame();
    CHECK(m.liveCount() == 0 && !m.enemy(0).active);
    ew->frame();  // timeline clock 3: two spawns, each with its first step, then their loop step
    const Enemy& a = m.enemy(0);
    const Enemy& b = m.enemy(1);
    CHECK(a.active && b.active && m.liveCount() == 2);
    CHECK(a.pos.x == 100.0f && a.pos.y == 51.0f && a.life == 30 && a.maxLife == 30 && a.score == 500 && a.itemDrop == 2);
    CHECK(b.mirror && b.itemDrop == -1 && b.score == 700 && b.life == 40);
    ew->frame();
    CHECK(m.enemy(2).active && m.enemy(2).life == 77 && m.enemy(2).maxLife == 77);  // the script's life wins
    ew->gui.wait = true;
    ew->frame();
    ew->frame();
    CHECK(ew->gui.dialogues.empty());
    ew->gui.wait = false;
    ew->frame();
    CHECK(ew->gui.dialogues.size() == 1 && ew->gui.dialogues[0] == 0);
    ZunRng ref = ew->rng;
    ew->frame();
    CHECK(m.enemy(3).active && m.enemy(3).pos.x == ref.f32InRange(368.0f) && m.enemy(3).pos.y == 71.0f);
    ew->g.boss.present = true;
    ew->frame();
    CHECK(!m.enemy(4).active);
    // The stage start draws the two random-drop counters (u16 steps), the counter first.
    CHECK(start > 0 && m.dropCounter() < 3 && m.dropIndex() < 8);
    EnemyManager other;
    ZunRng ref2 = ew->rng;
    std::string err;
    CHECK(other.stageStartWith(ew->w, ecl, &err) && ew->rng.generations() - ref2.generations() == 2);
    CHECK(other.dropCounter() == ref2.u16InRange(3) && other.dropIndex() == ref2.u16InRange(8));
}

static void testEclMovement() {
    const Bytes ecl = eclFile(
        {
            {{0, 95, {0}}, {0, 55, {10, 0, fw(150.0f), fw(100.0f), fw(0.0f)}}},                     // interpolate
            {{0, 95, {0}}, {0, 49, {fw(2.0f)}}, {0, 50, {fw(0.5f)}}},                              // polar + accel
            {{0, 95, {0}}, {0, 56, {0, fw(192.0f), fw(224.0f), fw(0.0f), fw(0.0f), fw(0.0f), fw(50.0f), fw(0.0f)}}},
            {{0, 95, {0}}, {0, 62, {fw(32.0f), fw(32.0f), fw(352.0f), fw(416.0f)}}, {0, 46, {fw(0.0f), fw(500.0f), fw(0.0f)}}},
            {{0, 95, {0}}, {0, 55, {10, 4, fw(150.0f), fw(100.0f), fw(0.0f)}}},                     // ease-out
            {{0, 95, {0}}, {0, 27, {fw(10018.0f), 4, 0, 0, fw(100.0f), fw(140.0f), 0, 0}}},         // interpolator on x
        },
        {});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    Enemy& in = m.spawn(0, {100, 100, 0}, 1, 0, 0, false);
    Enemy& po = m.spawn(1, {100, 200, 0}, 1, 0, 0, false);
    Enemy& orb = m.spawn(2, {0, 0, 0}, 1, 0, 0, false);
    Enemy& cl = m.spawn(3, {0, 0, 0}, 1, 0, 0, false);
    Enemy& ease = m.spawn(4, {100, 300, 0}, 1, 0, 0, false);
    Enemy& ip = m.spawn(5, {100, 350, 0}, 1, 0, 0, false);
    CHECK(in.vm.sprite != nullptr && in.moveMode == Enemy::Interpolate && in.moveDuration == 10);
    CHECK(cl.pos.x == 32.0f && cl.pos.y == 416.0f);  // clamped to the bounds
    ew->frame();
    CHECK(in.pos.x == 110.0f && in.pos.y == 100.0f);  // 2 of 10 steps (the spawn step's velocity is replaced)
    CHECK(po.pos.x == 103.0f && po.speed == 3.0f);
    CHECK(orb.pos.x == 242.0f && orb.pos.y == 224.0f);
    CHECK(ease.pos.x > 110.0f);
    ew->frame();
    CHECK(po.pos.x == 106.5f && ip.pos.x == 130.0f);
    for (int i = 0; i < 10; ++i) ew->frame();
    CHECK(in.pos.x == 150.0f && in.pos.y == 100.0f && in.moveMode == Enemy::Axis && in.velocity.x == 0.0f);
    CHECK(ease.pos.x == 150.0f);
    // The interpolator reached 140 on the 4th step; its last velocity is kept (axis mode).
    CHECK(ip.pos.x == 230.0f && ip.velocity.x == 10.0f && ip.ctx.interps[0].routine == EclInterp::None);
}

static void testEclVmControl() {
    const Bytes ecl = eclFile(
        {
            // Sub 0: call sub 1, then copy G'[0] (the caller's own copy, restored on return).
            {{0, 41, {1}}, {0, 4, {iw(10001), iw(10029)}, 0x3}, {0, 45, {3}}, {0, 4, {iw(10002), 1}, 0x1}},
            // Sub 1: the call refreshed G' from the globals.
            {{0, 4, {iw(10000), iw(10029)}, 0x3}, {0, 4, {iw(10038), iw(10000)}, 0x3}, {0, 42, {}}},
            // Sub 2: an interrupt into sub 3, which returns past the 109.
            {{0, 108, {3, 1}}, {0, 109, {1}}, {0, 4, {iw(10001), 9}, 0x1}},
            {{0, 4, {iw(10000), 5}, 0x1}, {0, 42, {}}},
            // Sub 4: a difficulty-masked instruction (Lunatic only) and a jump.
            {{0, 4, {iw(10000), 1}, 0x1, 0x08}, {0, 2, {5, 40}}, {0, 4, {iw(10001), 1}, 0x1}, {5, 4, {iw(10002), 1}, 0x1}},
        },
        {});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    g_eclGlobals.ints[0] = 5;
    Enemy& a = m.spawn(0, {0, 0, 0}, 1, 0, 0, false);
    CHECK(g_eclGlobals.ints[1] == 5);                           // the callee saw the refreshed copy
    CHECK(a.ctx.args.intA[0] == 0 && a.ctx.args.intA[1] == 0);  // the caller's block came back
    CHECK(a.depth == 0 && a.ctx.args.intA[2] == 0);
    ew->frame();
    ew->frame();
    CHECK(a.ctx.args.intA[2] == 0);
    ew->frame();
    CHECK(a.ctx.args.intA[2] == 1);  // three frames after the wait
    Enemy& b = m.spawn(2, {0, 0, 0}, 1, 0, 0, false);
    CHECK(b.ctx.args.intA[0] == 0 && b.ctx.args.intA[1] == 9 && b.depth == 0);  // args restored on return
    Enemy& c = m.spawn(4, {0, 0, 0}, 1, 0, 0, false);
    CHECK(c.ctx.args.intA[0] == 0 && c.ctx.args.intA[1] == 0 && c.ctx.args.intA[2] == 1 && c.ctx.time.current == 6);
    g_eclGlobals = EclGlobals{};
}

static void testEnemyDeathAndDamage() {
    const Bytes ecl = eclFile(
        {
            {{0, 95, {0}}, {3, 4, {iw(10027), 0}, 0x1}},  // dies by script at time 3
            {{0, 95, {0}}},                               // a target for the player's shots
        },
        {});
    auto ew = enemyWorld(ecl, true);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    GameState& g = ew->g;
    Enemy& a = m.spawn(0, {100, 100, 0}, 5, ItemField::Bomb, 1000, false);
    for (int i = 0; i < 4 && a.active; ++i) ew->frame();
    CHECK(!a.active && g.score == 100 && (g.eventFlags & 0x20));
    CHECK(ew->items.spawned.size() == 1 && ew->items.spawned[0].type == ItemField::Bomb && ew->items.spawned[0].state == 0);
    CHECK(ew->items.spawned[0].pos.x == 100.0f);
    // Random drops: every third death, from the table, the index wrapping at 32.
    const std::uint16_t counter = m.dropCounter(), index = m.dropIndex();
    ew->items.spawned.clear();
    Enemy& r = m.spawn(0, {50, 50, 0}, 5, -1, 0, false);
    for (int i = 0; i < 4 && r.active; ++i) ew->frame();
    CHECK(m.dropCounter() == counter + 1);
    if (counter % 3 == 0) {
        CHECK(ew->items.spawned.size() == 1 && ew->items.spawned[0].type == ew->tables.dropCycle[index] && m.dropIndex() == index + 1);
    } else {
        CHECK(ew->items.spawned.empty() && m.dropIndex() == index);
    }
    // Shot damage: 3 per hit, Cherry +10 per hit (stage 1, unfocused), death and its drop.
    ew->items.spawned.clear();
    g.cherry = 0;
    g.score = 0;
    Enemy& t = m.spawn(1, {192, 300, 0}, 9, ItemField::Point, 300, false);
    int frames = 0, hits = 0, lastLife = t.life;
    while (t.active && frames < 120) {
        ew->frame(BtnShoot);
        if (t.life < lastLife) {
            CHECK(lastLife - t.life == 3 && t.damageThisFrame == 3);
            ++hits;
            lastLife = t.life;
        }
        ++frames;
    }
    CHECK(!t.active && hits == 3 && g.cherry == 30 && g.score == 30);
    CHECK(ew->items.spawned.size() == 1 && ew->items.spawned[0].type == ItemField::Point);
}


static void testEnemySpellCards() {
    // The card name: "Test Card", XOR 0xAA over 48 bytes.
    std::vector<std::uint32_t> card(13, 0xAAAAAAAAu);
    card[0] = (5u << 16) | 2u;  // portrait 2, card 5
    const std::string name = "Test Card";
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto shift = 8 * (i % 4);
        card[1 + i / 4] = (card[1 + i / 4] & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(name[i] ^ 0xAA) << shift);
    }
    const Bytes ecl = eclFile(
        {
            // Sub 0: a boss with a 120-frame card whose timeout runs sub 1.
            {{0, 95, {0}}, {0, 99, {0}}, {0, 114, {120}}, {0, 115, {1}}, {0, 90, card}},
            {{0, 91, {}}},
            // Sub 2: a 600-frame card ended by the script at time 10.
            {{0, 95, {0}}, {0, 99, {0}}, {0, 114, {600}}, {0, 90, card}, {10, 91, {}}},
        },
        {});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    GameState& g = ew->g;
    Enemy& boss = m.spawn(0, {192, 100, 0}, 1000, 0, 0, false);
    CHECK(boss.boss && m.bossSlot(0) == 0 && g.boss.present);
    CHECK(g.spellCardActive && g.capturing && g.captureValue == 5000 && m.spellCard().drain == 5000 / 12);
    CHECK(ew->gui.cards.size() == 1 && ew->gui.cards[0].number == 5 && ew->gui.cards[0].portrait == 2 && ew->gui.cards[0].name == name);
    ew->frame();
    CHECK(g.captureValue == 4990);  // 5000 - 1 x 416 / 60, rounded down to 10
    g.cherry = 10000;
    for (int i = 0; i < 119; ++i) ew->frame();
    CHECK(g.spellCardActive && m.spellCard().active == 1 && g.boss.spellSeconds == 0);
    ew->frame();  // the timer callback: timed out, Cherry -25 %, the timeout sub ends the card
    CHECK(!g.capturing && g.captureValue == 0 && g.cherry == 7500);
    CHECK(!g.spellCardActive && m.spellCard().active == 0 && ew->gui.ends == 1);
    CHECK(ew->gui.spellBonuses.empty() && ew->gui.bonuses.empty() && g.spellsCaptured == 0);
    // A captured card: capture value + graze bonus.
    boss.active = false;
    g.score = 0;
    Enemy& b2 = m.spawn(2, {192, 100, 0}, 1000, 0, 0, false);
    CHECK(b2.boss && g.spellCardActive && g.captureValue == 5000);
    g.grazeBonus = 2500;
    for (int i = 0; i < 10; ++i) ew->frame();
    CHECK(!g.spellCardActive && ew->gui.ends == 2 && g.spellsCaptured == 1 && ew->gui.spellBonuses.size() == 1);
    const std::uint32_t bonus = ew->gui.spellBonuses.empty() ? 0 : ew->gui.spellBonuses[0];
    CHECK(bonus > 2500 && bonus % 10 == 0 && g.score == bonus / 10);
}

static void testEnemyCallbacks() {
    const Bytes ecl = eclFile(
        {
            // 0: a boss with a life callback at 50 into sub 1.
            {{0, 95, {0}}, {0, 99, {0}}, {0, 112, {50}}, {0, 113, {1}}},
            {{0, 4, {iw(10000), 77}, 0x1}},
            // 2: a plain enemy (killed by the callback).
            {{0, 95, {0}}},
            // 3: a boss with death type 3, no death effects.
            {{0, 95, {0}}, {0, 99, {1}}, {0, 106, {3}}, {0, 98, {0xFF}}},
            // 4: flies up and off the screen.
            {{0, 95, {0}}, {0, 47, {fw(0.0f), fw(-10.0f), fw(0.0f)}}},
            // 5: a periodic callback every 2 restarts into sub 6.
            {{0, 144, {2, 6}}},
            {{0, 17, {iw(10000)}, 0x1}, {0, 42, {}}},
            // 7: a timer callback after 5 frames into sub 8 (an ordinary enemy).
            {{0, 95, {0}}, {0, 114, {5}}, {0, 115, {8}}},
            {{0, 4, {iw(10001), 1}, 0x1}},
            // 9: death type 2 (items only, dies again every frame).
            {{0, 95, {0}}, {0, 106, {2}}, {0, 98, {0xFF}}},
            // 10: the game-speed routines (Youmu's slow-down) and its restore.
            {{0, 121, {10, 2}}, {10, 121, {11, 0}}, {11, 121, {11, 3}}},
        },
        {});
    auto ew = enemyWorld(ecl, false);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    GameState& g = ew->g;
    Enemy& boss = m.spawn(0, {100, 100, 0}, 100, 0, 0, false);
    Enemy& minion = m.spawn(2, {200, 100, 0}, 30, 0, 100, false);
    CHECK(boss.boss && !minion.boss && minion.active);
    boss.life = 40;
    ew->frame();  // the callback: life back to 50, its sub runs in the same frame, the minion dies
    CHECK(boss.life == 50 && boss.lifeThresholds[0] == -1 && boss.ctx.args.intA[0] == 77 && boss.active);
    CHECK(!minion.active && g.score == 10);
    // Death type 3: the boss stays, cannot be damaged, and leaves the HUD.
    Enemy& b3 = m.spawn(3, {150, 150, 0}, 10, 0, 0, false);
    CHECK(g.boss.present && b3.bossId == 1 && b3.death1 == -1);
    b3.life = 0;
    g.eventFlags = 0;
    ew->frame();
    CHECK(b3.active && b3.life == 1 && !b3.damageable && b3.deathType == 0 && !g.boss.present && (g.eventFlags & 0x20));
    // Off the screen after having been on it: despawned.
    Enemy& up = m.spawn(4, {100, 60, 0}, 10, 0, 0, false);
    int frames = 0;
    while (up.active && frames < 30) {
        ew->frame();
        ++frames;
    }
    CHECK(!up.active && frames == 7);  // y 50, 40, ..., -10; off once y + 8 < 0
    // The periodic sub keeps its own locals; every restart ticks its counter (its own return
    // too), so with interval 2 it runs on every frame after the first [QUIRK].
    Enemy& per = m.spawn(5, {0, 0, 0}, 1, 0, 0, false);
    for (int i = 0; i < 4; ++i) ew->frame();
    CHECK(per.periodicArgs.intA[0] == 3 && per.ctx.args.intA[0] == 0 && per.depth == 0);
    // A timer callback on an ordinary enemy also costs 25 % of the Cherry [QUIRK].
    g.cherry = 1000;
    Enemy& timed = m.spawn(7, {300, 100, 0}, 10, 0, 0, false);
    for (int i = 0; i < 5; ++i) ew->frame();
    CHECK(timed.ctx.args.intA[1] == 0 && g.cherry == 1000);
    ew->frame();
    CHECK(timed.ctx.args.intA[1] == 1 && g.cherry == 750 && timed.timerThreshold == -1);
    // Death type 2 drops its item every frame until the script intervenes [QUIRK].
    ew->items.spawned.clear();
    Enemy& d2 = m.spawn(9, {250, 250, 0}, 10, ItemField::Point, 0, false);
    d2.life = 0;
    ew->frame();
    ew->frame();
    // (The timer callback's kill-all also zeroed the other non-boss enemies' life: the
    // periodic one dies now and drops its type-0 item.)
    int points = 0;
    for (const auto& it : ew->items.spawned) points += it.type == ItemField::Point && it.pos.x == 250.0f;
    CHECK(d2.active && points == 2 && !per.active);
    // Ex routines 10 and 11: the game runs at 1/2 speed, frame timers tick fractionally; the
    // restore's own argument (3) sets the sticky force-integer flag.
    Enemy& slow = m.spawn(10, {0, 0, 0}, 1, 0, 0, false);
    CHECK(g_timing.speed == 0.5f && slow.ctx.time.current == 0 && slow.ctx.time.sub == 0.5f);
    for (int i = 0; i < 19; ++i) ew->frame();
    CHECK(slow.ctx.time.current == 10 && g_timing.speed == 0.5f);
    ew->frame();
    CHECK(g_timing.speed == 1.0f && !g_timing.forceInteger);
    ew->frame();
    CHECK(g_timing.speed == 1.0f && g_timing.forceInteger);
    g_timing = Timing{};
}

static void testEnemyExRoutines() {
    const Bytes ecl = eclFile(
        {
            // Four colour-2 pellets whose command chain already moved past slot 0 (a spawn delay
            // chains into slot 1 at once), then ex routine 1 one frame later.
            {{0, 95, {0}},
             {0, 79, {0, 0x2000, 0, 5, 0, 0, 0}},
             {0, 65, {(2u << 16) | 0u, 4, 1, fw(1.0f), fw(1.0f), fw(0.0f), fw(0.3f), 0x2000}},
             {1, 121, {1, 0}},
             {2, 121, {18, 0}}},
        },
        {});
    auto ew = enemyWorld(ecl, true);
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    BulletManager& bm = *ew->bullets;
    m.spawn(0, {192, 100, 0}, 10, 0, 0, false);
    int fired = 0;
    for (int i = 0; i < BulletManager::kBullets; ++i) {
        const auto& b = bm.bullet(i);
        if (b.state == 0) continue;
        ++fired;
        CHECK(b.vm.sprite != nullptr && b.color == 2 && b.cursor == 1 && b.tag == 0);
    }
    CHECK(fired == 4);
    const std::uint32_t before = ew->rng.generations();
    ew->enemies->update(ew->w);  // ex routine 1: one float draw per matching bullet
    CHECK(ew->rng.generations() - before == 8);
    bm.update(ew->w);
    int turning = 0;
    for (int i = 0; i < BulletManager::kBullets; ++i) {
        const auto& b = bm.bullet(i);
        if (b.state == 0) continue;
        if ((b.active & 0x20) && b.tag == 1 && b.speed == 0.3f + 1.0f / 60.0f && b.cursor == 1) ++turning;
    }
    CHECK(turning == 4);
}

static void testEnemyBullets() {
    const Bytes ecl = eclFile(
        {
            // A fan of 3 bullets, speed 2: rank-adjusted outside spell cards.
            {{0, 95, {0}}, {0, 65, {0, 3, 1, fw(2.0f), fw(1.0f), fw(0.0f), fw(0.1f), 0}}},
            // The same with count and speed from variables (mask bits 2 and 4).
            {{0, 95, {0}}, {0, 4, {iw(10000), 2}, 0x1}, {0, 65, {0, iw(10000), 1, fw(10004.0f), fw(1.0f), fw(0.0f), fw(0.1f), 0}, 0x14}},
        },
        {});
    auto ew = enemyWorld(ecl, true);  // patterns aim at the player
    CHECK(ew->ok);
    if (!ew->ok) return;
    EnemyManager& m = *ew->enemies;
    ew->g.rank = 32;  // speed + 32 x 0.3 / 32 - 0.15 = +0.15
    Enemy& e = m.spawn(0, {192, 100, 0}, 10, 0, 0, false);
    int live = 0;
    float speed = 0;
    for (int i = 0; i < BulletManager::kBullets; ++i) {
        if (ew->bullets->bullet(i).state != 0) {
            ++live;
            speed = ew->bullets->bullet(i).speed;
        }
    }
    CHECK(live == 3 && speed == 2.0f + 0.15f && e.shooter.count1 == 3 && e.shooter.aimMode == 1);
    Enemy& f = m.spawn(1, {192, 100, 0}, 10, 0, 0, false);
    CHECK(f.shooter.count1 == 2 && f.shooter.speed1 == 0.0f);  // speed 0 (variable, unset) is not adjusted
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
    // Checksums above 2^31 are written unsigned and read as 32-bit words.
    const auto big = findVersionLine(text("0100b 650752 2932163676\r\n"), kVersionTag);
    CHECK(big && big->size == 650752 && big->checksum == static_cast<std::int32_t>(2932163676u));
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

// ---- the game flow: LZSS encoder, score.dat, unlock rules, input, supervisor ----------------

static void testExeStringFormat() {
    using touhou::th07::FormatArg;
    using touhou::th07::formatExeString;
    const FormatArg clock[3] = {{7}, {5}, {9}};
    CHECK(formatExeString("T %.2d:%.2d:%.2d", clock) == "T 07:05:09");
    const FormatArg row[3] = {{0, "AB", true}, {12}, {-3}};
    CHECK(formatExeString("%s %6d %6d", row) == "AB     12     -3");
    const FormatArg pad[2] = {{0, "x", true}, {42}};
    CHECK(formatExeString("[%-4s][%05d]%%", pad) == "[x   ][00042]%");
    CHECK(formatExeString("%6d %6d", std::span<const FormatArg>(row + 1, 1)) == "    12      0");  // missing: 0
    CHECK(formatExeString("%x %d%", std::span<const FormatArg>(clock, 1)) == "%x 7%");           // unknown: copied
    // Shift-JIS bytes pass through untouched (their trail bytes are never %).
    const std::string sjis = "\x83\x76\x83\x8C %d";
    CHECK(formatExeString(sjis, std::span<const FormatArg>(clock, 1)) == "\x83\x76\x83\x8C 7");
}

static void testScreenEffects() {
    g_timing = Timing{};
    // Number of 16-bit draws between two generator states.
    auto drawsBetween = [](std::uint16_t from, std::uint16_t to) {
        ZunRng r(from);
        for (int n = 0; n < 100000; ++n) {
            if (r.currentSeed() == to) return n;
            r.u16();
        }
        return -1;
    };
    GameState g;
    g.stageFrame = 100;
    ZunRng rng(1234);
    float ox = 0, oy = 0;

    // A shake of duration D makes two u32 draws on each of D - 1 frames, then ends.
    ScreenEffects fx;
    fx.shake(10, 12.0f, 0.0f);
    const std::uint16_t before = rng.currentSeed();
    int frames = 0;
    while (fx.size() > 0 && frames < 100) {
        fx.update(rng, g, ox, oy);
        ++frames;
    }
    CHECK(frames == 10 && drawsBetween(before, rng.currentSeed()) == 4 * 9);
    // Time stop holds it (no draws); the stage's first frames end it at once.
    fx.shake(10, 12.0f, 0.0f);
    g.timeStop = true;
    std::uint16_t s0 = rng.currentSeed();
    fx.update(rng, g, ox, oy);
    CHECK(fx.size() == 1 && rng.currentSeed() == s0);
    g.timeStop = false;
    g.stageFrame = 1;
    fx.update(rng, g, ox, oy);
    CHECK(fx.size() == 0 && rng.currentSeed() == s0);
    g.stageFrame = 100;

    // A pulse: alpha falls from the colour's alpha over D frames, one alpha-0 frame between
    // cycles, later cycles starting at timer 1. Nothing is drawn from the RNG.
    struct Probe : SpriteSink {
        std::vector<std::uint32_t> colors;
        std::vector<float> widths;
        void draw(std::uint16_t, Blend, const SpriteInstance& s) override {
            colors.push_back(s.color);
            widths.push_back(s.w);
        }
    };
    SpriteRegion white;
    fx.pulse(4, 2, 0x80FFCFCFu);
    std::vector<std::uint32_t> alphas;
    s0 = rng.currentSeed();
    for (int i = 0; i < 20 && fx.size() > 0; ++i) {
        fx.update(rng, g, ox, oy);
        if (fx.size() == 0) break;
        Probe p;
        fx.draw(p, white);
        CHECK(p.colors.size() == 1 && p.widths[0] == kPlayfieldW);
        alphas.push_back(p.colors.empty() ? 999u : p.colors[0] >> 24);  // bytes R, G, B, A
    }
    CHECK(rng.currentSeed() == s0);
    // 0x80 = 128: 128, 96, 64, 32, then 0, then 96, 64, 32 (timer 1..3), then it ends.
    const std::vector<std::uint32_t> expect = {128, 96, 64, 32, 0, 96, 64, 32};
    CHECK(alphas.size() == expect.size());
    if (alphas.size() == expect.size()) {
        bool same = true;
        for (std::size_t i = 0; i < expect.size(); ++i) same = same && alphas[i] == expect[i];
        CHECK(same);
    }

    // Fade to white over the screen (dialogue opcode 14): alpha k * 255 / D for k < D, then
    // removed without a full-alpha frame.
    fx.clear();
    fx.fade(4, 402, 0x00FFFFFFu);
    int drawn = 0;
    std::uint32_t lastAlpha = 0, firstAlpha = 999;
    for (int i = 0; i < 500 && fx.size() > 0; ++i) {
        fx.update(rng, g, ox, oy);
        if (fx.size() == 0) break;
        Probe p;
        fx.draw(p, white);
        if (p.colors.size() == 1 && p.widths[0] == 640.0f) {
            const std::uint32_t a = p.colors[0] >> 24;
            if (firstAlpha == 999) firstAlpha = a;
            lastAlpha = a;
            ++drawn;
        }
    }
    CHECK(drawn == 402 && firstAlpha == 0 && lastAlpha == 401 * 255 / 402);
    // Fade from a colour: starts opaque.
    fx.fade(0, 30, 0x00000000u);
    fx.update(rng, g, ox, oy);
    Probe p;
    fx.draw(p, white);
    CHECK(p.colors.size() == 1 && (p.colors[0] >> 24) == 255);
}

static void testPlayfieldCamera() {
    g_timing = Timing{};
    EntryDef e;
    e.width = e.height = 512;
    e.format = 0;
    e.sprites = {{0, 0, 0, 128, 128}};
    e.scripts.push_back({0, {{3, 0, {0}}}});
    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"p.anm", anmFile({e})}})), &err));
    TextureLibrary textures;
    AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "p.anm", 4, 0x724, &err));
    if (!lib.loaded(4)) return;
    ZunRng rng(1);
    AnmVm vm;
    vm.startFresh(lib, rng, 0x724);
    vm.flags |= AnmVm::Visible | AnmVm::Active;
    struct Probe : SpriteSink {
        std::vector<Vertex3D> v;
        Draw3DState ds;
        void draw(std::uint16_t, Blend, const SpriteInstance&) override {}
        void drawQuad3D(std::uint16_t, Blend, const Vertex3D* c, const Draw3DState& st) override {
            v.assign(c, c + 4);
            ds = st;
        }
    };
    auto px = [](float ndc) { return (ndc + 1.0f) * 320.0f - 0.5f; };  // back to Direct3D pixels
    auto py = [](float ndc) { return (1.0f - ndc) * 240.0f - 0.5f; };
    Background bg;
    AnmDrawState none;
    // At depth 0 the playfield camera maps field coordinates 1:1 onto the playfield.
    vm.position = {192.0f, 224.0f, 0.0f};
    Probe p;
    bg.drawProjected(p, vm, none);
    CHECK(p.v.size() == 4);
    if (p.v.size() == 4) {
        CHECK(std::fabs(px(p.v[0].x) - (32 + 192 - 64)) < 0.01f && std::fabs(py(p.v[0].y) - (16 + 224 - 64)) < 0.01f);
        CHECK(std::fabs(px(p.v[3].x) - (32 + 192 + 64)) < 0.01f && std::fabs(py(p.v[3].y) - (16 + 224 + 64)) < 0.01f);
        // On top of everything, never writing depth, clipped to the playfield.
        CHECK(p.v[0].z == -1.0f && p.v[0].w == 1.0f && !p.ds.depthWrite && p.ds.clipX == 32.0f && p.ds.clipW == 384.0f);
    }
    // Nearer the camera (positive z) it grows about its projected centre.
    vm.position = {100.0f, 100.0f, 200.0f};
    Probe q;
    bg.drawProjected(q, vm, none);
    if (q.v.size() == 4) {
        const float w = px(q.v[1].x) - px(q.v[0].x);
        const float eye = 224.0f / std::tan(kZunPi / 20.0f);
        const float sc = eye / (eye - 200.0f);
        CHECK(std::fabs(w - 128.0f * sc) < 0.05f);
        const float cx = (px(q.v[0].x) + px(q.v[1].x)) / 2.0f;
        CHECK(std::fabs(cx - (32.0f + 192.0f + (100.0f - 192.0f) * sc)) < 0.05f);
    }
}

static void testInGameMenus() {
    g_timing = Timing{};
    // capture.anm's shape: a blank 512x512 texture and a 128x128 backdrop sprite at the origin.
    EntryDef e;
    e.width = e.height = 512;
    e.format = 0;
    e.sprites = {{0, 0, 0, 128, 128}};
    e.scripts.push_back({0, {{3, 0, {0}}, {7, 0, {fbits(3.0f), fbits(3.5f)}}}});
    Vfs vfs;
    std::string err;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"capture.anm", anmFile({e})}})), &err));
    TextureLibrary textures;
    AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "capture.anm", 4, 0x724, &err));
    if (!lib.loaded(4)) return;
    // The snapshot is magnified 3 x 3.5: sampled bilinearly, as every th07 texture is.
    CHECK(textures.filter(lib.slotTexture(4)) == TextureFilter::LinearWrap);
    ZunRng rng(3);
    GameState g;
    g.setDifficulty(Normal);
    World w;
    w.state = &g;
    w.rng = &rng;
    w.anm = &lib;
    using R = InGameMenu::Request;
    auto run = [&](InGameMenu& m, std::uint16_t pressed, int limit, int& passes) {
        passes = 0;
        R r = R::None;
        while (r == R::None && passes < limit) {
            r = m.update(w, pressed);
            ++passes;
        }
        return r;
    };

    InGameMenu m;
    int x = -1, y = -1, cw = -1, ch = -1;
    m.captureRect(x, y, cw, ch);
    CHECK(cw == 0 && ch == 0 && !m.takeCaptureRequest());
    // Pause: the opening pass asks for one snapshot of the backdrop sprite's rectangle.
    m.openPause(w);
    CHECK(m.active() && m.pauseMenu());
    CHECK(m.update(w, 0) == R::None);
    CHECK(m.takeCaptureRequest() && !m.takeCaptureRequest());
    m.captureRect(x, y, cw, ch);
    CHECK(x == 0 && y == 0 && cw == 128 && ch == 128);
    // Menu closes it 20 passes later.
    CHECK(m.update(w, BtnMenu) == R::None);
    int passes = 0;
    CHECK(run(m, 0, 100, passes) == R::Resume && passes == 20 && !m.active());

    // Continue: the cursor starts on No, which takes input from 30 passes on; giving up
    // reaches the results 20 passes after the choice.
    m.openContinue(w, 3);
    CHECK(m.update(w, 0) == R::None && m.takeCaptureRequest());
    CHECK(run(m, BtnShoot, 200, passes) == R::Results && passes == 29 + 1 + 20);
    // Moving to Yes (no timer reset) and confirming continues 30 passes later.
    m.openContinue(w, 3);
    CHECK(m.update(w, 0) == R::None);
    for (int i = 0; i < 29; ++i) m.update(w, BtnUp);  // locked
    CHECK(m.update(w, BtnUp) == R::None);              // to Yes
    CHECK(run(m, BtnShoot, 200, passes) == R::Continue && passes == 1 + 30);
    // No credits left: the results at once.
    g.continues = 3;
    m.openContinue(w, 3);
    CHECK(m.update(w, 0) == R::Results && !m.active());
}

static void testLzssEncoder() {
    auto roundTrip = [](const Bytes& in) {
        const Bytes packed = lzss::compress(in);
        Bytes out(in.size(), 0xEE);
        const std::size_t n = lzss::decompress(packed, out);
        return n == in.size() && out == in;
    };
    CHECK(roundTrip({}));
    CHECK(roundTrip({42}));
    CHECK(roundTrip(text("abcabcabcabcabcabcabcabcabc")));
    Bytes zeros(20000, 0);
    CHECK(roundTrip(zeros));
    CHECK(lzss::compress(zeros).size() < 2600);  // 18-byte matches of 18 bits (literals: 22500)
    ZunRng rng(7);
    Bytes noise(30000);
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng.u16() >> 3);
    CHECK(roundTrip(noise));
    // Repeats far apart (beyond what the window keeps) and near the 8 KiB wrap.
    Bytes mixed;
    for (int i = 0; i < 40000; ++i) mixed.push_back(static_cast<std::uint8_t>((i % 8191) * 7 + (i / 9000)));
    CHECK(roundTrip(mixed));
    Bytes records;
    for (int i = 0; i < 300; ++i) {
        const Bytes r = text("HSCR(\0(\0\1\0\0\0");
        records.insert(records.end(), r.begin(), r.end());
        put32(records, static_cast<std::uint32_t>(i * 1000));
    }
    CHECK(roundTrip(records));
    // The stream ends with the terminator: a decoder given room to spare stops exactly.
    const Bytes abc = text("hello hello hello");
    Bytes big(64, 0);
    CHECK(lzss::decompress(lzss::compress(abc), big) == abc.size());
}

namespace {
ScoreEntry entry(std::uint32_t score, const char* name, int stage = 3, int continues = 0) {
    ScoreEntry e;
    e.score = score;
    std::memcpy(e.name.data(), name, std::min<std::size_t>(8, std::strlen(name)));
    for (std::size_t i = std::strlen(name); i < 8; ++i) e.name[i] = ' ';
    e.name[8] = 0;
    e.stage = static_cast<std::uint8_t>(stage);
    e.continues = static_cast<std::int8_t>(continues);
    e.date = {'1', '1', '/', '0', '8', 0};
    e.slow = 1.5f;
    return e;
}
}  // namespace

static void testScoreFile() {
    // High-score insertion: before the first entry scoring no more (a tie ranks the newer first).
    ScoreList l;
    CHECK(ScoreFile::insert(l, entry(500, "A")) == 0);
    CHECK(ScoreFile::insert(l, entry(700, "B")) == 0);
    CHECK(ScoreFile::insert(l, entry(500, "C")) == 1);
    CHECK(l.size() == 3 && l[1].name[0] == 'C' && l[2].name[0] == 'A');
    std::array<ScoreList, kDifficulties * kShots> lists;
    ScoreFile::defaultLists(lists);
    const ScoreList& d = lists[ScoreFile::listIndex(2, 3)];
    CHECK(d.size() == 10 && d[0].score == 100000 && d[9].score == 10000 && d[0].placeholder && d[0].shot == 3 && d[0].difficulty == 2);
    CHECK(std::string(d[0].name.data()) == "--------" && std::string(d[0].date.data()) == "--/--");
    // A file entry equal to a placeholder ranks above it.
    CHECK(ScoreFile::insert(lists[ScoreFile::listIndex(2, 3)], entry(90000, "TIE")) == 1);

    // A whole file: written, read back.
    ScoreData data;
    data.clears[2].noContinue = {3, 99, 1, 1, 6, 0};
    data.clears[2].reached = {5, 99, 2, 1, 6, 0};
    data.resetSpells();
    data.spellDeclared(17, 4, "Test Sign \"Card\"");
    data.spellCaptured(17, 4, 123450);
    data.practiceAt(1, 4, 2) = {7, 98760};
    data.practiceAt(5, 5, 3) = {0, 0};  // not written
    data.stats.countPlay(1, 0);
    data.stats.countPlay(3, 5);
    data.stats.countContinue(3);
    data.stats.countClear(1);
    data.stats.play.add(3723004);  // 1:02:03.004
    data.stats.running.add(7200000);
    std::array<ScoreList, kDifficulties * kShots> out;
    ScoreFile::defaultLists(out);
    for (int i = 0; i < 12; ++i) ScoreFile::insert(out[ScoreFile::listIndex(1, 2)], entry(200000 + 1000 * static_cast<std::uint32_t>(i), "NAME", 99, 1));
    ScoreFile::insert(out[ScoreFile::listIndex(5, 0)], entry(55555, "PH", 8));
    ScoreFile::WriteInput in;
    in.lists = &out;
    in.data = &data;
    in.lastName = {'R', 'E', 'I', 'M', 'U', ' ', ' ', ' ', 0, 0, 0, 0};
    in.exe = {650752, 0x12345678};
    ZunRng rng(0x1234);
    ZunRng expect(0x1234);
    const std::uint8_t key = static_cast<std::uint8_t>(expect.u16() % 256);
    expect.u16();
    const Bytes file = ScoreFile::encode(in, rng);
    CHECK(rng.generations() == 2 && rng.currentSeed() == expect.currentSeed());  // key seed, then filler
    CHECK(file.size() > 0x1C && file[1] == key);
    // The plain header and checksum.
    Bytes plain = file;
    CHECK(ScoreFile::deobfuscate(plain));
    CHECK(plain[4] == 11 && plain[5] == 0 && plain[8] == 0x1C && plain[9] == 0);
    std::uint32_t total = plain[0x10] | plain[0x11] << 8 | plain[0x12] << 16;
    std::uint32_t body = plain[0x14] | plain[0x15] << 8 | plain[0x16] << 16;
    std::uint32_t packed = plain[0x18] | plain[0x19] << 8 | plain[0x1A] << 16;
    CHECK(total == body + 0x1C && packed + 0x1C == file.size());
    std::uint16_t sum = 0;
    for (std::size_t i = 4; i < plain.size(); ++i) sum = static_cast<std::uint16_t>(sum + plain[i]);
    CHECK(sum == (plain[2] | plain[3] << 8));
    // th06's running key: byte 2 is plain[2] XOR (key rotated left by 3).
    CHECK(file[2] == static_cast<std::uint8_t>(plain[2] ^ static_cast<std::uint8_t>((key << 3) | (key >> 5))));
    Bytes again = plain;
    ScoreFile::obfuscate(again);
    CHECK(again == file);

    const std::string ver = "0100b 650752 305419896\r\n";
    const auto img = ScoreFile::decode(file, std::span(reinterpret_cast<const std::uint8_t*>(ver.data()), ver.size()));
    CHECK(img.has_value());
    if (img) {
        std::array<ScoreList, kDifficulties * kShots> back;
        ScoreFile::defaultLists(back);
        ScoreFile::readHighScores(*img, back);
        const ScoreList& b = back[ScoreFile::listIndex(1, 2)];
        // Only the first 10 nodes are written (placeholders count, none of them here).
        CHECK(b.size() == 20 && b[0].score == 211000 && b[9].score == 202000 && !b[0].placeholder && b[10].placeholder);
        CHECK(b[0].stage == 99 && b[0].continues == 1 && std::string(b[0].date.data()) == "11/08" && b[0].slow == 1.5f);
        CHECK(std::string(b[0].name.data()) == "NAME    ");
        const ScoreList& p = back[ScoreFile::listIndex(5, 0)];
        CHECK(p.size() == 11 && p[0].placeholder && p[5].score == 55555 && p[5].stage == 8);
        CHECK(ScoreFile::hiScore(&*img, 1, 2) == std::make_pair(211000u, 1));
        CHECK(ScoreFile::hiScore(&*img, 5, 0) == std::make_pair(100000u, 0));  // raised to 100000
        CHECK(ScoreFile::hiScore(&*img, 0, 0) == std::make_pair(100000u, 0));  // no entries
        CHECK(ScoreFile::hiScore(nullptr, 0, 0) == std::make_pair(100000u, 0));
        ScoreData r;
        ScoreFile::readClears(&*img, r.clears);
        CHECK(r.clears[2].noContinue == data.clears[2].noContinue && r.clears[2].reached == data.clears[2].reached);
        CHECK(r.clears[0].noContinue[0] == 1 && r.clears[0].noContinue[5] == 0);
        ScoreFile::readSpells(*img, r.spells);
        const SpellRecord& s = r.spells[17];
        CHECK(s.present && s.attempts[4] == 1 && s.attempts[6] == 1 && s.captures[4] == 1 && s.captures[6] == 1 && s.bestBonus[4] == 123450);
        CHECK(std::string(reinterpret_cast<const char*>(s.name.data())) == "Test Sign \"Card\"" && s.sum == ScoreData::spellSum(s));
        CHECK(r.spells[3].present && r.spells[3].attempts[6] == 0);
        ScoreFile::readPractice(&*img, r);
        CHECK(r.practiceAt(1, 4, 2).score == 98760 && r.practiceAt(1, 4, 2).plays == 7 && r.practiceAt(0, 0, 0).score == 0);
        PlayStats st;
        CHECK(ScoreFile::readPlayStats(*img, st));
        CHECK(st.play.hours == 1 && st.play.minutes == 2 && st.play.seconds == 3 && st.play.milliseconds == 4 && st.running.hours == 2);
        CHECK(st.rows[1].plays == 1 && st.rows[3].plays == 1 && st.rows[6].plays == 2 && st.rows[3].playsPerShot[5] == 1 &&
              st.rows[6].continues == 1 && st.rows[1].clears == 1 && st.rows[6].clears == 0 && st.clearTotal() == 1);
        std::array<char, 12> name{};
        CHECK(ScoreFile::readLastName(&*img, name) && std::string(name.data()) == "REIMU   ");
    }
    // The VRSM stamp is checked against th07_0100b.ver: another size or checksum discards the
    // file; no .ver data accepts it.
    const std::string otherVer = "0100b 650752 1\r\n";
    CHECK(!ScoreFile::decode(file, std::span(reinterpret_cast<const std::uint8_t*>(otherVer.data()), otherVer.size())));
    CHECK(ScoreFile::decode(file, {}).has_value());
    // A damaged byte fails the checksum; a short file is ignored.
    Bytes bad = file;
    bad[bad.size() / 2] ^= 0x40;
    CHECK(!ScoreFile::decode(bad, {}));
    CHECK(!ScoreFile::decode(Bytes(0x1B, 0), {}));
    // A wrong version is ignored.
    Bytes v10 = plain;
    v10[4] = 10;
    ScoreFile::obfuscate(v10);
    CHECK(!ScoreFile::decode(v10, {}));

    // Hand-made images: walks that stop, zero-length records, the last TH7K.
    auto image = [](const std::vector<Bytes>& records) {
        Bytes bodyBytes;
        for (const Bytes& r : records) bodyBytes.insert(bodyBytes.end(), r.begin(), r.end());
        Bytes f(0x1C, 0);
        set32(f, 0x08, 0x1C);
        f[4] = 11;
        set32(f, 0x10, static_cast<std::uint32_t>(0x1C + bodyBytes.size()));
        set32(f, 0x14, static_cast<std::uint32_t>(bodyBytes.size()));
        const Bytes p = lzss::compress(bodyBytes);
        set32(f, 0x18, static_cast<std::uint32_t>(p.size()));
        f.insert(f.end(), p.begin(), p.end());
        f[1] = 0x5A;
        ScoreFile::obfuscate(f);
        return f;
    };
    auto rec = [](const char* tag, std::uint16_t len, std::uint8_t version = 1) {
        Bytes r(len, 0);
        std::memcpy(r.data(), tag, 4);
        r[4] = static_cast<std::uint8_t>(len), r[5] = static_cast<std::uint8_t>(len >> 8);
        r[6] = r[4], r[7] = r[5];
        r[8] = version;
        return r;
    };
    auto clrd = [&](int shot, std::uint8_t v) {
        Bytes r = rec("CLRD", 0x1C);
        std::fill(r.begin() + 0x0C, r.begin() + 0x18, v);
        r[0x18] = static_cast<std::uint8_t>(shot);
        return r;
    };
    {
        const auto im = ScoreFile::decode(image({rec("TH7K", 12), clrd(0, 7), clrd(9, 5), clrd(1, 8)}), {});
        CHECK(im.has_value());
        std::array<ClearRecord, kShots> c;
        if (im) ScoreFile::readClears(&*im, c);
        CHECK(c[0].reached[0] == 7 && c[1].reached[0] == 1);  // shot 9 stopped the walk
        Bytes lsnm1 = rec("LSNM", 0x18), lsnm2 = rec("LSNM", 0x18);
        std::memcpy(lsnm1.data() + 0x0C, "FIRST", 6);
        std::memcpy(lsnm2.data() + 0x0C, "SECOND", 7);
        Bytes pl1 = rec("PLST", 0x160), pl2 = rec("PLST", 0x160);
        pl1[0x0C] = 1;
        pl2[0x0C] = 2;
        const auto im2 = ScoreFile::decode(image({rec("TH7K", 12), lsnm1, pl1, lsnm2, pl2}), {});
        std::array<char, 12> n{};
        PlayStats st;
        CHECK(im2 && ScoreFile::readLastName(&*im2, n) && std::string(n.data()) == "FIRST");
        CHECK(im2 && ScoreFile::readPlayStats(*im2, st) && st.running.hours == 2);
        CHECK(!ScoreFile::decode(image({clrd(0, 7)}), {}));                            // no TH7K
        CHECK(!ScoreFile::decode(image({rec("TH7K", 12), rec("TH7K", 12, 2)}), {}));  // the last one is version 2
        Bytes zero = rec("XXXX", 12);
        zero[4] = zero[5] = 0;
        CHECK(!ScoreFile::decode(image({rec("TH7K", 12), zero}), {}));  // a zero-length record
        CHECK(ScoreFile::decode(image({rec("TH7K", 12), rec("ABCD", 16)}), {}).has_value());  // unknown tags skipped
        // CATK: a card number of 141 or more stops the walk; PSCR stops at a bad cell.
        Bytes cat1 = rec("CATK", 0x78), cat2 = rec("CATK", 0x78), cat3 = rec("CATK", 0x78);
        cat1[0x28] = 5, cat1[0x5C + 12] = 3;
        cat2[0x28] = 141;
        cat3[0x28] = 6;
        Bytes ps1 = rec("PSCR", 0x18), ps2 = rec("PSCR", 0x18), ps3 = rec("PSCR", 0x18);
        ps1[0x10] = 50, ps1[0x14] = 2, ps1[0x15] = 1, ps1[0x16] = 3;
        ps2[0x10] = 60, ps2[0x14] = 1, ps2[0x15] = 5;
        ps3[0x10] = 70, ps3[0x14] = 0;
        const auto im3 = ScoreFile::decode(image({rec("TH7K", 12), cat1, cat2, cat3, ps1, ps2, ps3}), {});
        ScoreData sd;
        if (im3) {
            ScoreFile::readSpells(*im3, sd.spells);
            ScoreFile::readPractice(&*im3, sd);
        }
        CHECK(sd.spells[5].present && sd.spells[5].attempts[6] == 3 && !sd.spells[6].present);
        CHECK(sd.practiceAt(2, 3, 1).score == 50 && sd.practiceAt(0, 0, 0).score == 0);
    }
}

static void testUnlockRules() {
    ScoreData sd;
    CHECK(!sd.extraUnlocked() && !sd.phantasmUnlocked());
    CHECK(sd.practiceStages(0, 0) == 1 && sd.practiceStages(0, 5) == 0);
    // Stage entered: no-continue only without continues; reached always (s - 1).
    sd.stageEntered(3, 2, 4, false);
    CHECK(sd.clears[3].noContinue[2] == 3 && sd.clears[3].reached[2] == 3 && sd.practiceStages(3, 2) == 3);
    sd.stageEntered(3, 2, 6, true);
    CHECK(sd.clears[3].noContinue[2] == 3 && sd.clears[3].reached[2] == 5);
    sd.stageEntered(3, 2, 2, false);  // never lowered
    CHECK(sd.clears[3].reached[2] == 5);
    // The ending: seen before? and the records at 99.
    CHECK(!sd.endingReached(3, 2, true) && sd.clears[3].reached[2] == 99 && sd.clears[3].noContinue[2] == 3);
    CHECK(sd.practiceStages(3, 2) == 6);
    CHECK(sd.endingReached(3, 2, true));
    CHECK(!sd.extraUnlocked());
    CHECK(!sd.endingReached(3, 0, false) && sd.clears[3].noContinue[0] == 99);
    CHECK(sd.extraUnlocked() && sd.extraShotUnlocked(3) && !sd.extraShotUnlocked(2));
    CHECK(sd.endingReached(3, 0, false));
    // Phantasm: the Extra clear plus 60 cards captured at least once in total.
    sd.clears[3].noContinue[Extra] = 99;
    sd.resetSpells();
    for (int c = 0; c < 59; ++c) sd.spells[static_cast<std::size_t>(c)].captures[6] = 1;
    sd.phantasmCheck();
    CHECK(!sd.phantasmUnlocked());
    sd.spells[100].captures[6] = 2;
    sd.phantasmCheck();
    CHECK(sd.spellsCapturedOnce() == 60 && sd.phantasmUnlocked() && sd.phantasmShotUnlocked(3) && !sd.phantasmShotUnlocked(0));

    // Spell history: attempts and captures for the shot and the total, best bonus, the sum;
    // a different name wipes the card.
    ScoreData h;
    h.resetSpells();
    h.spellDeclared(9, 1, "Card A");
    h.spellDeclared(9, 2, "Card A");
    h.spellCaptured(9, 2, 5000);
    h.spellCaptured(9, 2, 3000);
    const SpellRecord& s = h.spells[9];
    CHECK(s.attempts[1] == 1 && s.attempts[2] == 1 && s.attempts[6] == 2 && s.captures[2] == 2 && s.captures[6] == 2);
    CHECK(s.bestBonus[2] == 5000 && s.bestBonus[6] == 5000 && s.sum == ScoreData::spellSum(s));
    h.spellDeclared(9, 1, "Card B");  // a translated name
    CHECK(s.attempts[6] == 1 && s.captures[6] == 0 && s.bestBonus[6] == 0 && s.attempts[1] == 1);
    h.spells[10].attempts = {9999, 0, 0, 0, 0, 0, 9999};
    h.spells[10].sum = ScoreData::spellSum(h.spells[10]);
    h.spellDeclared(10, 0, "");
    CHECK(h.spells[10].attempts[0] == 9999 && h.spells[10].attempts[6] == 9999);
    // Play statistics stop at 999999; the clock carries.
    PlayStats st;
    st.rows[2].retries = 999999;
    st.countRetry(2);
    CHECK(st.rows[2].retries == 999999 && st.rows[6].retries == 1);
    PlayTime t;
    t.add(59999);
    t.add(1);
    t.add(3600000ull * 3);
    CHECK(t.hours == 3 && t.minutes == 1 && t.seconds == 0 && t.milliseconds == 0);
}

static void testMenuInput() {
    MenuInput in;
    in.sample(BtnShoot);
    CHECK(in.pressed(BtnShoot) && in.held(BtnShoot) && !in.pressed(BtnBomb));
    in.sample(BtnShoot | BtnEnter);
    // A mask counts as pressed when its masked value changed and is not zero.
    CHECK(!in.pressed(BtnShoot) && in.pressed(BtnEnter) && in.pressed(BtnShoot | BtnEnter));
    // Releasing one of the pair while holding the other also counts as a press [QUIRK].
    in.sample(BtnEnter);
    CHECK(in.pressed(static_cast<std::uint16_t>(BtnShoot | BtnEnter)) && !in.pressed(BtnEnter));
    // Auto-repeat: the whole word unchanged; the first repeat 33 passes after the change, then
    // every 8.
    MenuInput r;
    r.sample(BtnDown);  // the change
    std::vector<int> repeats;
    for (int pass = 1; pass <= 60; ++pass) {
        r.sample(BtnDown);
        if (r.repeating()) repeats.push_back(pass);
        CHECK(r.pressedOrRepeat(BtnDown) == r.repeating());
    }
    CHECK(repeats.size() == 4 && repeats[0] == 33 && repeats[1] == 41 && repeats[2] == 49 && repeats[3] == 57);
    r.sample(BtnDown | BtnFocus);  // any change restarts the count
    CHECK(!r.repeating() && !r.pressed(BtnDown));
    r.clear();
    CHECK(r.current() == 0 && r.previous() == 0 && !r.repeating());
    r.sample(BtnDown);  // still held after a transition: freshly pressed
    CHECK(r.pressed(BtnDown));
}

static void testMusicComments() {
    const std::string t =
        "header text, no track\r\n"
        "@bgm/th07_01.mid\r\n"
        "No.1 Title One\r\n"
        "line a\r\n"
        "\r\n"
        "line b\r\n"
        "@bgm/th07_02.mid\r\n\r\n"
        "No.2 Title Two\r\n"
        "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n";
    const auto tracks = parseMusicComments(text(t));
    CHECK(tracks.size() == 2);
    if (tracks.size() == 2) {
        CHECK(tracks[0].path == "bgm/th07_01.mid" && tracks[0].title == "No.1 Title One");
        CHECK(tracks[0].lines[0].empty() && tracks[0].lines[1] == "line a" && tracks[0].lines[2] == "line b" && tracks[0].lines[3].empty());
        CHECK(tracks[1].path == "bgm/th07_02.mid" && tracks[1].title == "No.2 Title Two");
        CHECK(tracks[1].lines[0].empty() && tracks[1].lines[1] == "1" && tracks[1].lines[7] == "7");
    }
}

namespace {

// A synthetic archive for the flow screens: menu ANM files with a few scripts each (set a
// sprite, wait for interrupts), musiccmt.txt and a short ending.
struct FlowData {
    Vfs vfs;
    TextureLibrary textures;
    AnmLibrary anm;
    ZunRng rng{1};
    std::string dir;
    std::uint64_t clock = 1000;
};

Bytes menuAnm(int scripts, int sprites, int format = 1) {
    EntryDef e;
    e.width = 64;
    e.height = 64;
    e.format = format;
    for (int i = 0; i < sprites; ++i) e.sprites.push_back({i, 0, static_cast<float>(i % 8) * 8.0f, 64, 8});
    for (int i = 0; i < scripts; ++i) e.scripts.push_back({i, {{3, 0, {static_cast<std::uint32_t>(i % sprites)}}, {20, 0, {}}}});
    return anmFile({e});
}

Bytes endScript() {
    Bytes b;
    auto add = [&](const std::string& s) { b.insert(b.end(), s.begin(), s.end()); };
    add("@s");
    add(std::string("3\0", 2));
    add(std::string("0\0", 2));
    add("\r\n@c");
    add(std::string("255\0", 4));
    add("\r\nLINE ONE\r\n@w");
    add(std::string("5\0", 2));
    add(std::string("0\0", 2));
    add("\r\nLINE TWO\r\n@V");
    add(std::string("-10\0", 4));
    add(std::string("2\0", 2));
    add("\r\n@z\r\n");
    return b;
}

void mountFlowData(FlowData& f, std::vector<Member> extra = {}, const char* dirTag = "flow") {
    std::string err;
    const std::string music = "@bgm/th07_01.mid\r\nTrack 1\r\ncomment\r\n@bgm/th07_02.mid\r\nTrack 2\r\nmore\r\n";
    std::vector<Member> members = {{"text.anm", menuAnm(30, 40, 0)},
                                   {"ascii.anm", menuAnm(8, 130)},
                                   {"capture.anm", menuAnm(2, 2)},
                                   {"title01.anm", menuAnm(164, 40)},
                                   {"result00.anm", menuAnm(41, 20)},
                                   {"music00.anm", menuAnm(32, 32, 0)},
                                   {"musiccmt.txt", text(music)},
                                   {"end00.end", endScript()}};
    for (Member& m : extra) members.push_back(std::move(m));
    f.vfs.mount(openArchive("th07.dat", mem(pbg4(members)), &err));
    f.dir = (std::filesystem::temp_directory_path() / (std::string("th07_") + dirTag + "_test_" + std::to_string(::getpid()))).string() + "/";
    std::filesystem::remove_all(f.dir);
    std::filesystem::create_directories(f.dir);
}

Supervisor::Boot flowBoot(FlowData& f) {
    Supervisor::Boot b;
    b.vfs = &f.vfs;
    b.textures = &f.textures;
    b.anm = &f.anm;
    b.rng = &f.rng;
    b.storageDir = f.dir;
    b.canPlay = false;  // no th07.exe tables: games cannot start
    b.clockMs = [&f]() { return f.clock += 16; };
    return b;
}

void press(Supervisor& s, std::uint16_t b, int after = 1) {
    s.pass(b);
    for (int i = 0; i < after; ++i) s.pass(0);
}
void idle(Supervisor& s, int passes) {
    for (int i = 0; i < passes; ++i) s.pass(0);
}

}  // namespace

static void testSupervisor() {
    FlowData f;
    mountFlowData(f);
    {
        Supervisor s;
        CHECK(s.start(flowBoot(f)));
        CHECK(s.active() == FlowState::Init && s.requested() == FlowState::MainMenu);
        CHECK(std::filesystem::exists(f.dir + "th07.cfg"));  // written back at boot
        s.pass(0);
        CHECK(s.active() == FlowState::MainMenu && s.previous() == FlowState::Init && s.mainMenu());
        CHECK(s.mainMenu() && s.mainMenu()->cursor() == 0 && s.context().maxContinues == 3);
        // Select and Return are ignored for 10 passes.
        press(s, BtnBomb);
        CHECK(s.mainMenu()->cursor() == 0);
        idle(s, 10);
        // Extra Start is locked: Down steps over it.
        press(s, BtnDown);
        CHECK(s.mainMenu()->cursor() == 2);
        press(s, BtnUp);
        CHECK(s.mainMenu()->cursor() == 0);
        press(s, BtnBomb);
        CHECK(s.mainMenu()->cursor() == 7);  // Return moves to Quit

        // The Result viewer and back: removing it writes score.dat (two RNG draws).
        press(s, BtnUp);
        press(s, BtnUp);
        press(s, BtnUp);
        CHECK(s.mainMenu()->cursor() == 4);
        const std::uint32_t writes = s.writes();
        const std::uint32_t draws = f.rng.generations();
        s.pass(BtnShoot);
        CHECK(!s.mainMenu() && s.requested() == FlowState::ResultViewer);
        s.pass(BtnShoot);  // the transition pass: nothing is held for the screen
        CHECK(s.active() == FlowState::ResultViewer && s.resultScreen() && s.resultScreen()->mode() == ResultScreen::Mode::Viewer);
        CHECK(s.resultScreen()->state() == ResultScreen::State::Menu && s.resultScreen()->menuCursor() == 1);
        idle(s, 21);
        press(s, BtnUp);
        press(s, BtnUp);
        CHECK(s.resultScreen()->menuCursor() == 8);  // Phantasm's row is skipped... (row 5 not crossed here)
        press(s, BtnShoot);
        CHECK(s.resultScreen() && s.resultScreen()->state() == ResultScreen::State::Exit);
        idle(s, 60);
        CHECK(s.active() == FlowState::MainMenu && s.writes() == writes + 1 && f.rng.generations() == draws + 2);
        CHECK(std::filesystem::exists(f.dir + "score.dat") && s.readScoreFile().has_value());
        CHECK(s.mainMenu() && s.mainMenu()->cursor() == 4 && s.previous() == FlowState::ResultViewer);

        // The Music Room: two tracks; Return goes back with the cursor on Music Room.
        idle(s, 10);
        press(s, BtnDown);
        press(s, BtnShoot);
        CHECK(s.requested() == FlowState::MusicRoom);
        s.pass(0);
        CHECK(s.active() == FlowState::MusicRoom && s.musicRoom() && s.musicRoom()->tracks().size() == 2);
        idle(s, 8);
        press(s, BtnDown);
        CHECK(s.musicRoom()->cursor() == 1);
        press(s, BtnDown);
        CHECK(s.musicRoom()->cursor() == 0 && s.musicRoom()->top() == 0);
        press(s, BtnUp);
        CHECK(s.musicRoom()->cursor() == 1);
        press(s, BtnShoot);
        CHECK(s.musicRoom()->selected() == 1);
        press(s, BtnBomb);
        CHECK(s.active() == FlowState::MainMenu && s.mainMenu() && s.mainMenu()->cursor() == 5);

        // Option: a 30-pass wait, then Right raises the lives; Return twice leaves; th07.cfg.
        idle(s, 10);
        press(s, BtnDown);
        press(s, BtnShoot);
        CHECK(s.mainMenu()->state() == MainMenu::State::OptionWait);
        idle(s, 32);
        CHECK(s.mainMenu()->state() == MainMenu::State::Option);
        idle(s, 4);
        press(s, BtnRight);
        CHECK(s.config().lives == 3);
        press(s, BtnBomb);
        press(s, BtnBomb);
        CHECK(s.mainMenu()->state() == MainMenu::State::Main && s.mainMenu()->cursor() == 6);
        {
            bool exists = false;
            std::ifstream cfg(f.dir + "th07.cfg", std::ios::binary);
            Bytes bytes((std::istreambuf_iterator<char>(cfg)), std::istreambuf_iterator<char>());
            exists = bytes.size() == 0x38;
            CHECK(exists && bytes[0x1C] == 3);
        }

        // Start: difficulty, character and shot select, then the game (which cannot start
        // without th07.exe here: back to the title).
        press(s, BtnUp);
        press(s, BtnUp);
        press(s, BtnUp);
        press(s, BtnUp);
        press(s, BtnUp);
        CHECK(s.mainMenu()->cursor() == 0);
        idle(s, 10);
        press(s, BtnShoot);
        CHECK(s.mainMenu()->state() == MainMenu::State::Difficulty);
        idle(s, 31);
        press(s, BtnDown);
        CHECK(s.mainMenu()->cursor() == 2);
        press(s, BtnShoot);
        CHECK(s.config().difficulty == 2 && s.mainMenu()->state() == MainMenu::State::Character);
        idle(s, 31);
        press(s, BtnRight);
        press(s, BtnShoot);
        CHECK(s.context().character == 1 && s.mainMenu()->state() == MainMenu::State::Shot);
        idle(s, 31);
        press(s, BtnDown);
        s.pass(BtnShoot);
        CHECK(s.context().shotType == 1 && s.context().shotIndex() == 3 && s.context().stage == 0 && s.requested() == FlowState::Game);
        s.pass(0);
        CHECK(s.active() == FlowState::MainMenu && s.mainMenu() && !s.game());

        // Quit: interrupt 1, then Exit after 60 passes.
        idle(s, 10);
        press(s, BtnBomb);
        press(s, BtnShoot);
        CHECK(s.mainMenu()->state() == MainMenu::State::Quit && !s.quit());
        idle(s, 62);
        CHECK(s.quit());
        s.shutdown();
    }
    {
        // The ending, then the result screen after a game: name entry for a top-10 score.
        Supervisor s;
        CHECK(s.start(flowBoot(f)));
        s.pass(0);
        CHECK(s.config().lives == 3);  // th07.cfg from the first session
        s.startEnding(0, Normal, false, 5000000);
        CHECK(s.active() == FlowState::Ending && s.ending() && s.ending()->script() == "data/end00.end");
        CHECK(s.scores().clears[0].noContinue[Normal] == 99 && s.scores().clears[0].reached[Normal] == 99 && !s.ending()->seen());
        s.pass(0);
        CHECK(s.ending()->lastLine() == "LINE ONE" && s.ending()->colorRef() == 255 && s.ending()->lineSlot() == 1);
        idle(s, 3);  // the line delay (3)
        CHECK(s.ending()->lastLine() == "LINE ONE");
        idle(s, 2);
        CHECK(s.ending()->lastLine() == "LINE TWO" || s.ending()->lineSlot() >= 1);
        idle(s, 20);
        CHECK(s.active() == FlowState::ResultAfterGame && s.resultScreen() && s.resultScreen()->mode() == ResultScreen::Mode::AfterGame);
        s.pass(0);
        CHECK(s.resultScreen()->state() == ResultScreen::State::NameEntry && s.resultScreen()->rank() == 0);
        // A saved LSNM exists: the key cursor starts on END.
        CHECK(s.resultScreen()->keyCursor() == ResultScreen::kKeyEnd);
        idle(s, 30);
        press(s, BtnDown);  // END + 16 wraps to 'P'
        CHECK(s.resultScreen()->keyCursor() == 15);
        press(s, BtnShoot);
        press(s, BtnLeft);
        press(s, BtnShoot);
        CHECK(s.resultScreen()->nameCursor() == 2);
        press(s, BtnBomb);  // backspace
        CHECK(s.resultScreen()->nameCursor() == 1);
        press(s, BtnShoot);
        press(s, BtnRight);
        press(s, BtnRight);
        press(s, BtnRight);  // row end wraps to its start
        CHECK(s.resultScreen()->keyCursor() == 1);
        press(s, BtnShoot);
        CHECK(std::string(s.resultScreen()->list(Normal, 0)[0].name.data(), 3) == "POB");
        // Esc backspaces, then finishes in the same pass [QUIRK].
        press(s, BtnMenu);
        CHECK(s.resultScreen()->state() == ResultScreen::State::Statistics && std::string(s.resultScreen()->lastName().data(), 3) == "PO ");
        idle(s, 90);
        press(s, BtnShoot);
        idle(s, 32);
        CHECK(s.resultScreen() && s.resultScreen()->state() == ResultScreen::State::Leave);
        idle(s, 61);
        CHECK(s.active() == FlowState::MainMenu);
        const auto img = s.readScoreFile();
        CHECK(img.has_value());
        if (img) {
            std::array<char, 12> name{};
            CHECK(ScoreFile::readLastName(&*img, name) && std::string(name.data()) == "PO      ");
            CHECK(ScoreFile::hiScore(&*img, Normal, 0).first == 5000000);
            std::array<ClearRecord, kShots> c;
            ScoreFile::readClears(&*img, c);
            CHECK(c[0].noContinue[Normal] == 99);  // the ending's records were saved
        }
        // After the result screen the title's cursor is on Start (not Extra/Phantasm).
        CHECK(s.mainMenu() && s.mainMenu()->cursor() == 0 && s.previous() == FlowState::ResultAfterGame);
        // The running time was counted.
        CHECK(s.scores().stats.running.totalSeconds() > 0 || s.scores().stats.running.milliseconds > 0);
        s.shutdown();
    }
    {
        // The Result viewer's hidden code on the Hard page, Focus held: Up x3, D x2, Down x2,
        // Q x3 sets every clear record of every shot and difficulty to 99 (saved on exit).
        Supervisor s;
        CHECK(s.start(flowBoot(f)));
        s.pass(0);
        idle(s, 10);
        for (int i = 0; i < 8 && s.mainMenu() && s.mainMenu()->cursor() != 4; ++i) press(s, BtnDown);
        CHECK(s.mainMenu() && s.mainMenu()->cursor() == 4);
        press(s, BtnShoot);
        idle(s, 21);
        press(s, BtnDown);
        CHECK(s.resultScreen() && s.resultScreen()->menuCursor() == 2);
        press(s, BtnShoot);
        CHECK(s.resultScreen()->state() == ResultScreen::State::ScorePage);
        idle(s, 31);
        const std::uint16_t seq[10] = {BtnUp, BtnUp, BtnUp, BtnD, BtnD, BtnDown, BtnDown, BtnQ, BtnQ, BtnQ};
        // A wrong key (Shoot) resets the sequence.
        s.pass(BtnFocus | BtnUp);
        s.pass(BtnFocus);
        s.pass(BtnFocus | BtnShoot);
        s.pass(BtnFocus);
        for (int i = 0; i < 9; ++i) {
            s.pass(static_cast<std::uint16_t>(BtnFocus | seq[i]));
            s.pass(BtnFocus);
        }
        CHECK(s.scores().clears[5].noContinue[3] != 99);
        s.pass(static_cast<std::uint16_t>(BtnFocus | seq[9]));
        s.pass(BtnFocus);
        bool all = true;
        for (const ClearRecord& c : s.scores().clears) {
            for (int d = 0; d < kDifficulties; ++d) all = all && c.noContinue[static_cast<std::size_t>(d)] == 99 && c.reached[static_cast<std::size_t>(d)] == 99;
        }
        CHECK(all && s.scores().phantasmUnlocked());
        press(s, BtnBomb);  // back to the menu
        CHECK(s.resultScreen()->state() == ResultScreen::State::Menu && s.resultScreen()->menuCursor() == 2);
        // With Phantasm unlocked, row 5 is selectable.
        idle(s, 21);
        press(s, BtnDown);
        press(s, BtnDown);
        press(s, BtnDown);
        CHECK(s.resultScreen()->menuCursor() == 5);
        press(s, BtnBomb);
        press(s, BtnShoot);  // Exit
        idle(s, 62);
        CHECK(s.active() == FlowState::MainMenu);
        const auto img = s.readScoreFile();
        std::array<ClearRecord, kShots> c;
        ScoreFile::readClears(img ? &*img : nullptr, c);
        CHECK(c[4].noContinue[Phantasm] == 99 && c[1].reached[Hard] == 99);
        s.shutdown();
    }
    std::filesystem::remove_all(f.dir);
}

// ---- replays ---------------------------------------------------------------------------------

namespace {

std::uint32_t le32(const Bytes& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}
std::uint16_t le16(const Bytes& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }

const ExeIdentity kTestExe{650752, 1234567};
const Bytes kTestVer = text("0009_ 466944 99\r\n0100b 650752 1234567\r\n");

// A recording as the game makes it: stage index `first` with `frames1` frames, then stage
// index first + 1 with `frames2`, closed at the teardown.
ReplayRecorder testRecording(const GameConfig& cfg, int first = 0, int frames1 = 61, int frames2 = 30) {
    ReplayRecorder rec;
    rec.create(3, Hard, cfg, kTestExe);
    ReplayRecorder::StageStart s1;
    s1.seed = 0x1234;
    s1.lives = 3, s1.bombs = 3, s1.rank = 16, s1.cherryMax = 250000, s1.nextExtend = 50;
    rec.startBlock(first, s1);
    for (int i = 0; i < frames1; ++i) rec.write(static_cast<std::uint16_t>(i), i == 5 ? kReplayEventPaused : 0);
    if (frames2 > 0) {
        rec.closeBlock();
        ReplayRecorder::StageStart s2 = s1;
        s2.seed = 0xBEEF, s2.score = 123456, s2.power = 64, s2.cherryPlus = 49999, s2.spells = 2, s2.totalGraze = 77;
        rec.startBlock(first + 1, s2);
        for (int i = 0; i < frames2; ++i) rec.write(0x0011, 0x0040, 45, i == 0);
    }
    rec.closeBlock();  // the teardown's terminator
    return rec;
}

}  // namespace

static void testReplayContainer() {
    GameConfig cfg;
    cfg.lives = 3;
    ReplayRecorder rec = testRecording(cfg);
    CHECK(rec.active() && rec.currentBlock() == 1 && rec.frame() == 30);
    CHECK(std::memcmp(rec.file().summary.bytes.data() + 0x0A, "NO N", 4) == 0);  // while recording [QUIRK]

    // The save's three draws: the key (64..191), the summary's filler, the header's filler.
    ZunRng rng(0x4242), expect(0x4242);
    const auto key = static_cast<std::uint8_t>(expect.u16InRange(128) + 64);
    const auto summaryFiller = static_cast<std::uint8_t>(expect.u16InRange(256));
    const auto headerFiller = static_cast<std::uint8_t>(expect.u16InRange(256));
    const std::uint32_t gen = rng.generations();
    const Bytes file = rec.save(777777, 777770, 0.0f, "ABCDEFGH", "11/09", rng);
    CHECK(rng.generations() == gen + 3 && rng.currentSeed() == expect.currentSeed());
    CHECK(file.size() > 0x54 && std::memcmp(file.data(), "T7RP", 4) == 0 && le16(file, 4) == 0x1100 && le16(file, 6) == 0);
    CHECK(file[0x0D] == key && file[0x0C] == headerFiller && key >= 64 && key <= 191 && le16(file, 0x0E) == 0);

    // Obfuscation from 0x10 with a running key (+7 per byte); the checksum covers the plain
    // bytes from 0x0D (the header filler is outside it).
    Bytes plain = file;
    ReplayFile::deobfuscate(plain);
    CHECK(file[0x10] == static_cast<std::uint8_t>(plain[0x10] + key) && file[0x11] == static_cast<std::uint8_t>(plain[0x11] + key + 7));
    std::uint32_t sum = 0x3F000318u;
    for (std::size_t i = 0x0D; i < plain.size(); ++i) sum += plain[i];
    CHECK(le32(plain, 8) == sum && ReplayFile::checksum(plain) == sum);
    {
        Bytes p2 = plain;
        p2[0x0C] ^= 0xFF;
        CHECK(ReplayFile::checksum(p2) == sum);
        Bytes again = plain;
        ReplayFile::obfuscate(again);
        CHECK(again == file);
    }

    // The image: header, summary, stage blocks from 0xE8 in order, then the FPS blocks.
    const std::uint32_t imageSize = le32(plain, 0x10), packed = le32(plain, 0x14), body = le32(plain, 0x18);
    CHECK(imageSize == 0x54 + body && packed == plain.size() - 0x54);
    Bytes image(imageSize, 0);
    std::memcpy(image.data(), plain.data(), 0x54);
    CHECK(lzss::decompress(std::span(plain).subspan(0x54), std::span(image).subspan(0x54)) == body);
    const std::uint32_t o1 = le32(image, 0x1C), o2 = le32(image, 0x20), f1 = le32(image, 0x38), f2 = le32(image, 0x3C);
    CHECK(o1 == 0xE8 && le32(image, 0x24) == 0 && le32(image, 0x34) == 0);
    CHECK(o2 == o1 + 0x2C + 4 * (61 + 2));  // entry 0, 61 frames, one terminator
    CHECK(f1 == o2 + 0x2C + 4 * (30 + 3));  // the last block has two terminators [QUIRK]
    CHECK(f2 == f1 + 4);                    // samples at frames 0, 30 and 60, plus one byte
    CHECK(imageSize == f2 + 2);
    CHECK(image[f1] == 60 && image[f1 + 1] == 60 && image[f1 + 2] == 60 && image[f1 + 3] == 60);
    CHECK(image[f2] == (45 | 0x80) && image[f2 + 1] == 45);  // the timing flag on the sample only
    CHECK(le32(image, o1) == 123456 && le32(image, o2) == 777777);  // end scores
    CHECK(le16(image, o1 + 0x20) == 0x1234 && le16(image, o2 + 0x20) == 0xBEEF && image[o2 + 0x22] == 64 && image[o2 + 0x27] == 2);
    CHECK(le32(image, o2 + 0x10) == 49999 && le32(image, o2 + 0x14) == 77 && le32(image, o1 + 0x0C) == 250000);
    CHECK(le16(image, o1 + 0x2C) == 0 && le16(image, o1 + 0x2C + 4 * 6) == 5 && le16(image, o1 + 0x2C + 4 * 6 + 2) == kReplayEventPaused);
    CHECK(le16(image, o1 + 0x2C + 4 * 62) == 0 && le16(image, o2 + 0x2C + 4 * 1) == 0x0011 && le16(image, o2 + 0x2C + 4 * 1 + 2) == 0x0040);
    // The summary.
    const std::size_t s = 0x54;
    CHECK(image[s] == summaryFiller && image[s + 1] == 'b' && image[s + 2] == 3 && image[s + 3] == Hard);
    CHECK(std::memcmp(image.data() + s + 4, "11/09\0", 6) == 0 && std::memcmp(image.data() + s + 0x0A, "ABCDEFGH\0", 9) == 0);
    CHECK(le16(image, s + 0x16) == 0x0100 && le32(image, s + 0x18) == 777770);
    {
        const std::vector<std::uint8_t> c = cfg.save();
        CHECK(std::memcmp(image.data() + s + 0x1C, c.data(), 0x38) == 0 && image[s + 0x1C + 0x1C] == 3);
    }
    float f74, f78, f7c;
    std::memcpy(&f74, image.data() + s + 0x74, 4);
    std::memcpy(&f78, image.data() + s + 0x78, 4);
    std::memcpy(&f7c, image.data() + s + 0x7C, 4);
    CHECK(f78 == 0.0f && f74 == 1.12f && f7c == 2.34f && le32(image, s + 0x80) == 30);
    CHECK(le32(image, s + 0x84) == 650752 && le32(image, s + 0x88) == 1234567 && std::memcmp(image.data() + s + 0x8C, "0100b\0", 6) == 0);

    // Loading (spec 7.7).
    std::string err;
    const auto r = ReplayFile::parse(file, kTestVer, &err);
    CHECK(r.has_value());
    if (r) {
        CHECK(r->summary.shot() == 3 && r->summary.difficulty() == Hard && r->summary.name() == "ABCDEFGH" && r->summary.date() == "11/09");
        CHECK(r->summary.score() == 777770 && r->summary.lifeCount() == 3 && !r->summary.slowModeRecorded() && r->summary.versionTag() == "0100b");
        CHECK(r->stageMask() == 0x03 && r->firstStage() == 0 && r->key == key && r->headerFiller == headerFiller);
        CHECK(r->stages[0].entries.size() == 63 && r->stages[1].entries.size() == 33 && r->stages[0].fps.size() == 4 && r->stages[1].fps.size() == 2);
        CHECK(r->stages[0].entries[6] == (ReplayEntry{5, kReplayEventPaused}) && r->stages[0].endScore == 123456 && r->stages[1].seed == 0xBEEF);
        CHECK(r->stages[1].cherryPlus == 49999 && r->stages[1].spells == 2 && r->stages[0].cherryMax == 250000 && r->stages[0].lives == 3);
        // A round trip through the writer gives the same contents.
        ReplayFile copy = *r;
        ZunRng rng2(9);
        const auto r2 = ReplayFile::parse(copy.serialize(rng2), kTestVer);
        CHECK(r2 && r2->stages[0].entries == r->stages[0].entries && r2->stages[1].entries == r->stages[1].entries);
        CHECK(r2 && r2->stages[1].fps == r->stages[1].fps && r2->summary.name() == "ABCDEFGH" && r2->summary.score() == 777770);
        // The replay list keeps the summary and the block headers only.
        const auto h = ReplayFile::parse(file, kTestVer, nullptr, true);
        CHECK(h && h->stages[1].present && h->stages[1].entries.empty() && h->stages[1].endScore == 777777);
    }
    CHECK(ReplayFile::parse(file, {}).has_value());  // no .ver data: the check passes

    // Validation failures.
    {
        Bytes b = file;
        b[0] = 'X';
        CHECK(!ReplayFile::parse(b, kTestVer, &err) && err == "not a th07 replay");
        b = file;
        b[4] = 0x01;
        CHECK(!ReplayFile::parse(b, kTestVer, &err) && err == "wrong version");
        b = file;
        b[b.size() - 3] ^= 0x10;
        CHECK(!ReplayFile::parse(b, kTestVer, &err) && err == "checksum mismatch");
        b = file;
        b[0x0C] ^= 0xFF;  // the header filler is not covered
        CHECK(ReplayFile::parse(b, kTestVer).has_value());
        CHECK(!ReplayFile::parse(Bytes(file.begin(), file.begin() + 0x30), kTestVer, &err) && err == "too short");
        // A cut body: the checksum is recomputed so the decompression is what fails.
        Bytes cut = plain;
        cut.resize(0x54 + packed / 2);
        set32(cut, 8, ReplayFile::checksum(cut));
        ReplayFile::obfuscate(cut);
        CHECK(!ReplayFile::parse(cut, kTestVer, &err) && err == "body does not decompress");
        // Another executable.
        CHECK(!ReplayFile::parse(file, text("0100b 650752 7654321\r\n"), &err) && err == "recorded by another version");
        CHECK(!ReplayFile::parse(file, text("0100a 650752 1234567\r\n"), &err));
    }
    {
        // Slow mode in the recorded configuration: rejected.
        GameConfig slow;
        slow.slowMode = 1;
        ReplayRecorder sr = testRecording(slow, 0, 10, 0);
        ZunRng r3(1);
        CHECK(!ReplayFile::parse(sr.save(1, 1, 0.0f, "SLOW", "01/01", r3), kTestVer, &err) && err == "recorded in slow mode");
        // A "debug" version string passes the integrity check whatever the .ver data says.
        ReplayRecorder dr = testRecording(cfg, 2, 10, 0);
        dr.file().summary.setExe({1, 2}, "debug");
        const auto d = ReplayFile::parse(dr.save(1, 1, 0.0f, "ZUN", "07/19", r3), kTestVer, &err);
        CHECK(d && d->summary.versionTag() == "debug" && d->firstStage() == 2 && d->stages[2].entries.size() == 13);
    }
    // The drop frees everything.
    rec.drop();
    CHECK(!rec.active() && rec.currentBlock() == -1);
}

static void testReplayRecordingAndPlayback() {
    // The game input: live play latches the raw input at the end of each pass (the player
    // reads the previous pass's), playback feeds entry k - 1 in frame k. Both must hand the
    // player the same inputs; the first frame of a stage sees 0 (the transition zeroed it).
    std::vector<std::uint16_t> raw = {0};  // frame 1: the supervisor's transition pass
    for (int i = 1; i < 200; ++i) raw.push_back(static_cast<std::uint16_t>((i * 37) % 0x200 | (i % 7 == 0 ? BtnMenu : 0)));
    ReplayRecorder rec;
    rec.create(0, Normal, GameConfig{}, kTestExe);
    rec.startBlock(0, {});
    std::uint16_t game = 0, prevGame = 0;  // reset when the hook is registered
    std::vector<std::uint16_t> liveSeen, liveSeenPrev;
    for (std::uint16_t r : raw) {
        liveSeen.push_back(game);
        liveSeenPrev.push_back(prevGame);
        prevGame = game;  // the record hook (priority 16)
        game = r;
        rec.write(r, 0);
    }
    rec.closeBlock();
    const ReplayStage& block = rec.file().stages[0];
    CHECK(block.entries.size() == raw.size() + 2 && block.entries[1].input == 0 && block.entries[0].input == 0);
    ReplayFeed feed;
    feed.start(&block);
    game = prevGame = 0;
    bool same = true;
    for (std::size_t k = 0; k < raw.size(); ++k) {
        prevGame = game;  // the feed hook (priority 5)
        game = feed.next();
        same = same && game == liveSeen[k] && prevGame == liveSeenPrev[k];
        // The entry recorded in this frame (its flags are this frame's events).
        same = same && feed.upcoming().input == raw[k];
    }
    CHECK(same && liveSeen[0] == 0 && liveSeen[1] == 0 && liveSeen[5] == raw[4] && feed.frame() == static_cast<std::int32_t>(raw.size()));
    // The whole raw word is kept (Menu, Q and so on).
    CHECK(block.entries[8].input == raw[7] && (raw[7] & BtnMenu));
    // Then the last entry, the terminator, and 0 past the block's end (the original reads on
    // into the next data).
    CHECK(feed.next() == raw.back() && feed.next() == 0 && !feed.pastEnd() && feed.next() == 0 && feed.pastEnd());

    // FPS: sample s's byte is read one sample late [QUIRK].
    ReplayStage st;
    st.present = true;
    st.entries.resize(70);
    st.fps = {60, 50 | 0x80, 40};
    feed.start(&st);
    feed.next();
    CHECK(feed.fps() == 50 && feed.fpsBad());
    for (int i = 0; i < 30; ++i) feed.next();
    CHECK(feed.fps() == 40 && !feed.fpsBad());
    for (int i = 0; i < 30; ++i) feed.next();
    CHECK(feed.fps() == 40);  // no byte after the last: the value stays

    // The recorder stops at the original's buffer size (115189 entries, 32 minutes).
    ReplayRecorder big;
    big.create(0, Normal, GameConfig{}, kTestExe);
    big.startBlock(0, {});
    for (int i = 0; i < 116000; ++i) big.write(1, 0);
    CHECK(big.file().stages[0].entries.size() == kReplayMaxEntries - 2);  // + the two terminators

    // The restore at a stage's set-up (spec 7.8.2).
    ReplayFile f;
    GameConfig four;
    four.lives = 4;
    f.summary.setConfig(four.save());
    f.stages[0].present = true;
    f.stages[0].endScore = 5000;
    ReplayStage& b = f.stages[1];
    b.present = true;
    b.seed = 0xABCD, b.lives = 5, b.bombs = 2, b.power = 77, b.rank = 20, b.chain = 3, b.spells = 4;
    b.cherry = 1000, b.cherryMax = 300000, b.cherryPlus = 60000, b.totalGraze = 99, b.extendPoints = 33, b.extendsGranted = 1, b.nextExtend = 125;
    GameState g;
    g.setDifficulty(Hard);
    g.lifeCount = 2;
    ZunRng rng(7);
    World w;
    w.state = &g;
    w.rng = &rng;
    GameConfig copy;
    restoreReplayStage(w, f, 1, copy);
    CHECK(rng.currentSeed() == 0xABCD);  // (lives and power draw first; the seed is set last)
    CHECK(g.livesI() == 5 && g.bombsI() == 2 && g.powerI() == 77 && g.rank == 20 && g.minRank == 10 && g.maxRank == 32);
    CHECK(g.fullPowerChain == 3 && g.spellsCaptured == 4 && g.cherry == 1000 && g.cherryMax == 300000 && g.cherryPlus == 50000);
    CHECK(g.totalGraze == 99 && g.extendPoints == 33 && g.extendsGranted == 1 && g.nextExtend == 125);
    CHECK(g.score == 5000 && g.guiScore == 5000);  // stage 2 starts from stage 1's end score
    CHECK(g.lifeCount == 4 && copy.lives == 4 && !g.slowMode);  // the recorded configuration
    // Extra and Phantasm (index 6) start at 0; a stage whose previous block is absent keeps it.
    f.stages[5].present = true;
    f.stages[5].endScore = 9999;
    f.stages[6] = b;
    f.stages[3] = b;
    GameState g2;
    w.state = &g2;
    restoreReplayStage(w, f, 6, copy);
    CHECK(g2.score == 0);
    restoreReplayStage(w, f, 3, copy);
    CHECK(g2.score == 0 && rng.currentSeed() == 0xABCD);
}

static void testReplayFlow() {
    // The save from the result screen, the replay menu and the attract demo, through the
    // supervisor (no th07.exe tables here: games do not start, the requests are checked).
    GameConfig cfg;
    ReplayRecorder demoRec = testRecording(cfg, 2, 40, 0);
    demoRec.file().summary.setShot(4);
    ZunRng drng(5);
    const Bytes demo = demoRec.save(10, 10, 0.0f, "ZUN", "07/19", drng);
    FlowData f;
    mountFlowData(f, {{"demorpy0.rpy", demo}}, "replay");
    {
        Supervisor s;
        CHECK(s.start(flowBoot(f)));
        s.pass(0);
        // Spec 7.6: the recording is saved with the result screen's name and the game's score;
        // the write is skipped for a main game whose configuration changed; it is freed always.
        ReplayRecorder& rec = s.recorder();
        rec = testRecording(s.config(), 0, 20, 0);
        GameState& last = s.context().lastGame;
        last = GameState{};
        last.difficulty = Normal;
        last.score = last.guiScore = 4242;
        const std::string dir = f.dir + "replay/";
        CHECK(s.saveReplay(dir + "th7_03.rpy", "TESTER  ") && !rec.active());
        {
            std::ifstream in(dir + "th7_03.rpy", std::ios::binary);
            const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto r = ReplayFile::parse(bytes, {});
            const auto today = todayDate();
            CHECK(r && r->summary.name() == "TESTER  " && r->summary.score() == 4242 && r->stages[0].endScore == 4242);
            CHECK(r && r->summary.date() == std::string(today.data(), 5) && r->stages[0].entries.size() == 23);
        }
        GameConfig other = s.config();
        other.lives = 4;
        rec = testRecording(other, 0, 20, 0);
        CHECK(!s.saveReplay(dir + "th7_04.rpy", "X") && !std::filesystem::exists(dir + "th7_04.rpy") && !rec.active());
        last.practice = true;  // practice and Extra change the copy (8 or 2 lives): saved
        rec = testRecording(other, 0, 20, 0);
        CHECK(s.saveReplay(dir + "th7_05.rpy", "PRACTICE") && std::filesystem::exists(dir + "th7_05.rpy"));
        // A user file and a broken file in the folder.
        std::filesystem::copy_file(dir + "th7_05.rpy", dir + "th7_ud0001.rpy");
        std::ofstream(dir + "th7_02.rpy", std::ios::binary) << "T7RP broken";
        const auto list = MainMenu::scanReplays(dir, {});
        CHECK(list.size() == 3 && list[0].label == "No.03" && list[1].label == "No.05" && list[2].label == "User ");
        CHECK(list.size() == 3 && list[0].file.summary.name() == "TESTER  " && list[0].file.stages[0].entries.empty());
        std::filesystem::remove(dir + "th7_03.rpy");
        std::filesystem::remove(dir + "th7_05.rpy");
        std::filesystem::remove(dir + "th7_ud0001.rpy");
        s.shutdown();
    }
    {
        // The result screen after a game: name entry, statistics, then "save?" (Yes), the slot
        // list (slot 1 empty), the replay name and END: replay/th7_01.rpy.
        Supervisor s;
        CHECK(s.start(flowBoot(f)));
        s.pass(0);
        s.recorder() = testRecording(s.config(), 0, 25, 0);
        s.startEnding(0, Normal, false, 5000000);
        idle(s, 30);
        CHECK(s.active() == FlowState::ResultAfterGame && s.resultScreen());
        s.pass(0);
        idle(s, 30);
        press(s, BtnMenu);  // the name entry ends at once
        CHECK(s.resultScreen()->state() == ResultScreen::State::Statistics);
        idle(s, 90);
        press(s, BtnShoot);
        idle(s, 31);
        CHECK(s.resultScreen()->state() == ResultScreen::State::Prompt && s.resultScreen()->promptYes());
        press(s, BtnShoot);  // locked until prompt pass 80
        CHECK(s.resultScreen()->state() == ResultScreen::State::Prompt);
        idle(s, 80);
        press(s, BtnRight);
        CHECK(!s.resultScreen()->promptYes());
        press(s, BtnLeft);
        press(s, BtnShoot);
        CHECK(s.resultScreen()->state() == ResultScreen::State::SlotList && !s.resultScreen()->slots()[0].filled);
        idle(s, 20);
        press(s, BtnShoot);
        CHECK(s.resultScreen()->state() == ResultScreen::State::ReplayName);
        // To END: up from the first key wraps to the last row, left wraps to its end.
        if (s.resultScreen()->keyCursor() != ResultScreen::kKeyEnd) {
            press(s, BtnUp);
            press(s, BtnLeft);
        }
        CHECK(s.resultScreen()->keyCursor() == ResultScreen::kKeyEnd);
        press(s, BtnShoot);
        CHECK(s.resultScreen()->state() == ResultScreen::State::Leave && s.resultScreen()->saved() && !s.recorder().active());
        CHECK(std::filesystem::exists(f.dir + "replay/th7_01.rpy"));
        idle(s, 62);
        CHECK(s.active() == FlowState::MainMenu && s.mainMenu());

        // The replay menu: the file, its stage list, mode 1, then a replay game is requested.
        idle(s, 10);
        for (int i = 0; i < 4 && s.mainMenu()->cursor() != 3; ++i) press(s, BtnDown);  // (the ending unlocked Extra Start)
        CHECK(s.mainMenu()->cursor() == 3);
        press(s, BtnShoot);
        CHECK(s.mainMenu()->state() == MainMenu::State::Replay);
        idle(s, 41);
        CHECK(s.mainMenu()->replays().size() == 1 && s.mainMenu()->replays()[0].label == "No.01");
        press(s, BtnShoot);
        CHECK(s.mainMenu()->state() == MainMenu::State::ReplayStages && s.mainMenu()->cursor() == 0);
        press(s, BtnDown);  // stages 2-7 are absent: back to stage 1
        CHECK(s.mainMenu()->cursor() == 0);
        press(s, BtnShoot);
        CHECK(s.mainMenu()->state() == MainMenu::State::ReplayMode);
        press(s, BtnDown);
        s.pass(BtnShoot);
        CHECK(s.requested() == FlowState::Game && s.context().replay && !s.context().demo && s.context().replayFile);
        CHECK(s.context().playbackMode == 1 && s.context().stage == 0 && s.context().character == 1 && s.context().shotType == 1);
        CHECK(s.config().difficulty != Hard);  // the remembered difficulty is not the replay's
        // (The game cannot start here: the title opens, on the replay menu after a replay.)
        idle(s, 3);
        CHECK(s.active() == FlowState::MainMenu && s.mainMenu() && s.mainMenu()->state() == MainMenu::State::Replay && !s.context().replay);
        s.shutdown();
    }
    {
        // The attract demo: more than 900 idle passes on the title load demorpy0.rpy from the
        // archive (the first demo of a session is 0).
        Supervisor s;
        CHECK(s.start(flowBoot(f)));
        s.pass(0);
        int passes = 0;
        while (s.requested() != FlowState::Game && passes < 1200) {
            s.pass(0);
            ++passes;
        }
        CHECK(passes >= 900 && passes <= 902 && s.context().demo && s.context().replay && s.context().demoIndex == 0);
        CHECK(s.context().stage == 2 && s.context().character == 2 && s.context().shotType == 0 && s.context().playbackMode == 0);
        idle(s, 3);
        // Back on the title (not the replay menu) with the demo flags cleared; the next demo
        // (1) is not in this archive, so the idle counter just starts again.
        CHECK(s.active() == FlowState::MainMenu && s.mainMenu() && s.mainMenu()->state() == MainMenu::State::Main && !s.context().replay && !s.context().demo);
        idle(s, 950);
        CHECK(s.context().demoIndex == 1 && s.active() == FlowState::MainMenu && s.requested() == FlowState::MainMenu);
        s.shutdown();
    }
    std::filesystem::remove_all(f.dir);
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

// ---- English text mode (a synthetic thcrap patch stack) ------------------------------------

namespace {

void writeText(const std::filesystem::path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
void writeBytes(const std::filesystem::path& p, const Bytes& b) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// A PNG (RGBA8, zlib "stored" blocks) of w x h pixels.
Bytes pngFile(int w, int h, const Bytes& rgba) {
    auto be32 = [](Bytes& b, std::uint32_t v) {
        for (int k = 3; k >= 0; --k) b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    };
    auto crc32 = [](const Bytes& b, std::size_t from) {
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = from; i < b.size(); ++i) {
            c ^= b[i];
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        return ~c;
    };
    Bytes raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y * w * 4), rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * w * 4));
    }
    Bytes z = {0x78, 0x01};
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        put16(z, static_cast<std::uint16_t>(n));
        put16(z, static_cast<std::uint16_t>(~n));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const Bytes& data) {
        be32(png, static_cast<std::uint32_t>(data.size()));
        const std::size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        be32(png, crc32(png, start));
    };
    Bytes ihdr;
    be32(ihdr, static_cast<std::uint32_t>(w));
    be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}
Bytes solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    Bytes p;
    for (int i = 0; i < w * h; ++i) p.insert(p.end(), {r, g, b, a});
    return p;
}

// msgN.dat: script count, offsets, then each script's commands and a (0, 0) terminator.
struct MsgCmd {
    std::uint16_t time;
    std::uint8_t op;
    Bytes args;
};
Bytes msgLine(int side, int line, const std::string& s) {
    Bytes b;
    put16(b, static_cast<std::uint16_t>(side));
    put16(b, static_cast<std::uint16_t>(line));
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
    return b;
}
Bytes msgFile(const std::vector<std::vector<MsgCmd>>& scripts) {
    Bytes b;
    put32(b, static_cast<std::uint32_t>(scripts.size()));
    for (std::size_t i = 0; i < scripts.size(); ++i) put32(b, 0);
    for (std::size_t i = 0; i < scripts.size(); ++i) {
        set32(b, 4 + 4 * i, static_cast<std::uint32_t>(b.size()));
        for (const MsgCmd& c : scripts[i]) {
            put16(b, c.time);
            b.push_back(c.op);
            b.push_back(static_cast<std::uint8_t>(c.args.size()));
            b.insert(b.end(), c.args.begin(), c.args.end());
        }
        put32(b, 0);  // the terminator: time 0, opcode 0, no arguments
    }
    return b;
}
struct MsgText {
    int time, op, side, line;
    std::string text;
    bool operator==(const MsgText&) const = default;
};
// The text commands of script `index`, from its offset up to its terminator.
std::vector<MsgText> msgTexts(const Bytes& b, int index) {
    std::vector<MsgText> out;
    std::size_t p = b[4 + 4 * static_cast<std::size_t>(index)] | b[5 + 4 * static_cast<std::size_t>(index)] << 8;
    while (p + 4 <= b.size()) {
        const int time = b[p] | b[p + 1] << 8, op = b[p + 2], n = b[p + 3];
        if (op == 3 || op == 8) {
            std::string s;
            for (std::size_t i = p + 8; i < p + 4 + static_cast<std::size_t>(n) && b[i]; ++i) s.push_back(static_cast<char>(b[i]));
            out.push_back({time, op, b[p + 4] | b[p + 5] << 8, b[p + 6] | b[p + 7] << 8, s});
        }
        p += 4 + static_cast<std::size_t>(n);
        if (time == 0 && op == 0) break;
    }
    return out;
}

// thcrap/repos with lang_en, base_tsa and script_latin (and a missing dependency).
struct PatchDirs {
    std::filesystem::path root, base, latin, lang;
    PatchDirs() {
        root = std::filesystem::temp_directory_path() / ("th07_english_test_" + std::to_string(::getpid())) / "thcrap";
        std::filesystem::remove_all(root.parent_path());
        base = root / "repos" / "nmlgc" / "base_tsa";
        latin = root / "repos" / "nmlgc" / "script_latin";
        lang = root / "repos" / "thpatch" / "lang_en";
        writeText(base / "patch.js", R"({"id": "base_tsa"})");
        writeText(latin / "patch.js", R"({"id": "script_latin", "dependencies": ["base_tsa"]})");
        writeText(lang / "patch.js", R"({"id": "lang_en", "dependencies": ["nmlgc/base_tsa", "nmlgc/missing", "nmlgc/script_latin",]})");
    }
    ~PatchDirs() { std::filesystem::remove_all(root.parent_path()); }
};

const std::string kU8Quote = "\xE2\x80\x99";  // ’

}  // namespace

static void testEnglishStackAndLookups() {
    PatchDirs d;
    const auto stack = English::discoverStack(d.root.string());
    CHECK(stack.size() == 3);
    if (stack.size() == 3) {
        CHECK(stack[0] == d.base.string() + "/" && stack[1] == d.latin.string() + "/" && stack[2] == d.lang.string() + "/");
    }
    CHECK(English::discoverStack((d.root / "nowhere").string()).empty());

    writeText(d.base / "th07" / "spells.js", R"({"3": "Base Three", "4": "Four"})");
    writeText(d.lang / "th07" / "spells.js", "{\"3\": \"Lang Three\\u0014a note\"}");
    writeText(d.lang / "themes.js", R"({"th07_01": "Song One", "th06_01": "Not this game"})");
    writeText(d.base / "stringdefs.js", R"({"Music Room Numbered Title": "#<r$%2d$9>: %s"})");
    writeText(d.lang / "th07" / "musiccmt.js", R"({"1": ["@", "First line", ""], "2": ["@", "Second"]})");
    writeText(d.base / "th07" / "stringlocs.v1.00b.js", R"({"Rx98874": "th07 Bomb Reimu A unfocused", "rx98800": "no def", })");
    writeText(d.lang / "stringdefs.js", "{\"th07 Bomb Reimu A unfocused\": \"Spirit Sign \\\"It" + kU8Quote + "s\\\"\"}");
    English e(stack);
    CHECK(e.active() && e.stack().size() == 3);
    // spells.js merged over the stack (the higher patch wins), translator's note dropped.
    CHECK(e.spellName(3) == std::optional<std::string>("Lang Three") && e.spellName(4) == std::optional<std::string>("Four"));
    CHECK(!e.spellName(5));
    // themes.js and musiccmt.js: "@" is the numbered title from stringdefs.
    CHECK(e.musicTitle(1) == std::optional<std::string>("Song One") && !e.musicTitle(2));
    const auto c1 = e.musicComment(1);
    CHECK(c1 && c1->size() == 3);
    if (c1 && c1->size() == 3) {
        CHECK((*c1)[0] == std::optional<std::string>("#<r$ 1$9>: Song One"));
        CHECK((*c1)[1] == std::optional<std::string>("First line") && (*c1)[2] == std::optional<std::string>(""));
    }
    const auto c2 = e.musicComment(2);
    CHECK(c2 && c2->size() == 2 && !(*c2)[0] && (*c2)[1] == std::optional<std::string>("Second"));  // no title: the game's line
    CHECK(!e.musicComment(3));
    // stringlocs (the build variant, keys in either case) and stringdefs.
    CHECK(e.exeString(ExeTables::kBombNameAddresses[0]) == std::optional<std::string>("Spirit Sign \"It" + kU8Quote + "s\""));
    CHECK(!e.exeString(0x498800) && !e.exeString(0x1234));
    CHECK(formatPatchString("#<r$%2d$9>: %s", 7, "Title") == "#<r$ 7$9>: Title");
    CHECK(formatPatchString("%03d|%-4s|%%|%.2d", 5, "ab") == "005|ab  |%|00");
    CHECK(English::gameText("x") == std::string("\x01x"));
}

static void testEnglishMsgPatch() {
    PatchDirs d;
    // Script 0: two boxes at time 10, a boss introduction (op 8) and a box at 20, a box
    // without a translation at 30, a box whose first translated line is bad ruby at 40.
    const Bytes msg = msgFile({
        {{0, 1, {0, 0, 0, 0}},
         {10, 3, msgLine(0, 0, "A")}, {10, 3, msgLine(0, 1, "B")}, {10, 4, {60, 0, 0, 0}},
         {10, 3, msgLine(1, 0, "C")}, {10, 4, {60, 0, 0, 0}},
         {20, 8, msgLine(1, 0, "T")}, {20, 8, msgLine(1, 1, "N")},
         {20, 3, msgLine(0, 0, "D")}, {20, 4, {60, 0, 0, 0}},
         {30, 3, msgLine(0, 0, "E")}, {30, 3, msgLine(0, 1, "F")}, {30, 4, {60, 0, 0, 0}},
         {40, 3, msgLine(0, 0, "H")}, {40, 3, msgLine(0, 1, "I")},
         {50, 0, {}}},
        {{5, 3, msgLine(0, 0, "G")}, {5, 4, {60, 0, 0, 0}}, {6, 0, {}}},
    });
    writeText(d.lang / "th07" / "msg1.dat.jdiff", R"({
        "0": {"10_0": {"lines": ["one"]}, "10_1": {"lines": ["x", "y", "z"]}, "20_h1_0": {"lines": ["Title", "Name"]},
              "20_0": ["dee"], "40_0": {"lines": ["|bad ruby", "eye"]}},
        "1": {"5_0": {"lines": ["Gee\u0014a note"]}}
    })");
    English e(English::discoverStack(d.root.string()));
    Bytes out = msg;
    std::string why;
    const auto diff = Json::parse(R"({"0": {"10_0": ["one"]}})", &why);
    CHECK(diff.has_value());
    auto u8 = [](const std::string& s) { return std::string("\x01") + s; };
    // Through the Vfs read filter, as the game reads it.
    Vfs vfs;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"msg1.dat", msg}, {"other.dat", text("unchanged")}})), &why));
    vfs.setReadFilter([&e](const std::string& name, std::optional<Bytes> b) { return e.filterFile(name, std::move(b)); });
    const auto patched = vfs.read("data/msg1.dat");
    CHECK(patched.has_value() && vfs.read("other.dat") == std::optional<Bytes>(text("unchanged")));
    if (!patched) return;
    const std::vector<MsgText> want0 = {
        {10, 3, 0, 0, u8("one")},                         // "B" dropped: beyond the translation's lines
        {10, 3, 1, 0, u8("x")}, {10, 3, 1, 1, u8("y")}, {10, 3, 1, 2, u8("z")},  // extra lines after the box
        {20, 8, 1, 0, u8("Title")}, {20, 8, 1, 1, u8("Name")},  // "<time>_h1_<i>"
        {20, 3, 0, 0, u8("dee")},                         // the index restarts after an introduction
        {30, 3, 0, 0, "E"}, {30, 3, 0, 1, "F"},           // no translation: the original
        {40, 3, 0, 1, u8("eye")},                         // bad ruby: that line dropped
    };
    const auto got0 = msgTexts(*patched, 0);
    CHECK(got0 == want0);
    // Script 1 moved with the commands; its note kept as an index.
    const auto got1 = msgTexts(*patched, 1);
    CHECK(got1.size() == 1 && !got1.empty() && got1[0].text == u8("Gee") + "\x14" "0" && e.note(0) == "a note" && e.note(1).empty());
    // The extra lines sit before the box's pause; the commands around are kept.
    std::size_t p = 4 + 8, ops = 0, pauses = 0;
    while (p + 4 <= patched->size() && !((*patched)[p] == 0 && (*patched)[p + 1] == 0 && (*patched)[p + 2] == 0)) {
        if ((*patched)[p + 2] == 4) ++pauses;
        ++ops;
        p += 4 + (*patched)[p + 3];
    }
    CHECK(pauses == 4 && ops == 4 + 10 + 1 + 1);
    // patchMsg on its own: unchanged without a matching entry.
    Bytes untouched = msg;
    const auto none = Json::parse(R"({"7": {"10_0": ["x"]}})", &why);
    CHECK(none && !e.patchMsg(untouched, *none) && untouched == msg);
    CHECK(e.patchMsg(out, *diff) && msgTexts(out, 0).front().text == u8("one"));
}

static void testEnglishEnding() {
    // .end jdiff: the run of text lines starting at line L replaced by jdiff["L"].
    const std::string end = std::string("@s3\0" "0\0\n@c255\0\n", 14) + std::string("orig one\0\n", 10) + std::string("orig two\0\n", 10) +
                            std::string("@w5\0" "0\0\n", 7) + std::string("kept\0\n", 6) + std::string("@z\n", 3);
    std::string why;
    const auto diff = Json::parse(R"({"2": {"lines": ["<c$Reimu:$12345678> \"Hi.\"", "<c$$12345678\"> There.\u0014note", ""]},
                                      "3": {"lines": ["never: not the start of a run"]}})",
                                  &why);
    CHECK(diff.has_value());
    if (!diff) return;
    Bytes b(end.begin(), end.end());
    CHECK(English::patchEnd(b, *diff));
    const std::string want = std::string("@s3\0" "0\0\n@c255\0\n", 14) + "\x01<c$Reimu:$12345678> \"Hi.\"" + std::string("\0\n", 2) +
                             "\x01<c$$12345678\"> There." + std::string("\0\n", 2) + std::string("\0\n", 2) +
                             std::string("@w5\0" "0\0\n", 7) + std::string("kept\0\n", 6) + std::string("@z\n", 3);
    CHECK(std::string(b.begin(), b.end()) == want);

    // The ending reads a translated line whole: odd lengths, longer than the 66-byte buffer.
    const std::string longLine = std::string("\x01") + std::string(81, 'x');
    Bytes script(longLine.begin(), longLine.end());
    for (char c : std::string("\0\n@z\n", 5)) script.push_back(static_cast<std::uint8_t>(c));
    FlowContext ctx;
    AnmLibrary anm;
    ZunRng rng{1};
    ScoreData scores;
    ctx.anm = &anm;
    ctx.rng = &rng;
    ctx.scores = &scores;
    ctx.request = [](FlowState) {};
    Ending ending(ctx);
    ending.startScript(script, "test.end");
    MenuInput in;
    in.sample(0);
    ending.update(in);
    CHECK(ending.lastLine() == longLine);

    // The layout markup (thcrap's layout_process), 6 texels a character.
    auto measure = [](std::string_view s, bool) { return 6.0f * static_cast<float>(s.size()); };
    float total = 0.0f;
    auto runs = layoutMarkup("<c$Reimu:$12345678> \"Hi.\"", measure, 512.0f, &total);
    CHECK(runs.size() == 2 && total == 84.0f);
    if (runs.size() == 2) {
        CHECK(runs[0].text == "Reimu:" && runs[0].x == 6.0f && runs[0].width == 36.0f);  // centred in the width of "12345678"
        CHECK(runs[1].text == " \"Hi.\"" && runs[1].x == 48.0f);
    }
    runs = layoutMarkup("<c$$12345678\"> There.", measure, 512.0f, &total);
    CHECK(runs.size() == 1 && !runs.empty() && runs[0].x == 54.0f);  // after the quote of the line above
    runs = layoutMarkup("No. <r$ 7$99>  Title", measure, 512.0f, &total);
    CHECK(runs.size() == 3 && runs[1].text == " 7" && runs[1].x == 24.0f && runs[2].x == 36.0f);
    runs = layoutMarkup("say <i$this> now", measure, 512.0f, &total);
    CHECK(runs.size() == 3 && runs[1].italic && runs[1].x == 24.0f && !runs[2].italic && runs[2].x == 48.0f);
    runs = layoutMarkup("<s$hidden>shown <single>", measure, 512.0f, &total);
    CHECK(runs.size() == 2 && runs[0].text == "shown " && runs[0].x == 0.0f && runs[1].text == "<single>");
    // Drawn: narrowed as a whole when wider than the room.
    FallbackTextRenderer fb;
    Image band(128, 16);
    TextStyle style;
    CHECK(renderTranslated(fb, "<c$Reimu:$12345678> \"Hi.\"", style, 0.0f, 60.0f, TextAlign::Left, band) == 60.0f);
    CHECK(renderTranslated(fb, "plain", style, 10.0f, 100.0f, TextAlign::Right, band) == 30.0f);
    // The desktop fallback shows typographic punctuation and accents as ASCII.
    CHECK(asciiFallback("It" + kU8Quote + "s \xE2\x80\x9Cok\xE2\x80\x9D\xE2\x80\xA6 \xC3\xA9\xC5\x8D\xE3\x80\x80!") == "It's \"ok\"... eo  !");
    CHECK(wrapText("aaa bbb ccc\nd", 42.0f, [](std::string_view s) { return 6.0f * static_cast<float>(s.size()); }) ==
          std::vector<std::string>({"aaa bbb", "ccc", "d"}));
}

static void testEnglishPictures() {
    // thcrap's PNG-over-THTX blits: an opaque original gets the picture blended over it, a
    // transparent one is overwritten, an empty part of the picture leaves it alone.
    Bytes tex;
    for (int i = 0; i < 8; ++i) tex.insert(tex.end(), {10, 20, 30, 255});  // 4 x 2, BGRA
    Bytes pic;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (x < 2) pic.insert(pic.end(), {200, 100, 50, 128});
            else pic.insert(pic.end(), {1, 2, 3, 0});
        }
    }
    std::string why;
    const auto img = decodeImage(pngFile(4, 2, pic), &why);
    CHECK(img.has_value());
    if (!img) return;
    CHECK(English::blitPng(tex.data(), 1, 4, 2, {{0, 0, 2, 2}, {2, 0, 2, 2}}, *img, 0, 0));
    const std::uint8_t blended[4] = {(10 * 127 + 50 * 128) >> 8, (20 * 127 + 100 * 128) >> 8, (30 * 127 + 200 * 128) >> 8, 255};
    CHECK(std::memcmp(tex.data(), blended, 4) == 0 && std::memcmp(tex.data() + 4 * 5, blended, 4) == 0);
    CHECK(tex[8] == 10 && tex[9] == 20 && tex[10] == 30 && tex[11] == 255);  // the empty half: unchanged
    Bytes tex5 = {0xF0, 0x80, 0xF0, 0x80};  // A4R4G4B4, alpha 8: not opaque
    const auto red = decodeImage(pngFile(2, 1, solid(2, 1, 255, 0, 0, 255)), &why);
    CHECK(red && English::blitPng(tex5.data(), 5, 2, 1, {}, *red, 0, 0) && tex5 == Bytes({0x00, 0xFF, 0x00, 0xFF}));
    Bytes tex3 = {0, 0};
    CHECK(red && English::blitPng(tex3.data(), 3, 1, 1, {}, *red, 2, 0) == false && tex3 == Bytes({0, 0}));  // outside the picture
    CHECK(red && English::blitPng(tex3.data(), 3, 1, 1, {}, *red, 1, 0) && tex3 == Bytes({0x00, 0xF8}));  // R5G6B5: overwritten

    // Through the file filter: every patch's picture in stack order, sprite by sprite, and
    // whole-file replacements.
    PatchDirs d;
    writeBytes(d.base / "th07" / "data" / "test.png", pngFile(16, 16, solid(16, 16, 255, 0, 0, 255)));
    Bytes half;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (x < 8) half.insert(half.end(), {0, 0, 0, 0});
            else half.insert(half.end(), {0, 0, 255, 255});
        }
    }
    writeBytes(d.lang / "th07" / "data" / "test.png", pngFile(16, 16, half));
    writeText(d.latin / "th07" / "text.anm", "replaced");
    English e(English::discoverStack(d.root.string()));
    EntryDef entry;  // 16 x 16 THTX named data/test.png, one sprite: the whole texture
    entry.sprites.push_back({0, 0, 0, 16, 16});
    Bytes anm = anmFile({entry});
    const Bytes original = anm;
    CHECK(e.patchAnm(anm, "front.anm"));
    std::size_t thtx = 0;
    for (std::size_t i = 0; i + 4 < anm.size(); ++i) {
        if (std::memcmp(anm.data() + i, "THTX", 4) == 0) thtx = i + 0x10;
    }
    CHECK(thtx != 0);
    const std::uint8_t r1 = (255 * 255) >> 8;  // base: red blended over the opaque original
    const std::uint8_t keep = static_cast<std::uint8_t>((r1 * 255) >> 8), blue = (255 * 255) >> 8;
    CHECK(anm[thtx + 2] == keep && anm[thtx + 0] == 0 && anm[thtx + 3] == 255);  // left: the base's red (lang empty there)
    CHECK(anm[thtx + 8 * 4 + 0] == blue && anm[thtx + 8 * 4 + 2] == 0);           // right: the lang patch's blue on top
    const auto filtered = e.filterFile("front.anm", original);
    CHECK(filtered == std::optional<Bytes>(anm));
    CHECK(e.filterFile("text.anm", text("original")) == std::optional<Bytes>(text("replaced")));
    EntryDef blank;  // a blank '@' texture: never patched
    blank.format = 0;
    const Bytes blankAnm = anmFile({blank});
    CHECK(e.filterFile("etama.anm", blankAnm) == std::optional<Bytes>(blankAnm));
    CHECK(!e.filterFile("missing.dat", std::nullopt));
}

static void testEnglishTextImages() {
    PatchDirs d;
    // base_tsa's th07.js defines the images; lang_en has them: title 3 cells, name 4 cells.
    writeText(d.base / "th07.js", R"({"breakpoints": {"textimage_init": {"images": {
        "0x702": {"filename": "ti_bosstitle.png", "texture_slot": 50, "sprite_w": 8, "sprite_h": 4,
                  "script": "0300 0c00 00000000 02000000 0600 1400 00000000 +224.0f +352.0f +0.0f ffff 0800 1000 0000"},
        "0x703": {"filename": "ti_bossname.png", "texture_slot": 51, "sprite_w": 8, "sprite_h": 4,
                  "script": "0300 0c00 00000000 03000000 0600 1400 00000000 +100.0f -8.5f +0.0f ffff 0800 1000 0000"}}}}})");
    {
        English none(English::discoverStack(d.root.string()));
        CHECK(!none.hasTextImages() && none.textImageSprite(0, 1, 0) == -1 && !none.textImageAnm());
    }
    writeBytes(d.lang / "th07" / "ti_bosstitle.png", pngFile(8, 12, solid(8, 12, 255, 255, 255, 255)));
    writeBytes(d.lang / "th07" / "ti_bossname.png", pngFile(8, 16, solid(8, 16, 0, 255, 0, 255)));
    English e(English::discoverStack(d.root.string()));
    CHECK(e.hasTextImages());
    CHECK(e.textImageSprite(0, 2, 0) == 1 && e.textImageSprite(1, 2, 0) == 3 + 1);  // stage 2: the second boss
    CHECK(e.textImageSprite(0, 3, 0) == 2 && e.textImageSprite(1, 3, 0) == 5);
    CHECK(e.textImageSprite(0, 4, 0x4AD + 6) == -1);  // the third sister of stage 4: cell 5, not in the title image
    CHECK(e.textImageSprite(2, 1, 0) == -1);
    // The synthesized ANM loads like a game file through the Vfs filter.
    Vfs vfs;
    std::string why;
    vfs.mount(openArchive("th07.dat", mem(pbg4({{"text.anm", anmFile({EntryDef{}})}})), &why));
    vfs.setReadFilter([&e](const std::string& name, std::optional<Bytes> b) { return e.filterFile(name, std::move(b)); });
    TextureLibrary textures;
    AnmLibrary lib;
    CHECK(lib.load(vfs, textures, English::kTextImageFile, 48, 0x5E0, &why));
    const th07::AnmSprite* name1 = lib.sprite(0x5E0 + 4);
    CHECK(name1 && name1->startY == 12.0f + 4.0f && name1->endX == 8.0f && lib.sprite(0x5E0 + 6) && !lib.sprite(0x5E0 + 7));
    ZunRng rng{1};
    AnmVm vm;
    vm.startFresh(lib, rng, 0x5E1);
    CHECK(vm.position.x == 100.0f && vm.position.y == -8.5f && vm.activeSprite == 0x5E0 + 3);
    CHECK(vm.selectSprite(lib, 0x5E0 + 5) && vm.sprite && vm.sprite->startY == 12.0f + 8.0f);
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
        {"th07.cfg: layout, validation, rebuild rules", testGameConfig},
        {"ANM: version-2 chains, THTX textures, slots", testAnmLoad},
        {"ANM VM: timing, waits, loops, registers, RNG, channels", testAnmVm},
        {"ANM drawing: rounding, anchors, rotation, colour", testAnmDraw},
        {"stage backgrounds: .std, script, camera, culling", testBackground},
        {"game state: extends, rank, score roll, cherry", testGameState},
        {"player: .sht, movement, shots, death, deathbomb", testPlayer},
        {"ECL: file, header and offsets, x86 conversions", testEclFile},
        {"ECL: arguments, variables, self-modifying writes", testEclArguments},
        {"ECL: random variables and their RNG steps", testEclRandomVariables},
        {"ECL: timelines spawn enemies", testEclTimelines},
        {"ECL: movement modes, bounds, interpolators", testEclMovement},
        {"ECL: calls, returns, interrupts, waits, jumps", testEclVmControl},
        {"enemies: death, item drops, shot damage, Cherry", testEnemyDeathAndDamage},
        {"enemies: spell cards, timeout, capture", testEnemySpellCards},
        {"enemies: callbacks, death types, despawn, game speed", testEnemyCallbacks},
        {"enemies: bullet patterns and rank", testEnemyBullets},
        {"enemies: ex routines on the bullet pool", testEnemyExRoutines},
        {"LZSS encoder: round trips", testLzssEncoder},
        {"score.dat: layout, codec, walks, VRSM, hi-scores", testScoreFile},
        {"unlock rules, clear progress, spell history, stats", testUnlockRules},
        {"menu input: pressed, masks, auto-repeat", testMenuInput},
        {"Music Room: musiccmt.txt", testMusicComments},
        {"supervisor: title, results, Music Room, ending", testSupervisor},
        {"replays: container, checksum, key, validation", testReplayContainer},
        {"replays: input latency, feed, FPS, stage restore", testReplayRecordingAndPlayback},
        {"replays: save prompt, slot list, menu, demo", testReplayFlow},
        {"pause and continue menus: snapshot, locks", testInGameMenus},
        {"playfield camera: projected sprites", testPlayfieldCamera},
        {"screen effects: shake draws, pulses, fades", testScreenEffects},
        {"exe strings: printf-style formats without printf", testExeStringFormat},
        {"English text: patch stack, spells, music, strings", testEnglishStackAndLookups},
        {"English text: msg jdiff keys and line rules", testEnglishMsgPatch},
        {"English text: endings, layout markup, fallback", testEnglishEnding},
        {"English text: PNG over THTX, replacements", testEnglishPictures},
        {"English text: boss introduction text images", testEnglishTextImages},
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
