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
#include <memory>
#include <string>
#include <vector>

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
#include "th07/Player.hpp"
#include "th07/World.hpp"
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
    std::string err;
    ew->vfs.mount(openArchive("th07.dat",
                              mem(pbg4({{"stg1enm.anm", anmFile({enm})},
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
