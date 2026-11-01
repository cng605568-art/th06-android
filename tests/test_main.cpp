// Host tests for the engine core: archive formats, decoders, the object pool and a
// deterministic headless run of the game simulation. No GPU, no SDL.
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <zstd.h>

#include "core/Math.hpp"
#include "core/ObjectPool.hpp"
#include "game/Game.hpp"
#include "game/GameArt.hpp"
#include "gfx/Anm.hpp"
#include "gfx/Image.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Archive.hpp"
#include "io/BitReader.hpp"
#include "io/Lzss.hpp"
#include "io/MappedFile.hpp"
#include "io/Vfs.hpp"

namespace th06 {
std::array<std::uint8_t, 16> pkglKeystreamForTests(std::uint32_t seed);
std::uint32_t pkglCrc32ForTests(std::string_view s);
}  // namespace th06

using namespace th06;
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

// ---- test-side encoders --------------------------------------------------------------

class BitWriter {
  public:
    void write(std::uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            cur_ = static_cast<std::uint8_t>((cur_ << 1) | ((value >> i) & 1u));
            if (++n_ == 8) {
                out_.push_back(cur_);
                cur_ = 0;
                n_ = 0;
            }
        }
    }
    void writeVar(std::uint32_t v) {  // same size choice as thtk's th06_write_uint32
        int bytes = 1;
        if (v & 0xffffff00u) bytes = 2;
        if (v & 0xffff0000u) bytes = 3;
        if (v & 0xff000000u) bytes = 4;
        write(static_cast<std::uint32_t>(bytes - 1), 2);
        write(v, bytes * 8);
    }
    Bytes finish() {
        if (n_) out_.push_back(static_cast<std::uint8_t>(cur_ << (8 - n_)));
        n_ = 0;
        cur_ = 0;
        return std::move(out_);
    }

  private:
    Bytes out_;
    std::uint8_t cur_ = 0;
    int n_ = 0;
};

// Greedy LZSS encoder producing streams for lzss::decompress (dict pos = 1 + index).
static Bytes lzssEncode(const Bytes& in) {
    BitWriter w;
    std::size_t p = 0;
    while (p < in.size()) {
        std::size_t bestLen = 0, bestStart = 0;
        const std::size_t windowStart = p > 8191 ? p - 8191 : 0;
        for (std::size_t s = windowStart; s < p; ++s) {
            if (((1 + s) & 0x1fff) == 0) continue;  // position 0 is the terminator
            std::size_t len = 0;
            while (len < 18 && p + len < in.size() && in[s + len] == in[p + len]) ++len;
            if (len > bestLen) {
                bestLen = len;
                bestStart = s;
                if (len == 18) break;
            }
        }
        if (bestLen >= 3) {
            w.write(0, 1);
            w.write(static_cast<std::uint32_t>((1 + bestStart) & 0x1fff), 13);
            w.write(static_cast<std::uint32_t>(bestLen - 3), 4);
            p += bestLen;
        } else {
            w.write(1, 1);
            w.write(in[p], 8);
            ++p;
        }
    }
    w.write(0, 1);
    w.write(0, 13);
    return w.finish();
}

struct TestFile {
    std::string name;
    Bytes data;
};

static Bytes buildPbg3(const std::vector<TestFile>& files) {
    Bytes out(13, 0);  // magic + room for the largest possible header, like thtk
    std::memcpy(out.data(), "PBG3", 4);
    struct Entry {
        std::uint32_t checksum, offset, size;
        std::string name;
    };
    std::vector<Entry> entries;
    for (const TestFile& f : files) {
        const Bytes z = lzssEncode(f.data);
        std::uint32_t sum = 0;
        for (const std::uint8_t b : z) sum += b;
        entries.push_back({sum, static_cast<std::uint32_t>(out.size()),
                           static_cast<std::uint32_t>(f.data.size()), f.name});
        out.insert(out.end(), z.begin(), z.end());
    }
    const auto tableOffset = static_cast<std::uint32_t>(out.size());
    BitWriter t;
    for (const Entry& e : entries) {
        t.writeVar(0);
        t.writeVar(0);
        t.writeVar(e.checksum);
        t.writeVar(e.offset);
        t.writeVar(e.size);
        for (const char c : e.name) t.write(static_cast<std::uint8_t>(c), 8);
        t.write(0, 8);
    }
    const Bytes table = t.finish();
    out.insert(out.end(), table.begin(), table.end());
    BitWriter h;
    h.writeVar(static_cast<std::uint32_t>(entries.size()));
    h.writeVar(tableOffset);
    const Bytes header = h.finish();
    std::memcpy(out.data() + 4, header.data(), header.size());
    return out;
}

struct PkglMember {
    std::string name;
    Bytes data;
    bool compress;
    std::uint32_t key;
};

template <class T>
static void putLe(Bytes& b, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

static Bytes buildPkgl(const std::string& stem, const std::vector<PkglMember>& members) {
    std::size_t tableSize = 0;
    for (const auto& m : members) tableSize += 32 + m.name.size();
    Bytes out(8 + tableSize, 0);
    std::memcpy(out.data(), "PKGL", 4);
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>(tableSize >> (8 * i));

    Bytes table;
    for (const auto& m : members) {
        Bytes payload = m.data;
        if (m.compress) {
            payload.resize(ZSTD_compressBound(m.data.size()));
            const std::size_t z = ZSTD_compress(payload.data(), payload.size(), m.data.data(), m.data.size(), 19);
            CHECK(!ZSTD_isError(z));
            payload.resize(z);
        }
        const auto ks = pkglKeystreamForTests(m.key);
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= ks[i & 15];
        const std::uint64_t offset = out.size();
        out.insert(out.end(), payload.begin(), payload.end());
        putLe<std::uint16_t>(table, m.compress ? 1 : 0);
        putLe<std::uint32_t>(table, m.key);
        putLe<std::uint64_t>(table, m.data.size());
        putLe<std::uint64_t>(table, payload.size());
        putLe<std::uint64_t>(table, offset);
        putLe<std::uint16_t>(table, static_cast<std::uint16_t>(m.name.size()));
        table.insert(table.end(), m.name.begin(), m.name.end());
    }
    const auto ks = pkglKeystreamForTests(pkglCrc32ForTests(stem));
    for (std::size_t i = 0; i < table.size(); ++i) out[8 + i] = table[i] ^ ks[i & 15];
    return out;
}

// 32-bit A8R8G8B8 DDS (bytes B,G,R,A in memory).
static Bytes buildDdsArgb(int w, int h, const std::vector<std::array<std::uint8_t, 4>>& rgba) {
    Bytes b(128, 0);
    std::memcpy(b.data(), "DDS ", 4);
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put32(4, 124);
    put32(8, 0x100F);
    put32(12, static_cast<std::uint32_t>(h));
    put32(16, static_cast<std::uint32_t>(w));
    put32(76, 32);
    put32(80, 0x41);  // RGB | ALPHAPIXELS
    put32(88, 32);
    put32(92, 0x00ff0000);
    put32(96, 0x0000ff00);
    put32(100, 0x000000ff);
    put32(104, 0xff000000);
    for (const auto& p : rgba) {
        b.push_back(p[2]);
        b.push_back(p[1]);
        b.push_back(p[0]);
        b.push_back(p[3]);
    }
    return b;
}

struct TestScript {
    std::uint32_t id;
    std::int32_t sprite;  // -1: the script never sets a sprite
};

// ANM v0 file: header, sprite offsets, script table, sprites, script bytecode, names.
static Bytes buildAnm(int texW, int texH, const std::string& image, const std::string& alpha,
                      std::uint32_t colorKey, const std::vector<AnmSprite>& sprites,
                      const std::vector<TestScript>& scripts = {{7, 1}}) {
    const std::size_t n = sprites.size(), m = scripts.size();
    const std::size_t spritesAt = 0x40 + 4 * n + 8 * m;
    const std::size_t codeAt = spritesAt + 20 * n;
    constexpr std::size_t kScriptBytes = 12 + 8 + 4;  // scale(8 args), set sprite, delete
    const std::size_t namesAt = codeAt + kScriptBytes * m;
    Bytes b(namesAt, 0);
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put32(0x00, static_cast<std::uint32_t>(n));
    put32(0x04, static_cast<std::uint32_t>(m));
    put32(0x0C, static_cast<std::uint32_t>(texW));
    put32(0x10, static_cast<std::uint32_t>(texH));
    put32(0x14, 1);
    put32(0x18, colorKey);
    put32(0x1C, static_cast<std::uint32_t>(namesAt));
    if (!alpha.empty()) put32(0x24, static_cast<std::uint32_t>(namesAt + image.size() + 1));
    for (std::size_t i = 0; i < n; ++i) {
        put32(0x40 + 4 * i, static_cast<std::uint32_t>(spritesAt + 20 * i));
        const std::size_t sp = spritesAt + 20 * i;
        put32(sp, sprites[i].id);
        float f[4] = {sprites[i].x, sprites[i].y, sprites[i].w, sprites[i].h};
        std::memcpy(b.data() + sp + 4, f, 16);
    }
    for (std::size_t k = 0; k < m; ++k) {
        const std::size_t table = 0x40 + 4 * n + 8 * k;
        const std::size_t code = codeAt + kScriptBytes * k;
        put32(table, scripts[k].id);
        put32(table + 4, static_cast<std::uint32_t>(code));
        b[code + 2] = 2;  // scale x, y: an instruction before the sprite is set
        b[code + 3] = 8;
        if (scripts[k].sprite >= 0) {
            b[code + 12 + 2] = 1;  // set sprite
            b[code + 12 + 3] = 4;
            put32(code + 16, static_cast<std::uint32_t>(scripts[k].sprite));
        } else {
            b[code + 12 + 2] = 6;  // nop
            b[code + 12 + 3] = 4;
        }
        // trailing 4 zero bytes = delete
    }
    b.insert(b.end(), image.begin(), image.end());
    b.push_back(0);
    if (!alpha.empty()) {
        b.insert(b.end(), alpha.begin(), alpha.end());
        b.push_back(0);
    }
    return b;
}

static Bytes patterned(std::size_t n, std::uint32_t seed) {
    Bytes out(n);
    Rng rng(seed);
    for (std::size_t i = 0; i < n; ++i) {
        // Mix of runs, repeats and noise so every LZSS path is exercised.
        if (i % 300 < 120) out[i] = static_cast<std::uint8_t>("touhou koumakyou "[i % 17]);
        else if (i % 300 < 180) out[i] = 0;
        else out[i] = static_cast<std::uint8_t>(rng.next());
    }
    return out;
}

static std::unique_ptr<Archive> open(const std::string& name, const Bytes& bytes) {
    std::string err;
    auto a = openArchive(name, MappedFile::fromMemory(bytes), &err);
    if (!a) std::fprintf(stderr, "  open %s: %s\n", name.c_str(), err.c_str());
    return a;
}

static Bytes extract(const Archive& a, const std::string& name) {
    for (std::size_t i = 0; i < a.entries().size(); ++i) {
        if (a.entries()[i].name == name) {
            Bytes out;
            std::string err;
            if (!a.extract(i, out, &err)) std::fprintf(stderr, "  extract %s: %s\n", name.c_str(), err.c_str());
            return out;
        }
    }
    return {};
}

static void dumpIfRequested(const std::string& name, const Bytes& bytes) {
    if (const char* dir = std::getenv("TH06_TEST_DUMP")) {
        std::ofstream(std::filesystem::path(dir) / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

// ---- tests ---------------------------------------------------------------------------

static void testBitReader() {
    BitWriter w;
    w.write(0b101, 3);
    w.writeVar(0x12);
    w.writeVar(0x1234);
    w.writeVar(0x123456);
    w.writeVar(0xDEADBEEF);
    w.write(0x7FFFFFFF, 31);
    const Bytes b = w.finish();
    BitReader r(b);
    CHECK(r.read(3) == 0b101);
    CHECK(r.readVarU32() == 0x12);
    CHECK(r.readVarU32() == 0x1234);
    CHECK(r.readVarU32() == 0x123456);
    CHECK(r.readVarU32() == 0xDEADBEEF);
    CHECK(r.read(31) == 0x7FFFFFFF);
    CHECK(!r.overrun());
    r.read(16);
    CHECK(r.overrun());
}

static void testLzss() {
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{17}, std::size_t{9000}, std::size_t{20000}}) {
        const Bytes data = patterned(size, static_cast<std::uint32_t>(size + 1));
        const Bytes z = lzssEncode(data);
        Bytes out(data.size());
        CHECK(lzss::decompress(z, out) == data.size());
        CHECK(out == data);
        if (size > 1000) CHECK(z.size() < data.size());
    }
    // A stream that ends (terminator) before the expected size is reported as short.
    const Bytes data = patterned(500, 3);
    const Bytes z = lzssEncode(data);
    Bytes big(800);
    CHECK(lzss::decompress(z, big) == 500);
    // Truncated input never claims success.
    const Bytes cut(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(z.size() / 2));
    Bytes out(data.size());
    CHECK(lzss::decompress(cut, out) < data.size());
}

static void testPbg3() {
    std::vector<TestFile> files = {
        {"etama3.anm", patterned(3000, 1)},
        {"plst00.wav", patterned(12000, 2)},
        {"stage1.std", patterned(64, 3)},
        {"empty.txt", {}},
    };
    const Bytes arc = buildPbg3(files);
    dumpIfRequested("test_pbg3.dat", arc);
    auto a = open("紅魔郷CM.DAT", arc);
    CHECK(a != nullptr);
    if (!a) return;
    CHECK(a->format() == "PBG3");
    CHECK(a->entries().size() == files.size());
    for (const TestFile& f : files) CHECK(extract(*a, f.name) == f.data);

    Bytes corrupt = arc;
    corrupt[0] = 'X';
    std::string err;
    CHECK(openArchive("bad.dat", MappedFile::fromMemory(corrupt), &err) == nullptr);
    Bytes truncated(arc.begin(), arc.begin() + 20);
    CHECK(openArchive("short.dat", MappedFile::fromMemory(truncated), &err) == nullptr);
}

static void testPkgl() {
    // Golden keystreams from an independent Python transcription of the format notes.
    struct Golden {
        std::uint32_t seed;
        std::array<std::uint8_t, 16> ks;
    };
    const Golden golden[] = {
        {0x00000000u, {0xc1, 0x5c, 0x02, 0x89, 0xec, 0x2d, 0x0a, 0x91, 0x67, 0xec, 0x8e, 0x65, 0xa1, 0x8d, 0xeb, 0xbe}},
        {0x00000001u, {0x8e, 0x60, 0x75, 0x4c, 0x9d, 0xc5, 0x65, 0x6f, 0xdd, 0xba, 0x50, 0xf0, 0xb7, 0xe4, 0x9e, 0x77}},
        {0xdeadbeefu, {0x4b, 0x4f, 0x84, 0x02, 0xc9, 0xa5, 0xf4, 0xbf, 0xfc, 0xb0, 0x05, 0xa9, 0x51, 0xdc, 0xe5, 0xc1}},
        {0x8bc79290u, {0x5f, 0x85, 0xc0, 0x8c, 0x90, 0x91, 0xe0, 0x18, 0x40, 0xe9, 0xcd, 0x14, 0xbe, 0x53, 0x6c, 0x4c}},
        {0x6c9807bau, {0x0a, 0x61, 0x80, 0x92, 0x91, 0x53, 0x2b, 0x49, 0x2d, 0x52, 0x88, 0x96, 0x03, 0x6b, 0x93, 0x3f}},
    };
    for (const Golden& g : golden) CHECK(pkglKeystreamForTests(g.seed) == g.ks);
    CHECK(pkglCrc32ForTests("123456789") == 0xCBF43926u);
    CHECK(pkglCrc32ForTests("th06CM") == 0x8bc79290u);

    const std::vector<PkglMember> members = {
        {"ver0102.dat", Bytes{'T', 'h', 'i', 's', ' ', 'G', 'a', 'm', 'e'}, false, 0x1234u},
        {"eff00.dds", patterned(40000, 9), true, 0xCAFEF00Du},
        {"th06_01.pos", Bytes{1, 2, 3, 4, 5, 6, 7, 8}, false, 0u},
    };
    const Bytes arc = buildPkgl("th06CM", members);
    dumpIfRequested("th06CM.dat", arc);

    auto a = open("th06CM.dat", arc);
    CHECK(a != nullptr);
    if (a) {
        CHECK(a->format() == "PKGL");
        CHECK(a->entries().size() == members.size());
        for (const auto& m : members) CHECK(extract(*a, m.name) == m.data);
    }
    // Renamed copy: the key falls back to the seven shipped file-name stems.
    CHECK(open("renamed copy.dat", buildPkgl("th06ST", members)) != nullptr);
    // Unknown key: rejected instead of returning garbage entries.
    std::string err;
    CHECK(openArchive("other.dat", MappedFile::fromMemory(buildPkgl("custom", members)), &err) == nullptr);
}

static void testImages() {
    static const std::uint8_t kPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66,
        0x1a, 0x00, 0x00, 0x00, 0x20, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x9f, 0xe1, 0x3f, 0x43, 0x03, 0x88, 0x62, 0xf8, 0xff, 0xff, 0xff, 0x7f, 0x2e, 0x11, 0x39, 0x0d,
        0x06, 0x06, 0x86, 0xff, 0x00, 0x87, 0xf5, 0x09, 0xdc, 0xd8, 0x16, 0x16, 0xd1, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    std::string err;
    auto png = decodeImage(kPng, &err);
    CHECK(png.has_value());
    if (png) {
        CHECK(png->width == 3 && png->height == 2);
        CHECK(std::memcmp(png->pixel(0, 0), "\xff\x00\x00\xff", 4) == 0);
        CHECK(png->pixel(1, 0)[3] == 128);
        CHECK(std::memcmp(png->pixel(1, 1), "\x0a\x14\x1e\x28", 4) == 0);
    }

    auto dds = decodeImage(buildDdsArgb(2, 1, {{{1, 2, 3, 4}}, {{250, 251, 252, 253}}}), &err);
    CHECK(dds.has_value());
    if (dds) {
        CHECK(std::memcmp(dds->pixel(0, 0), "\x01\x02\x03\x04", 4) == 0);
        CHECK(std::memcmp(dds->pixel(1, 0), "\xfa\xfb\xfc\xfd", 4) == 0);
    }

    // BC1 (DXT1): one 4x4 block, colour0 = pure red, all indices 0, on a 3x3 image.
    Bytes bc1(128, 0);
    std::memcpy(bc1.data(), "DDS ", 4);
    bc1[4] = 124;
    bc1[12] = 3;
    bc1[16] = 3;
    bc1[80] = 0x04;
    std::memcpy(bc1.data() + 84, "DXT1", 4);
    const std::uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
    bc1.insert(bc1.end(), block, block + 8);
    auto red = decodeImage(bc1, &err);
    CHECK(red.has_value());
    if (red) {
        CHECK(red->width == 3 && red->height == 3);
        CHECK(std::memcmp(red->pixel(2, 2), "\xff\x00\x00\xff", 4) == 0);
    }
    Bytes truncatedBc1(bc1.begin(), bc1.end() - 4);
    CHECK(!decodeImage(truncatedBc1, &err).has_value());

    Image keyed(2, 1);
    std::memcpy(keyed.pixel(0, 0), "\x10\x20\x30\xff", 4);
    std::memcpy(keyed.pixel(1, 0), "\x11\x20\x30\xff", 4);
    applyColorKey(keyed, 0xff102030u);
    CHECK(keyed.pixel(0, 0)[3] == 0 && keyed.pixel(1, 0)[3] == 255);

    Image mask(1, 1);
    std::memcpy(mask.pixel(0, 0), "\x00\x00\x7f\xff", 4);
    applyAlphaFromMask(keyed, mask);
    CHECK(keyed.pixel(1, 0)[3] == 0x7f);

    const Image padded = padTo(keyed, 4, 4);
    CHECK(padded.width == 4 && padded.height == 4 && padded.pixel(3, 3)[3] == 0);
    CHECK(std::memcmp(padded.pixel(1, 0), keyed.pixel(1, 0), 4) == 0);
}

static void testAnmAndVfs() {
    const std::vector<AnmSprite> sprites = {{0, 0, 0, 16, 16}, {1, 16, 0, 16, 16}, {2, 0, 16, 32, 48}};
    const Bytes anmBytes = buildAnm(64, 64, "data/etama3.png", "data/etama3_a.png", 0, sprites);
    std::string err;
    auto anm = parseAnm(anmBytes, &err);
    CHECK(anm.has_value());
    if (anm) {
        CHECK(anm->sprites.size() == 3);
        CHECK(anm->imageName == "data/etama3.png");
        CHECK(anm->alphaName == "data/etama3_a.png");
        CHECK(anm->textureWidth == 64 && anm->sprites[2].h == 48.0f);
        CHECK(anm->scripts.size() == 1 && anm->scripts[0].id == 7 && anm->scripts[0].firstSprite == 1);
        CHECK(anm->scriptFirstSprite(7) == 1 && anm->scriptFirstSprite(99) == -1);
        CHECK(anm->spriteIndexById(2) == 2 && anm->spriteIndexById(50) == -1);
    }
    Bytes broken = anmBytes;
    broken[0] = 0xff;  // absurd sprite count
    CHECK(!parseAnm(broken, &err).has_value());

    // Texture shipped as DDS under a .png name (New Classic) plus an alpha mask, inside a
    // smaller-than-header image: exercises name fallback, mask and padding together.
    std::vector<std::array<std::uint8_t, 4>> px(32 * 32, {200, 100, 50, 255});
    std::vector<std::array<std::uint8_t, 4>> maskPx(32 * 32, {0, 0, 90, 255});
    const Bytes pbg3 = buildPbg3({{"etama3.anm", anmBytes},
                                  {"etama3.dds", buildDdsArgb(32, 32, px)},
                                  {"etama3_a.dds", buildDdsArgb(32, 32, maskPx)},
                                  {"shared.txt", Bytes{'c', 'l', 'a', 's', 's', 'i', 'c'}}});
    const Bytes pkgl = buildPkgl("th06CM", {{"shared.txt", Bytes{'n', 'c'}, false, 5u},
                                            {"only_nc.txt", Bytes{'x'}, true, 6u}});

    Vfs vfs;
    CHECK(vfs.mount(open("KOUMAKYO_CM.dat", pbg3)));
    CHECK(vfs.mount(open("th06CM.dat", pkgl)));
    CHECK(vfs.fileCount() == 5);
    CHECK(Vfs::normalize("data\\Sub/ETAMA3.ANM") == "etama3.anm");
    auto shared = vfs.read("data/SHARED.TXT");
    CHECK(shared && *shared == Bytes({'c', 'l', 'a', 's', 's', 'i', 'c'}));  // first mount wins
    CHECK(vfs.read("only_nc.txt").has_value());
    CHECK(!vfs.read("missing.png").has_value());

    TextureLibrary lib;
    auto loaded = loadAnm(vfs, lib, "data/etama3.anm", &err);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(lib.width(loaded->texture) == 64);
        const SpriteRegion r = loaded->sprite(lib, 1);
        CHECK(r.u0 == 0.25f && r.u1 == 0.5f && r.width == 16.0f);
        auto img = lib.build(loaded->texture);
        CHECK(img && img->width == 64 && img->height == 64);
        if (img) {
            CHECK(std::memcmp(img->pixel(0, 0), "\xc8\x64\x32\x5a", 4) == 0);  // alpha from mask
            CHECK(img->pixel(40, 40)[3] == 0);                                 // padding
        }
    }
}

static void testGameArtFromArchives() {
    // Like the real etama3.anm: 16x16 item sprites come before the bullets, so picking
    // "any 16x16 sprite" would show items. Script 3 (ball bullet) starts at sprite 40.
    std::vector<AnmSprite> etamaSprites;
    for (std::uint32_t i = 0; i < 20; ++i) etamaSprites.push_back({i, static_cast<float>(i % 16) * 16, 0, 16, 16});
    for (std::uint32_t c = 0; c < 16; ++c) etamaSprites.push_back({40 + c, static_cast<float>(c) * 16, 16, 16, 16});
    const Bytes etama = buildAnm(256, 64, "data/etama3.png", "", 0, etamaSprites,
                                 {{0, 0}, {1, 4}, {3, 40}, {21, 10}});
    // Player: idle script 0 points at sprite 6 (32x48); other sprites are other sizes.
    const std::vector<AnmSprite> playerSprites = {{0, 0, 0, 16, 16}, {6, 32, 0, 32, 48}, {7, 64, 0, 32, 48}};
    const Bytes player = buildAnm(128, 64, "data/player00.png", "data/player00_a.png", 0, playerSprites,
                                  {{1, 7}, {0, 6}});
    std::vector<std::array<std::uint8_t, 4>> px(64 * 64, {255, 255, 255, 255});
    const Bytes texture = buildDdsArgb(64, 64, px);

    Vfs vfs;
    CHECK(vfs.mount(open("KOUMAKYO_CM.dat", buildPbg3({{"etama3.anm", etama},
                                                       {"etama3.dds", texture},
                                                       {"player00.anm", player},
                                                       {"player00.dds", texture}}))));
    TextureLibrary lib;
    const GameArt art = buildGameArt(&vfs, lib);
    CHECK(art.bulletsFromGameData && art.bulletsNote.empty());
    CHECK(art.bullets.size() == 16);
    if (art.bullets.size() == 16) {
        CHECK(art.bullets[0].v0 == 16.0f / 64.0f);  // second row = the ball sprites, not items
        CHECK(art.bullets[15].u0 == 240.0f / 256.0f);
    }
    CHECK(art.playerFromGameData && art.playerNote.empty());
    CHECK(art.player.u0 == 32.0f / 128.0f && art.player.height == 48.0f);  // script 0's sprite, not sprite 7

    // Missing player file: falls back with a readable reason for the HUD.
    Vfs partial;
    partial.mount(open("KOUMAKYO_CM.dat", buildPbg3({{"etama3.anm", etama}, {"etama3.dds", texture}})));
    TextureLibrary lib2;
    const GameArt fallback = buildGameArt(&partial, lib2);
    CHECK(fallback.bulletsFromGameData);
    CHECK(!fallback.playerFromGameData && fallback.playerNote == "PLAYER00.ANM NOT FOUND");
}

static void testMappedFileAndDirectory() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("th06_test_" + std::to_string(::getpid()));
    fs::create_directories(dir / "data");
    const Bytes pbg3 = buildPbg3({{"a.txt", Bytes{'p', 'b', 'g'}}});
    const Bytes pkgl = buildPkgl("th06TL", {{"a.txt", Bytes{'p', 'k', 'g'}, true, 1u}});
    std::ofstream(dir / "紅魔郷TL.DAT", std::ios::binary).write(reinterpret_cast<const char*>(pbg3.data()), static_cast<std::streamsize>(pbg3.size()));
    std::ofstream(dir / "data" / "th06TL.dat", std::ios::binary).write(reinterpret_cast<const char*>(pkgl.data()), static_cast<std::streamsize>(pkgl.size()));
    Bytes padding(2048, 0);  // >= 1 KiB but not an archive: must be ignored
    std::ofstream(dir / "score.dat", std::ios::binary).write(reinterpret_cast<const char*>(padding.data()), 2048);

    std::string err;
    auto mapped = MappedFile::open((dir / "紅魔郷TL.DAT").string(), &err);
    CHECK(mapped && mapped->isMapped() && mapped->bytes().size() == pbg3.size());

    int fds[2];
    CHECK(::pipe(fds) == 0);
    CHECK(::write(fds[1], pbg3.data(), pbg3.size()) == static_cast<ssize_t>(pbg3.size()));
    ::close(fds[1]);
    auto piped = MappedFile::adoptFd(fds[0], &err);
    CHECK(piped && !piped->isMapped());
    CHECK(piped && Bytes(piped->bytes().begin(), piped->bytes().end()) == pbg3);

    Vfs classicFirst;
    CHECK(classicFirst.mountDirectory(dir.string(), false) == 2);
    auto a = classicFirst.read("a.txt");
    CHECK(a && *a == Bytes({'p', 'b', 'g'}));
    Vfs ncFirst;
    CHECK(ncFirst.mountDirectory(dir.string(), true) == 2);
    auto b = ncFirst.read("a.txt");
    CHECK(b && *b == Bytes({'p', 'k', 'g'}));
    fs::remove_all(dir);
}

static void testObjectPool() {
    struct Obj {
        int value;
        int pad[7];
    };
    using Pool = ObjectPool<Obj, 1000>;
    auto pool = std::make_unique<Pool>();

    Pool::Handle h1;
    CHECK(pool->spawnTracked(h1, Obj{1, {}}) != nullptr);
    CHECK(pool->release(h1));
    CHECK(!pool->release(h1));  // double release is rejected
    Pool::Handle h2;
    pool->spawnTracked(h2, Obj{2, {}});
    CHECK(h2.slot == h1.slot);  // slot reused...
    CHECK(pool->get(h1) == nullptr && pool->get(h2) != nullptr);  // ...old handle is stale
    pool->clear();
    CHECK(pool->empty() && !pool->alive(h2));

    for (int i = 0; i < 1000; ++i) CHECK(pool->spawn(Obj{i, {}}) != nullptr);
    CHECK(pool->full() && pool->spawn(Obj{-1, {}}) == nullptr);
    pool->clear();

    // Randomised comparison against a std::map model.
    std::map<int, Pool::Handle> model;
    Rng rng(12345);
    int next = 0;
    bool consistent = true;
    for (int step = 0; step < 200000; ++step) {
        const std::uint32_t r = rng.next() % 100;
        if (r < 55) {
            Pool::Handle h;
            if (pool->spawnTracked(h, Obj{next, {}})) {
                model[next] = h;
            } else {
                consistent = consistent && model.size() == Pool::capacity();
            }
            ++next;
        } else if (r < 92) {
            if (model.empty()) continue;
            auto it = model.begin();
            std::advance(it, static_cast<long>(rng.next() % model.size()));
            const Pool::Handle h = it->second;
            consistent = consistent && pool->release(h) && !pool->alive(h);
            model.erase(it);
        } else {
            const int k = static_cast<int>(rng.next() % 5);
            const std::size_t removed = pool->removeIf([k](Obj& o) { return o.value % 5 == k; });
            std::size_t expected = 0;
            for (auto it = model.begin(); it != model.end();) {
                if (it->first % 5 == k) {
                    it = model.erase(it);
                    ++expected;
                } else {
                    ++it;
                }
            }
            consistent = consistent && removed == expected;
        }
        if (step % 997 == 0) {
            consistent = consistent && pool->size() == model.size();
            long long poolSum = 0, modelSum = 0;
            pool->forEach([&](Obj& o) { poolSum += o.value; });
            for (const auto& [v, h] : model) {
                modelSum += v;
                const Obj* o = pool->get(h);
                consistent = consistent && o && o->value == v;
            }
            consistent = consistent && poolSum == modelSum;
        }
    }
    CHECK(consistent);
}

namespace {
struct CountingSound final : SoundSink {
    int plays = 0;
    void play(Sfx) override { ++plays; }
};
struct CountingSink final : SpriteSink {
    std::size_t sprites = 0;
    void draw(std::uint16_t, Blend, const SpriteInstance&) override { ++sprites; }
};
}  // namespace

static Game::Stats simulate(int ticks, int* soundPlays, std::size_t* rendered, std::size_t* peakFromOutside) {
    TextureLibrary lib;
    CountingSound sound;
    auto game = std::make_unique<Game>(buildGameArt(nullptr, lib), &sound);
    std::size_t peak = 0;
    for (int t = 0; t < ticks; ++t) {
        InputState in;
        in.shoot = true;
        in.left = (t / 90) % 2 == 0;
        in.right = !in.left;
        in.up = (t / 200) % 3 == 0;
        in.focus = (t / 300) % 2 == 1;
        in.bomb = t == 700;
        game->tick(in);
        peak = std::max(peak, game->stats().bullets);
    }
    CountingSink sink;
    game->render(sink, HudInfo{60.0f, 3});
    if (soundPlays) *soundPlays = sound.plays;
    if (rendered) *rendered = sink.sprites;
    if (peakFromOutside) *peakFromOutside = peak;
    return game->stats();
}

static void testBulletCapacity() {
    auto bm = std::make_unique<BulletManager>();
    for (int i = 0; i < 200; ++i) bm->ring({192, 100}, 64, 0.0f, 5.0f, 0);  // 12800 requests
    CHECK(bm->size() == BulletManager::kCapacity);
    CHECK(bm->dropped() == 12800 - BulletManager::kCapacity);
    const auto r = bm->tick({192, 100}, 1.5f, 16.0f, true);  // invulnerable: no hits/grazes
    CHECK(!r.hit && r.grazes == 0 && bm->size() == BulletManager::kCapacity);
    for (int t = 0; t < 200; ++t) bm->tick({0, 0}, 1.5f, 16.0f, true);
    CHECK(bm->size() == 0);  // everything left the field and was culled
    bm->ring({192, 100}, 10, 0.0f, 1.0f, 0);
    CHECK(bm->size() == 10);  // pool fully reusable after saturation
}

static void testGameSimulation() {
    std::string err;
    CHECK(fontSelfTest(&err));

    TextureLibrary lib;
    const GameArt art = buildGameArt(nullptr, lib);
    CHECK(art.bullets.size() == 8 && !art.bulletsFromGameData);
    for (std::uint16_t id = 0; id < lib.size(); ++id) {
        auto img = lib.build(id);
        CHECK(img && img->width == lib.width(id) && img->height == lib.height(id));
    }

    int plays = 0;
    std::size_t rendered = 0, peak = 0;
    const Game::Stats s = simulate(4 * 600 + 120, &plays, &rendered, &peak);
    std::printf("  simulation: %llu ticks, peak %zu bullets, grazes %u, hits %u, bombs %u, %d sounds, %zu sprites drawn\n",
                static_cast<unsigned long long>(s.ticks), s.peakBullets, s.grazes, s.hits, s.bombs, plays, rendered);
    CHECK(s.ticks == 4 * 600 + 120);
    CHECK(s.peakBullets == peak);
    CHECK(s.peakBullets > 2000);  // density phase actually stresses the pool
    CHECK(s.dropped == 0);        // ...without ever exceeding it
    CHECK(s.bombs == 1);
    CHECK(plays > 0);
    CHECK(rendered > s.bullets);

    const Game::Stats again = simulate(4 * 600 + 120, nullptr, nullptr, nullptr);
    CHECK(again.bullets == s.bullets && again.grazes == s.grazes && again.hits == s.hits &&
          again.peakBullets == s.peakBullets);  // deterministic
}

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"bit reader / varints", testBitReader},
        {"LZSS round trip", testLzss},
        {"PBG3 archives (2002)", testPbg3},
        {"PKGL archives (New Classic)", testPkgl},
        {"image decoding (PNG, DDS, BC1, colour key, mask)", testImages},
        {"ANM sprite tables + VFS", testAnmAndVfs},
        {"sprite choice via ANM scripts (bullets, player)", testGameArtFromArchives},
        {"mmap / fd fallback / directory mounting", testMappedFileAndDirectory},
        {"object pool vs reference model", testObjectPool},
        {"bullet pool saturation and reuse", testBulletCapacity},
        {"headless game simulation", testGameSimulation},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-55s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
