// Host tests for the engine core: archive formats and decoders, the ANM and ECL virtual
// machines on hand-assembled bytecode, bullets, the sound queue, the fixed-timestep clock
// and a deterministic headless run of a whole stage built from a synthetic archive.
// No GPU, no SDL, no game assets.
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
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

#include "audio/SoundPlayer.hpp"
#include "core/FixedTimestep.hpp"
#include "core/Math.hpp"
#include "core/Sha256.hpp"
#include "core/ZunMath.hpp"
#include "game/Bomb.hpp"
#include "game/BulletManager.hpp"
#include "game/Ecl.hpp"
#include "game/EffectManager.hpp"
#include "game/EnemyManager.hpp"
#include "game/ItemManager.hpp"
#include "game/Player.hpp"
#include "game/Resources.hpp"
#include "game/ScreenShake.hpp"
#include "game/ShotData.hpp"
#include "game/Stage.hpp"
#include "gfx/Anm.hpp"
#include "gfx/AnmManager.hpp"
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

// ---- engine test data: hand-assembled ANM / ECL bytecode -----------------------------

static std::uint32_t fbits(float f) { return std::bit_cast<std::uint32_t>(f); }

// ANM instruction: {i16 time, u8 opcode, u8 argBytes, u32 args...}.
class AnmCode {
  public:
    std::uint32_t ins(std::int16_t time, std::uint8_t op, std::initializer_list<std::uint32_t> args = {}) {
        const auto at = static_cast<std::uint32_t>(bytes.size());
        putLe<std::int16_t>(bytes, time);
        bytes.push_back(op);
        bytes.push_back(static_cast<std::uint8_t>(4 * args.size()));
        for (const std::uint32_t a : args) putLe<std::uint32_t>(bytes, a);
        return at;
    }
    Bytes bytes;
};

struct AnmScriptBytes {
    std::uint32_t id;
    Bytes code;
};

// ANM v0 file with real bytecode: header, sprite offsets, script table, sprites, code, name.
static Bytes buildAnmScripts(int texW, int texH, const std::string& image, const std::vector<AnmSprite>& sprites,
                             const std::vector<AnmScriptBytes>& scripts) {
    const std::size_t n = sprites.size(), m = scripts.size();
    const std::size_t spritesAt = 0x40 + 4 * n + 8 * m;
    Bytes b(spritesAt + 20 * n, 0);
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put32(0x00, static_cast<std::uint32_t>(n));
    put32(0x04, static_cast<std::uint32_t>(m));
    put32(0x0C, static_cast<std::uint32_t>(texW));
    put32(0x10, static_cast<std::uint32_t>(texH));
    put32(0x14, 1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t sp = spritesAt + 20 * i;
        put32(0x40 + 4 * i, static_cast<std::uint32_t>(sp));
        put32(sp, sprites[i].id);
        const float f[4] = {sprites[i].x, sprites[i].y, sprites[i].w, sprites[i].h};
        std::memcpy(b.data() + sp + 4, f, 16);
    }
    for (std::size_t k = 0; k < m; ++k) {
        const std::size_t table = 0x40 + 4 * n + 8 * k;
        put32(table, scripts[k].id);
        put32(table + 4, static_cast<std::uint32_t>(b.size()));
        b.insert(b.end(), scripts[k].code.begin(), scripts[k].code.end());
    }
    put32(0x1C, static_cast<std::uint32_t>(b.size()));
    b.insert(b.end(), image.begin(), image.end());
    b.push_back(0);
    return b;
}

// ECL variable ids.
enum : std::int32_t {
    kI0 = -10001, kI1 = -10002, kI2 = -10003, kI3 = -10004, kF0 = -10005, kF1 = -10006, kF2 = -10007,
    kEnemyLife = -10024,
};

// ECL file: header, sub table, timeline 0, subs. Instructions are
// {i32 time, i16 opcode, i16 size, u8 unknown, u8 difficultyMask, u16 unknown, u32 args...}.
class EclBuilder {
  public:
    void spawn(std::int16_t time, std::int16_t op, std::int16_t sub, float x, float y, std::int16_t life,
               std::int16_t item, std::int32_t score) {
        putLe<std::int16_t>(timeline_, time);
        putLe<std::int16_t>(timeline_, sub);
        putLe<std::int16_t>(timeline_, op);
        putLe<std::int16_t>(timeline_, 28);
        putLe<std::uint32_t>(timeline_, fbits(x));
        putLe<std::uint32_t>(timeline_, fbits(y));
        putLe<std::uint32_t>(timeline_, fbits(0.0f));
        putLe<std::int16_t>(timeline_, life);
        putLe<std::int16_t>(timeline_, item);
        putLe<std::int32_t>(timeline_, score);
    }
    int sub() {
        subs_.emplace_back();
        return static_cast<int>(subs_.size()) - 1;
    }
    std::uint32_t here() const { return static_cast<std::uint32_t>(subs_.back().size()); }
    // Jump offset for the instruction about to be emitted: `target` relative to its start.
    std::uint32_t to(std::uint32_t target) const { return target - here(); }
    std::uint32_t ins(std::int32_t time, std::int16_t op, std::initializer_list<std::uint32_t> args = {},
                      std::uint8_t difficultyMask = 0xff) {
        Bytes& b = subs_.back();
        const auto at = static_cast<std::uint32_t>(b.size());
        putLe<std::int32_t>(b, time);
        putLe<std::int16_t>(b, op);
        putLe<std::int16_t>(b, static_cast<std::int16_t>(12 + 4 * args.size()));
        b.push_back(0);
        b.push_back(difficultyMask);
        putLe<std::uint16_t>(b, 0);
        for (const std::uint32_t a : args) putLe<std::uint32_t>(b, a);
        return at;
    }
    Bytes build() const {
        Bytes out(16 + 4 * subs_.size(), 0);
        auto put32 = [&](std::size_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) out[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
        };
        out[0] = static_cast<std::uint8_t>(subs_.size());
        out[2] = 1;  // mainCount
        put32(4, static_cast<std::uint32_t>(out.size()));
        out.insert(out.end(), timeline_.begin(), timeline_.end());
        putLe<std::int16_t>(out, -1);  // terminator
        putLe<std::int16_t>(out, 0);
        putLe<std::int16_t>(out, 0);
        putLe<std::int16_t>(out, 8);
        for (std::size_t i = 0; i < subs_.size(); ++i) {
            put32(16 + 4 * i, static_cast<std::uint32_t>(out.size()));
            out.insert(out.end(), subs_[i].begin(), subs_[i].end());
        }
        return out;
    }

  private:
    Bytes timeline_;
    std::vector<Bytes> subs_;
};

// EclRawInstrBulletArgs as eight words.
struct BulletWords {
    std::uint32_t w[8];
};
static BulletWords bulletWords(std::int16_t sprite, std::int16_t color, std::int32_t count1, std::int32_t count2,
                               float speed1, float speed2, float angle1, float angle2, std::uint32_t flags) {
    return {{static_cast<std::uint16_t>(sprite) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(color)) << 16),
             static_cast<std::uint32_t>(count1), static_cast<std::uint32_t>(count2), fbits(speed1), fbits(speed2),
             fbits(angle1), fbits(angle2), flags}};
}
#define BW(bw) {bw.w[0], bw.w[1], bw.w[2], bw.w[3], bw.w[4], bw.w[5], bw.w[6], bw.w[7]}

// EclRawInstrLaserArgs as thirteen words.
struct LaserWords {
    std::uint32_t w[13];
};
static LaserWords laserWords(std::int16_t sprite, std::int16_t color, float angle, float speed, float startOffset,
                             float endOffset, float startLength, float width, std::int32_t startTime,
                             std::int32_t duration, std::int32_t despawnDuration, std::int32_t hitboxStartTime,
                             std::int32_t hitboxEndDelay, std::uint32_t flags) {
    return {{static_cast<std::uint16_t>(sprite) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(color)) << 16),
             fbits(angle), fbits(speed), fbits(startOffset), fbits(endOffset), fbits(startLength), fbits(width),
             static_cast<std::uint32_t>(startTime), static_cast<std::uint32_t>(duration),
             static_cast<std::uint32_t>(despawnDuration), static_cast<std::uint32_t>(hitboxStartTime),
             static_cast<std::uint32_t>(hitboxEndDelay), flags}};
}
#define LW(lw) {lw.w[0], lw.w[1], lw.w[2], lw.w[3], lw.w[4], lw.w[5], lw.w[6], lw.w[7], lw.w[8], lw.w[9], lw.w[10], lw.w[11], lw.w[12]}

// ---- synthetic game data (generated shapes, no game assets) ---------------------------

static void paintDisc(Image& img, int x0, int y0, int size, std::array<std::uint8_t, 3> c) {
    const float r = size * 0.5f - 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x - r, dy = y - r;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > r) continue;
            std::uint8_t* p = img.pixel(x0 + x, y0 + y);
            const bool core = d < r * 0.45f;
            p[0] = core ? 255 : c[0];
            p[1] = core ? 255 : c[1];
            p[2] = core ? 255 : c[2];
            p[3] = 255;
        }
    }
}
static void paintBox(Image& img, int x0, int y0, int w, int h, std::array<std::uint8_t, 4> c) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) std::memcpy(img.pixel(x0 + x, y0 + y), c.data(), 4);
    }
}
static Bytes ddsFromImage(const Image& img) {
    std::vector<std::array<std::uint8_t, 4>> px;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const std::uint8_t* p = img.pixel(x, y);
            px.push_back({p[0], p[1], p[2], p[3]});
        }
    }
    return buildDdsArgb(img.width, img.height, px);
}
constexpr std::array<std::array<std::uint8_t, 3>, 8> kTestColors = {{
    {200, 200, 200}, {230, 40, 60}, {240, 140, 30}, {240, 220, 40}, {60, 200, 80}, {40, 200, 220}, {60, 90, 240}, {170, 60, 230},
}};

// etama3.anm, laid out like the real file where the engine hard-codes sprite numbers:
//   sprites 0..6 items, 7..13 their above-screen indicators (scripts 21..27)
//   16 + type*8 + colour: 9 bullet types x 8 colours (scripts 0..8; pellets are 8x8)
//   88..137 spawn effects (scripts 11..20), 140..144 the big spawn glow (laser origins),
//   146..153 line laser colours (script 9), 154..161 beam colours (script 10).
static Bytes syntheticEtama3() {
    std::vector<AnmSprite> sprites;
    for (std::uint32_t i = 0; i < 14; ++i) sprites.push_back({i, i * 16.0f, 0, 16, 16});
    for (std::uint32_t t = 0; t < 9; ++t) {
        const float size = t == 0 ? 8.0f : 16.0f;
        for (std::uint32_t c = 0; c < 8; ++c) sprites.push_back({16 + t * 8 + c, c * 16.0f, 16 + t * 16.0f, size, size});
    }
    for (std::uint32_t j = 0; j < 50; ++j) sprites.push_back({88 + j, (j % 8) * 16.0f, 32, 16, 16});
    for (std::uint32_t k = 0; k < 5; ++k) sprites.push_back({140 + k, 128 + k * 16.0f, 16, 16, 16});
    for (std::uint32_t c = 0; c < 8; ++c) sprites.push_back({146 + c, 128 + c * 16.0f, 32, 16, 16});
    for (std::uint32_t c = 0; c < 8; ++c) sprites.push_back({154 + c, 128 + c * 16.0f, 48, 16, 16});
    std::vector<AnmScriptBytes> scripts;
    for (std::uint32_t t = 0; t < 9; ++t) {
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {16 + t * 8});
        c.ins(0, anm_op::Static);
        scripts.push_back({t, c.bytes});
    }
    for (std::uint32_t k = 0; k < 2; ++k) {  // 9 line laser, 10 beam
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {k == 0 ? 146u : 154u});
        c.ins(1, anm_op::Halt);
        scripts.push_back({9 + k, c.bytes});
    }
    for (std::uint32_t s = 11; s <= 20; ++s) {
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {88 + 5 * (s - 11)});
        c.ins(0, anm_op::BlendAdditive);
        c.ins(0, anm_op::AlphaInterpLinear, {0, 8});
        c.ins(8, anm_op::Delete);
        scripts.push_back({s, c.bytes});
    }
    for (std::uint32_t type = 0; type < 7; ++type) {  // items
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {type});
        if (type == 6) c.ins(0, anm_op::RotationSpeed, {fbits(0.0f), fbits(0.0f), fbits(0.2f)});
        c.ins(1, anm_op::Halt);
        scripts.push_back({21 + type, c.bytes});
    }
    return buildAnmScripts(256, 256, "data/etama3.dds", sprites, scripts);
}
static Bytes syntheticEtama3Texture() {
    Image img(256, 160);
    constexpr std::array<std::array<std::uint8_t, 3>, 7> kItemColors = {{
        {230, 40, 50}, {60, 110, 240}, {230, 40, 50}, {60, 200, 80}, {240, 210, 40}, {240, 120, 200}, {250, 250, 250},
    }};
    for (int i = 0; i < 14; ++i) {
        const auto& c = kItemColors[static_cast<std::size_t>(i % 7)];
        const int inset = (i % 7 == 0 || i % 7 == 6) ? 4 : 1;  // small power and stars are smaller
        paintBox(img, i * 16 + inset, inset, 16 - 2 * inset, 16 - 2 * inset, {c[0], c[1], c[2], i >= 7 ? std::uint8_t(160) : std::uint8_t(255)});
    }
    for (int t = 0; t < 9; ++t) {
        for (int c = 0; c < 8; ++c) paintDisc(img, c * 16, 16 + t * 16, t == 0 ? 8 : 16, kTestColors[static_cast<std::size_t>(c)]);
    }
    for (int k = 0; k < 5; ++k) paintDisc(img, 128 + k * 16, 16, 16, kTestColors[static_cast<std::size_t>(k)]);
    for (int row = 0; row < 2; ++row) {
        for (int c = 0; c < 8; ++c) {  // lasers: bright core fading to the colour at the edges
            const auto& col = kTestColors[static_cast<std::size_t>(c)];
            for (int x = 0; x < 16; ++x) {
                const float core = 1.0f - std::fabs(x - 7.5f) / 8.0f;
                const auto mix = [&](int v) { return static_cast<std::uint8_t>(v + (255 - v) * core * core); };
                for (int y = 0; y < 16; ++y) {
                    std::uint8_t* p = img.pixel(128 + c * 16 + x, 32 + row * 16 + y);
                    p[0] = mix(col[0]);
                    p[1] = mix(col[1]);
                    p[2] = mix(col[2]);
                    p[3] = static_cast<std::uint8_t>(255 * std::min(1.0f, core * 2.0f));
                }
            }
        }
    }
    return ddsFromImage(img);
}
// etama4.anm: bubble bullets (32x32, 8 colours) and two spawn effects.
static Bytes syntheticEtama4() {
    std::vector<AnmSprite> sprites;
    for (std::uint32_t c = 0; c < 8; ++c) sprites.push_back({c, (c % 4) * 32.0f, (c / 4) * 32.0f, 32, 32});
    std::vector<AnmScriptBytes> scripts;
    AnmCode bubble;
    bubble.ins(0, anm_op::SetSprite, {0});
    bubble.ins(0, anm_op::Static);
    scripts.push_back({0, bubble.bytes});
    for (std::uint32_t s = 1; s <= 2; ++s) {
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {0});
        c.ins(6, anm_op::Delete);
        scripts.push_back({s, c.bytes});
    }
    // Effects (local scripts 3..19): lifetimes chosen for the tests; the orbit (18) waits
    // forever like a spell circle.
    for (std::uint32_t s = 3; s <= 19; ++s) {
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {s % 8});
        c.ins(0, anm_op::BlendAdditive);
        if (s == 18) {
            c.ins(1, anm_op::Halt);
        } else {
            const std::int16_t life = s == 6 ? 30 : s == 7 ? 70 : s == 8 ? 250 : s == 19 ? 120 : (s >= 9 && s <= 16) ? 20 : 16;
            c.ins(life, anm_op::Delete);
        }
        scripts.push_back({s, c.bytes});
    }
    return buildAnmScripts(256, 256, "data/etama4.dds", sprites, scripts);
}
static Bytes syntheticEtama4Texture() {
    Image img(128, 64);
    for (int c = 0; c < 8; ++c) paintDisc(img, (c % 4) * 32, (c / 4) * 32, 32, kTestColors[static_cast<std::size_t>(c)]);
    return ddsFromImage(img);
}
// player00.anm: idle/moving 0..4, Marisa bomb 5..11, shots 64..71, hits 96..100, orbs 128/129,
// Reimu bombs 133..140.
static Bytes syntheticPlayer() {
    const std::vector<AnmSprite> sprites = {{0, 0, 0, 32, 48}, {1, 32, 0, 16, 16}, {2, 48, 0, 16, 16}};
    std::vector<AnmScriptBytes> scripts;
    for (std::uint32_t s = 0; s <= 4; ++s) {
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {0});
        c.ins(0, anm_op::Static);
        scripts.push_back({s, c.bytes});
    }
    AnmCode shot;
    shot.ins(0, anm_op::SetSprite, {1});
    shot.ins(0, anm_op::Alpha, {0xa0});
    shot.ins(1, anm_op::Halt);  // a finished (static) script would retire the shot
    scripts.push_back({64, shot.bytes});
    for (std::uint32_t k = 65; k <= 68; ++k) {  // amulets / needles / missiles
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {1});
        c.ins(1, anm_op::Halt);
        scripts.push_back({k, c.bytes});
    }
    for (std::uint32_t k = 69; k <= 71; ++k) {  // lasers: shown until interrupt 1, then gone
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {1});
        c.ins(0, anm_op::Halt);
        c.ins(0, anm_op::InterruptLabel, {1});
        c.ins(0, anm_op::Delete);
        scripts.push_back({k, c.bytes});
    }
    for (std::uint32_t k = 96; k <= 100; ++k) {  // hit / explosion animations
        AnmCode hit;
        hit.ins(0, anm_op::SetSprite, {2});
        hit.ins(0, anm_op::BlendAdditive);
        hit.ins(0, anm_op::ScaleSpeed, {fbits(0.1f), fbits(0.1f)});
        hit.ins(6, anm_op::Delete);
        scripts.push_back({k, hit.bytes});
    }
    for (std::uint32_t k = 5; k <= 11; ++k) {  // Marisa's bomb stars (5..7) and Master Spark beams (8..11)
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {k <= 7 ? 2u : 1u});
        c.ins(1, anm_op::Halt);
        scripts.push_back({k, c.bytes});
    }
    for (std::uint32_t k = 133; k <= 136; ++k) {  // Reimu A bomb orb layers: explode on interrupt 1
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {2});
        c.ins(1, anm_op::Halt);
        c.ins(1, anm_op::InterruptLabel, {1});
        c.ins(1, anm_op::BlendAdditive);
        c.ins(29, anm_op::Delete);
        scripts.push_back({k, c.bytes});
    }
    for (std::uint32_t k = 137; k <= 140; ++k) {  // Reimu B beams; the first slides right through its offset
        AnmCode c;
        c.ins(0, anm_op::PositionMode, {1});
        c.ins(0, anm_op::SetSprite, {1});
        if (k == 137) c.ins(0, anm_op::MovePositionInterpLinear, {fbits(60.0f), fbits(0.0f), fbits(0.0f), 60});
        c.ins(1, anm_op::Halt);
        scripts.push_back({k, c.bytes});
    }
    for (std::uint32_t k = 128; k <= 129; ++k) {  // option orbs
        AnmCode c;
        c.ins(0, anm_op::SetSprite, {2});
        c.ins(0, anm_op::RotationSpeed, {fbits(0.0f), fbits(0.0f), fbits(0.1f)});
        c.ins(1, anm_op::Halt);
        scripts.push_back({k, c.bytes});
    }
    return buildAnmScripts(256, 256, "data/player00.dds", sprites, scripts);
}
static Bytes syntheticPlayerTexture() {
    Image img(64, 48);
    paintBox(img, 6, 4, 20, 40, {250, 240, 245, 255});
    paintBox(img, 0, 16, 6, 24, {200, 30, 50, 255});
    paintBox(img, 26, 16, 6, 24, {200, 30, 50, 255});
    paintBox(img, 14, 20, 4, 4, {200, 30, 50, 255});
    paintBox(img, 36, 0, 8, 16, {255, 180, 210, 255});
    paintDisc(img, 48, 0, 16, {255, 160, 200});
    return ddsFromImage(img);
}
// stg1enm.anm (offset 0x100): fairy (2-frame loop, script 0), spinner (script 1).
static Bytes syntheticEnemy() {
    const std::vector<AnmSprite> sprites = {{0, 0, 0, 32, 32}, {1, 32, 0, 32, 32}, {2, 64, 0, 64, 64}};
    std::vector<AnmScriptBytes> scripts;
    AnmCode fairy;
    const std::uint32_t top = fairy.ins(0, anm_op::SetSprite, {0});
    fairy.ins(8, anm_op::SetSprite, {1});
    fairy.ins(16, anm_op::Jump, {top});
    scripts.push_back({0, fairy.bytes});
    AnmCode spinner;
    spinner.ins(0, anm_op::SetSprite, {2});
    spinner.ins(0, anm_op::RotationSpeed, {fbits(0.0f), fbits(0.0f), fbits(0.05f)});
    spinner.ins(1, anm_op::Halt);  // halted VMs keep rotating; a static one would freeze
    scripts.push_back({1, spinner.bytes});
    return buildAnmScripts(256, 256, "data/stg1enm.dds", sprites, scripts);
}
static Bytes syntheticEnemyTexture() {
    Image img(128, 64);
    paintDisc(img, 0, 0, 32, {90, 140, 255});
    paintDisc(img, 32, 0, 32, {120, 170, 255});
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (std::abs(x - 32) + std::abs(y - 32) <= 30) std::memcpy(img.pixel(64 + x, y), "\xf0\x60\xa0\xff", 4);
        }
    }
    return ddsFromImage(img);
}
static Bytes syntheticEnemy2() {
    const std::vector<AnmSprite> sprites = {{64, 0, 0, 16, 16}};
    AnmCode c;
    c.ins(0, anm_op::SetSprite, {64});
    c.ins(0, anm_op::Static);
    return buildAnmScripts(256, 256, "data/stg1enm2.dds", sprites, {{64, c.bytes}});
}

// ecldata1.ecl: ten fairies firing aimed fans and rings, then a spinner shooting a
// rotating 12-way ring and aimed big bullets until it is shot down.
static Bytes syntheticStageEcl() {
    EclBuilder e;
    for (int k = 0; k < 10; ++k) {
        e.spawn(static_cast<std::int16_t>(20 + 50 * k), k % 2 ? timeline_op::EnemyCreateMirror : timeline_op::EnemyCreate,
                0, k % 2 ? 288.0f : 96.0f, -8.0f, 40, -1, 300);
    }
    e.spawn(600, timeline_op::EnemyCreate, 1, 192.0f, -24.0f, 1500, -1, 10000);

    e.sub();  // 0: fairy
    e.ins(0, ecl_op::AnmSetMain, {0});
    e.ins(0, ecl_op::MoveAxisSpeed, {fbits(0.0f), fbits(1.2f), fbits(0.0f)});
    const BulletWords fan = bulletWords(3, 1, 3, 2, 3.0f, 2.0f, 0.0f, 0.3f, ExSpawnEffectShort);
    e.ins(40, ecl_op::BulletFanAimed, BW(fan));
    e.ins(80, ecl_op::MoveAxisSpeed, {fbits(1.5f), fbits(0.6f), fbits(0.0f)});
    const BulletWords ring = bulletWords(1, 5, 8, 1, 2.0f, 1.0f, 0.0f, 0.0f, 0);
    e.ins(100, ecl_op::BulletCircle, BW(ring));
    e.ins(400, ecl_op::EnemyDelete);

    e.sub();  // 1: spinner
    e.ins(0, ecl_op::AnmSetMain, {1});
    e.ins(0, ecl_op::MoveAxisSpeed, {fbits(0.0f), fbits(2.0f), fbits(0.0f)});
    const LaserWords down = laserWords(0, 5, kZunHalfPi, 8.0f, 0.0f, 0.0f, 400.0f, 24.0f, 40, 120, 24, 30, 12, 0);
    e.ins(20, ecl_op::LaserCreate, LW(down));
    e.ins(30, ecl_op::LaserIndex, {1});
    const LaserWords aimed = laserWords(1, 1, 0.35f, 6.0f, 0.0f, 0.0f, 300.0f, 16.0f, 30, 90, 20, 20, 10, 0);
    e.ins(30, ecl_op::LaserCreateAimed, LW(aimed));
    e.ins(40, ecl_op::MoveAxisSpeed, {fbits(0.0f), fbits(0.0f), fbits(0.0f)});
    e.ins(40, ecl_op::DropItems, {4});
    const std::uint32_t loop = e.ins(40, ecl_op::SetInt, {static_cast<std::uint32_t>(kI0), 0});
    const BulletWords spin = bulletWords(0, 3, 12, 1, 1.8f, 1.0f, static_cast<float>(kF0), 0.0f, 0);
    e.ins(40, ecl_op::BulletCircle, BW(spin));
    e.ins(40, ecl_op::MathFloatAdd, {static_cast<std::uint32_t>(kF0), fbits(static_cast<float>(kF0)), fbits(0.2f)});
    const BulletWords big = bulletWords(6, 1, 3, 1, 2.5f, 1.0f, 0.0f, 0.3f, ExSpawnEffect);
    e.ins(44, ecl_op::BulletFanAimed, BW(big));
    e.ins(48, ecl_op::Jump, {40, e.to(loop)});
    return e.build();
}

static Bytes syntheticStageArchive() {
    return buildPbg3({{"player00.anm", syntheticPlayer()},
                      {"player01.anm", syntheticPlayer()},  // Marisa: same synthetic art
                      {"player00.dds", syntheticPlayerTexture()},
                      {"etama3.anm", syntheticEtama3()},
                      {"etama3.dds", syntheticEtama3Texture()},
                      {"etama4.anm", syntheticEtama4()},
                      {"etama4.dds", syntheticEtama4Texture()},
                      {"stg1enm.anm", syntheticEnemy()},
                      {"stg1enm.dds", syntheticEnemyTexture()},
                      {"stg1enm2.anm", syntheticEnemy2()},
                      {"stg1enm2.dds", syntheticEnemyTexture()},
                      {"ecldata1.ecl", syntheticStageEcl()}});
}

// A World with the synthetic bullet/player art loaded straight from bytes.
struct TestWorld {
    TextureLibrary lib;
    ZunRng rng{0};
    AnmManager anm{lib, rng};
    std::unique_ptr<BulletManager> bullets = std::make_unique<BulletManager>();
    std::unique_ptr<EnemyManager> enemies = std::make_unique<EnemyManager>();
    std::unique_ptr<ItemManager> items = std::make_unique<ItemManager>();
    std::unique_ptr<EffectManager> effects = std::make_unique<EffectManager>();
    Player player;
    GameState state;
    UnportedFeatures unported;
    EclFile ecl;
    World world;
    bool ok = true;

    TestWorld() {
        const std::uint16_t tex = lib.add("synthetic", 256, 256, [] { return std::optional<Image>(Image(256, 256)); });
        std::string err;
        ok = anm.loadAnmFromBytes(syntheticPlayer(), tex, kAnmFilePlayer, kAnmOffsetPlayer, &err) &&
             anm.loadAnmFromBytes(syntheticEtama3(), tex, kAnmFileBullet3, kAnmOffsetBullet3, &err) &&
             anm.loadAnmFromBytes(syntheticEtama4(), tex, kAnmFileBullet4, kAnmOffsetBullet4, &err) &&
             anm.loadAnmFromBytes(syntheticEnemy(), tex, kAnmFileEnemy, kAnmOffsetEnemy, &err);
        if (!ok) std::fprintf(stderr, "  TestWorld: %s\n", err.c_str());
        world.anm = &anm;
        world.bullets = bullets.get();
        world.enemies = enemies.get();
        world.items = items.get();
        world.effects = effects.get();
        world.player = &player;
        world.ecl = &ecl;
        world.state = &state;
        world.rng = &rng;
        world.unported = &unported;
        state.setDifficulty(Normal);
        ok = ok && bullets->initializeTemplates(world, &err);
        player.reset(world);
    }
};

namespace {
struct RecordingSink final : SpriteSink {
    struct Item {
        std::uint16_t texture;
        Blend blend;
        SpriteInstance s;
    };
    std::vector<Item> items;
    void draw(std::uint16_t texture, Blend blend, const SpriteInstance& s) override { items.push_back({texture, blend, s}); }
};
struct RecordingSound final : SoundSink {
    std::vector<int> played;
    void playSound(int idx) override { played.push_back(idx); }
};
struct RecordingBackend final : SampleBackend {
    struct Play {
        int voice, sample;
        float gain;
    };
    std::vector<Play> plays;
    int loads = 0;
    bool loadSample(int, const std::uint8_t*, std::size_t) override { return ++loads, true; }
    void playSample(int voice, int sample, float gain) override { plays.push_back({voice, sample, gain}); }
};
}  // namespace

static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

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

static void testZunMath() {
    // Rng::GetRandomU16 of the 2002 executable, from seed 0 (reference values computed
    // independently from the published algorithm).
    ZunRng rng(0);
    CHECK(rng.u16() == 0xC374);
    CHECK(rng.u16() == 0xBFC7);
    ZunRng a(0x1234), b(0x1234);
    const std::uint32_t hi = b.u16(), lo = b.u16();
    CHECK(a.u32() == ((hi << 16) | lo));  // high half first
    CHECK(a.generations() == 2);
    ZunRng c(7);
    const float f = c.f32InRange(10.0f);
    CHECK(f >= 0.0f && f <= 10.0f);

    CHECK(near(addNormalizeAngle(kZunPi, 0.5f), 0.5f - kZunPi));
    CHECK(near(addNormalizeAngle(-3.0f * kZunPi, 0.25f), 0.25f - kZunPi, 1e-4f));
    CHECK(std::isfinite(addNormalizeAngle(1e9f, 0.0f)));  // bounded loop: returns even for absurd input
    Vec3f v;
    sincosmul(v, kZunHalfPi, 2.0f);
    CHECK(near(v.x, 0.0f) && near(v.y, 2.0f));

    GameState gs;
    gs.setDifficulty(Easy);
    CHECK(gs.rank == 16 && gs.minRank == 12 && gs.maxRank == 20);
    gs.increaseSubrank(1000);
    CHECK(gs.rank == 20);  // clamped
    gs.setDifficulty(Normal);
    CHECK(gs.rankLerpInt(0, 32) == 16 && near(gs.rankLerpFloat(-0.5f, 0.5f), 0.0f));
    CHECK(colorCombineAlpha(0x00123456u, 0x80ffffffu) == 0x80123456u);
    CHECK(argbToRgbaBytes(0x80112233u) == 0x80332211u);
}

static void testAnmVm() {
    TextureLibrary lib;
    ZunRng rng(0);
    AnmManager anm(lib, rng);
    const std::uint16_t tex = lib.add("vm-test", 256, 256, [] { return std::optional<Image>(Image(256, 256)); });
    const std::vector<AnmSprite> sprites = {{0, 0, 0, 16, 16}, {1, 16, 0, 16, 16}, {2, 32, 0, 16, 16}, {3, 48, 0, 32, 16}};

    AnmCode s0;
    s0.ins(0, anm_op::SetSprite, {1});
    s0.ins(0, anm_op::Alpha, {0x40});
    s0.ins(0, anm_op::AlphaInterpLinear, {0xc0, 4});
    const std::uint32_t loopBack = s0.ins(2, anm_op::SetSprite, {2});
    s0.ins(3, anm_op::Halt);
    s0.ins(3, anm_op::InterruptLabel, {7});
    s0.ins(3, anm_op::SetSprite, {3});
    s0.ins(6, anm_op::Jump, {loopBack});
    AnmCode s1;
    s1.ins(0, anm_op::SetSprite, {0});
    s1.ins(1, anm_op::Delete);
    AnmCode s2;  // catch-all interrupt label
    s2.ins(0, anm_op::SetSprite, {0});
    s2.ins(0, anm_op::Halt);
    s2.ins(0, anm_op::InterruptLabel, {static_cast<std::uint32_t>(-1)});
    s2.ins(0, anm_op::SetSprite, {2});
    s2.ins(0, anm_op::Static);

    std::string err;
    constexpr int kOff = 0x100;
    CHECK(anm.loadAnmFromBytes(buildAnmScripts(256, 256, "x.dds", sprites, {{0, s0.bytes}, {1, s1.bytes}, {2, s2.bytes}}),
                               tex, 8, kOff, &err));
    CHECK(anm.hasScript(kOff) && anm.hasScript(kOff + 2) && !anm.hasScript(kOff + 3));
    CHECK(anm.sprite(kOff + 3) && anm.sprite(kOff + 3)->widthPx == 32.0f && near(anm.sprite(kOff + 3)->u0, 48.0f / 256));

    AnmVm vm;
    anm.setAndExecuteScript(vm, kOff);  // frame 0 runs inside
    CHECK(vm.activeSpriteIndex == kOff + 1 && vm.visible);
    CHECK((vm.color >> 24) == 0x60);  // 0x40 -> 0xc0 over 4 frames, one frame done
    CHECK(vm.timeInScript == 1);
    anm.executeScript(vm);  // 1
    CHECK((vm.color >> 24) == 0x80 && vm.activeSpriteIndex == kOff + 1);
    anm.executeScript(vm);  // 2: sprite 2
    CHECK(vm.activeSpriteIndex == kOff + 2 && (vm.color >> 24) == 0xa0);
    anm.executeScript(vm);  // 3: halt
    CHECK(vm.stopped && (vm.color >> 24) == 0xc0 && vm.timeInScript == 3);
    CHECK(anm.executeScript(vm) == 0 && vm.timeInScript == 3 && vm.activeSpriteIndex == kOff + 2);  // still halted
    vm.pendingInterrupt = 7;
    anm.executeScript(vm);  // resumes after "label 7" at its time (3)
    CHECK(vm.activeSpriteIndex == kOff + 3 && vm.pendingInterrupt == 0 && !vm.stopped && vm.timeInScript == 4);
    anm.executeScript(vm);  // 4
    anm.executeScript(vm);  // 5
    CHECK(vm.activeSpriteIndex == kOff + 3);
    anm.executeScript(vm);  // 6: jump back to the time-2 instruction
    CHECK(vm.activeSpriteIndex == kOff + 2 && vm.timeInScript == 3);
    anm.executeScript(vm);
    CHECK(vm.stopped && vm.timeInScript == 3);  // halted again

    vm.pendingInterrupt = 9;  // no such label, no catch-all: the VM stays put
    anm.executeScript(vm);
    CHECK(vm.activeSpriteIndex == kOff + 2 && vm.timeInScript == 3 && vm.pendingInterrupt == 0);

    AnmVm del;
    anm.setAndExecuteScript(del, kOff + 1);
    CHECK(del.visible && anm.executeScript(del) == 1 && !del.visible && del.currentInstruction == nullptr);
    CHECK(anm.executeScript(del) == 1);

    AnmVm any;
    anm.setAndExecuteScript(any, kOff + 2);
    any.pendingInterrupt = 5;
    anm.executeScript(any);
    CHECK(any.activeSpriteIndex == kOff + 2);  // took the -1 label

    AnmVm missing;
    anm.setAndExecuteScript(missing, kOff + 50);
    CHECK(!missing.visible && anm.executeScript(missing) == 1);

    // Drawing (Draw2 / Draw3 / Draw and the D3D blend states).
    RecordingSink sink;
    AnmVm d;
    anm.setAndExecuteScript(d, kOff + 2);  // sprite 0, then halted
    d.pos = {100.4f, 50.6f, 0.0f};
    anm.beginFrame();
    anm.drawWorld(sink, d, kGameRegionX, kGameRegionY);
    CHECK(sink.items.size() == 1);
    if (!sink.items.empty()) {
        const auto& it = sink.items[0];
        CHECK(it.texture == tex && it.blend == Blend::Alpha);
        CHECK(it.s.x == 132.0f && it.s.y == 67.0f && it.s.w == 16.0f && it.s.h == 16.0f);  // rint, 256 * 16/256
        CHECK(it.s.angle == 0.0f && it.s.color == 0xffffffffu);
    }
    d.rotation.z = 0.5f;
    d.additive = true;
    d.colorAdd = true;
    d.color = 0x80ff0000u;
    anm.drawWorld(sink, d, kGameRegionX, kGameRegionY);
    CHECK(sink.items.size() == 2);
    if (sink.items.size() == 2) {
        const auto& it = sink.items[1];
        CHECK(it.blend == Blend::AdditiveColorAdd && near(it.s.x, 132.4f, 1e-4f) && it.s.angle == -0.5f);
        CHECK(it.s.color == 0x800000ffu);  // ARGB -> bytes R,G,B,A
    }
    d.anchor = AnmAnchor::TopLeft;
    d.rotation = {};
    d.scaleX = 2.0f;
    anm.drawScreen(sink, d);  // DrawNoRotation
    CHECK(sink.items.size() == 3 && sink.items[2].s.w == 32.0f && sink.items[2].s.x == 116.0f);
    d.color = 0;
    anm.drawWorld(sink, d, 0, 0);
    CHECK(sink.items.size() == 3);  // fully transparent colour is skipped

    // UV cache: texture coordinates refresh only when the sprite changes between draws.
    AnmVm scrollA, scrollB;
    anm.setAndExecuteScript(scrollA, kOff + 2);
    anm.setAndExecuteScript(scrollB, kOff + 2);
    scrollA.uvScrollX = 0.25f;
    anm.beginFrame();
    anm.drawWorld(sink, scrollA, 0, 0);
    anm.drawWorld(sink, scrollB, 0, 0);
    CHECK(sink.items.size() == 5 && sink.items[3].s.u0 == 0.25f && sink.items[4].s.u0 == 0.25f);

    // Releasing a slot forgets its sprites and scripts.
    anm.releaseAnm(8);
    CHECK(!anm.fileLoaded(8) && !anm.hasScript(kOff) && anm.sprite(kOff) == nullptr);
}

static void testEclVm() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    EclBuilder e;
    e.spawn(0, timeline_op::EnemyCreate, 0, 192.0f, 100.0f, 50, -1, 1000);
    e.spawn(1, timeline_op::EnemyCreate, 3, 50.0f, 50.0f, 10, -1, 0);
    e.spawn(2, timeline_op::EnemyCreateMirror, 1, 200.0f, 100.0f, 10, -1, 0);

    e.sub();  // 0
    e.ins(0, ecl_op::SetInt, {static_cast<std::uint32_t>(kI0), 3});
    e.ins(0, ecl_op::SetFloat, {static_cast<std::uint32_t>(kF0), fbits(1.5f)});
    e.ins(0, ecl_op::SetInt, {static_cast<std::uint32_t>(kI2), 99}, 0x1d);  // all but Normal: skipped
    e.ins(0, ecl_op::SetInt, {static_cast<std::uint32_t>(kI3), 42}, 0x02);  // Normal only
    const BulletWords ring = bulletWords(3, 2, 4, 1, static_cast<float>(kF0), 1.0f, 0.0f, 0.0f, 0);
    const std::uint32_t fire = e.ins(1, ecl_op::BulletCircle, BW(ring));
    e.ins(3, ecl_op::Loop, {1, e.to(fire), static_cast<std::uint32_t>(kI0)});
    e.ins(3, ecl_op::Call, {2, 7, fbits(2.5f)});
    e.ins(3, ecl_op::CmpInt, {static_cast<std::uint32_t>(kI0), 0});
    const std::uint32_t incAt = e.ins(3, ecl_op::MathInc, {5});
    e.ins(10, ecl_op::EnemyDelete);

    e.sub();  // 1: moves (and must not fall through into sub 2)
    e.ins(0, ecl_op::MoveAxisSpeed, {fbits(1.0f), fbits(0.5f), fbits(0.0f)});
    e.ins(1000, ecl_op::EnemyDelete);

    e.sub();  // 2: called with int0 = 7, float0 = 2.5
    e.ins(0, ecl_op::MathFloatAdd, {static_cast<std::uint32_t>(kF1), fbits(static_cast<float>(kF0)), fbits(10.0f)});
    e.ins(0, ecl_op::MovePosition, {fbits(static_cast<float>(kF1)), fbits(200.0f), fbits(0.0f)});
    e.ins(0, ecl_op::MathIntMul, {static_cast<std::uint32_t>(kI1), static_cast<std::uint32_t>(kI0), 3});
    e.ins(0, ecl_op::SetInt, {static_cast<std::uint32_t>(kEnemyLife), static_cast<std::uint32_t>(kI1)});
    e.ins(0, ecl_op::Ret);

    e.sub();  // 3: jumps to itself forever
    e.ins(0, ecl_op::Jump, {0, 0});

    std::string err;
    CHECK(tw.ecl.load(e.build(), &err));
    CHECK(tw.ecl.subCount() == 4);
    EnemyManager& em = *tw.enemies;
    em.startStage(tw.world);
    Enemy& a = em.enemy(0);

    em.update(tw.world);  // frame 0: spawn (runs time 0), then the frame's run (time 1: fire)
    // Timeline time 0 is a multiple of the subrank interval: rank 16 -> 17 before anything
    // spawns, so bullet speeds get +17/32 - 0.5 = +0.03125.
    CHECK(tw.state.rank == 17);
    const float speed = 1.5f + tw.state.rankLerpFloat(-0.5f, 0.5f);
    CHECK(speed == 1.53125f);
    CHECK(em.activeCount() == 1 && a.slotOccupied && a.life == 50 && a.score == 1000);
    CHECK(a.ctx.int0 == 3 && near(a.ctx.float0, 1.5f) && a.ctx.int2 == 0 && a.ctx.int3 == 42);
    CHECK(a.ctx.time == 2 && tw.bullets->activeCount() == 4);
    const float angles[4] = {0.0f, kZunHalfPi, kZunPi, -kZunHalfPi};
    for (int i = 0; i < 4; ++i) {
        const Bullet& b = tw.bullets->at(i);
        CHECK(b.state == Fired && b.speed == speed && near(b.angle, angles[i], 1e-5f));
        CHECK(near(b.velocity.x, speed * std::cos(angles[i])) && near(b.velocity.y, speed * std::sin(angles[i])));
        CHECK(b.pos.x == 192.0f && b.pos.y == 100.0f);
    }

    em.update(tw.world);  // frame 1: the runaway sub is removed by the instruction guard
    CHECK(em.activeCount() == 1 && !em.timelineFinished());
    em.update(tw.world);  // frame 2: loop 1 of 2 jumps back, fires again; mirrored enemy spawns
    CHECK(tw.bullets->activeCount() == 8 && a.ctx.int0 == 2 && em.activeCount() == 2 && em.timelineFinished());
    Enemy& m = em.enemy(1);
    CHECK(m.mirrored && m.position.x == 199.0f && m.position.y == 100.5f);  // moved in its first frame
    em.update(tw.world);  // 3
    em.update(tw.world);  // 4: last jump
    CHECK(tw.bullets->activeCount() == 12 && a.ctx.int0 == 1);
    em.update(tw.world);  // 5
    em.update(tw.world);  // 6: loop ends, call, return, compare, self-modifying increment
    CHECK(tw.bullets->activeCount() == 12 && a.ctx.int0 == 0 && a.stackDepth == 0);
    CHECK(near(a.position.x, 12.5f) && a.position.y == 200.0f);  // float0 + 10 via a float var ref
    CHECK(a.life == 21);                                         // int0 * 3 written to ENEMY_LIFE
    CHECK(a.ctx.compareRegister == 0 && a.ctx.time == 4);
    CHECK(tw.ecl.argAt<std::int32_t>(tw.ecl.subOffset(0) + incAt, 0) == 6);  // the script edited itself
    CHECK(m.position.x == 195.0f && m.position.y == 102.5f);
    for (int f = 7; f <= 12; ++f) em.update(tw.world);
    CHECK(a.slotOccupied);
    em.update(tw.world);  // 13: time 10, delete
    CHECK(!a.slotOccupied && em.activeCount() == 1);
    CHECK(em.instructionsExecuted() > 30);

    // Files that would make the VM read out of bounds are rejected up front.
    Bytes bad = e.build();
    bad[16] = 0xff;  // sub 0 offset far past the end
    bad[17] = 0xff;
    EclFile f;
    CHECK(!f.load(bad, &err));
    // The game's own files put the timeline last and end it with only the 4-byte start of a
    // record (time -1): only `time` is ever read there.
    {
        Bytes real;
        auto put16 = [&](std::int16_t v) { putLe<std::int16_t>(real, v); };
        auto put32 = [&](std::uint32_t v) { putLe<std::uint32_t>(real, v); };
        put16(1);   // subCount
        put16(1);   // mainCount
        put32(20 + 16);  // timeline after the sub
        put32(0);
        put32(0);
        put32(20);  // sub 0
        // sub 0: one 12-byte instruction (delete at time 100) + 4 padding
        putLe<std::int32_t>(real, 100);
        put16(ecl_op::EnemyDelete);
        put16(12);
        put32(0x0000ff00u);
        put32(0);
        // timeline: one spawn at frame 3, then the short terminator ending the file
        put16(3);
        put16(0);
        put16(timeline_op::EnemyCreate);
        put16(28);
        for (int i = 0; i < 5; ++i) put32(0);
        put16(-1);
        put16(0);
        EclFile g;
        CHECK(g.load(real, &err) && g.timelineRecords() == 1 && g.timelineEnd() == real.size() - 4);
        TimelineInstr t;
        TimelineInstrArgs args;
        CHECK(g.timelineAt(g.timelineEnd(), t, args) && t.time == -1 && t.size == 0);
    }
    // A damaged chain (record of size 0) is cut off there instead of rejecting the file.
    Bytes damaged(40, 0);
    damaged[0] = 1;   // one sub, at 20
    damaged[4] = 20;  // timeline at 20: a record with time 5 and size 0
    damaged[16] = 20;
    damaged[20] = 5;
    CHECK(f.load(damaged, &err) && f.timelineRecords() == 0 && f.timelineEnd() == 20);
}

static void testBullets() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    BulletManager& bm = *tw.bullets;
    // Player spawns at (192, 384): straight below a shooter at (192, 100).
    CHECK(tw.player.positionCenter.x == 192.0f && tw.player.positionCenter.y == 384.0f);
    EnemyShooter s;
    s.sprite = 3;
    s.color = 1;
    s.position = {192.0f, 100.0f, 0.0f};
    s.aimMode = FanAimed;
    s.count1 = 3;
    s.count2 = 2;
    s.speed1 = 3.0f;
    s.speed2 = 2.0f;
    s.angle1 = 0.0f;
    s.angle2 = 0.2f;
    bm.spawnBulletPattern(tw.world, s);
    CHECK(bm.activeCount() == 6);
    const float fan[3] = {kZunHalfPi, kZunHalfPi - 0.2f, kZunHalfPi + 0.2f};
    for (int i = 0; i < 6; ++i) {
        const Bullet& b = bm.at(i);
        CHECK(near(b.angle, fan[i % 3], 1e-5f) && near(b.speed, i < 3 ? 3.0f : 2.5f));
        CHECK(b.sprites.spriteBullet.activeSpriteIndex == kAnmOffsetBullet3 + 16 + 3 * 8 + 1);  // type 3, colour 1
        CHECK(b.sprites.grazeSize.x == 6.0f);
    }
    s.count1 = 2;
    s.count2 = 1;
    bm.spawnBulletPattern(tw.world, s);
    CHECK(near(bm.at(6).angle, kZunHalfPi + 0.1f) && near(bm.at(7).angle, kZunHalfPi - 0.1f));

    // Movement + interpolation state.
    const Vec3f before = bm.at(0).pos;
    bm.update(tw.world);
    CHECK(bm.at(0).prevPos.y == before.y && near(bm.at(0).pos.y, before.y + 3.0f, 1e-4f));

    // removeAllBullets(false): each bullet plays its cancel (donut) animation, then frees
    // its slot; the synthetic donut scripts end at script time 8.
    bm.removeAllBullets(tw.world, false);
    CHECK(bm.activeCount() == 8 && bm.at(0).state == Despawning);
    for (int f = 0; f < 8; ++f) bm.update(tw.world);
    CHECK(bm.activeCount() == 0);
    // Spawn effects delay firing until their ANM script ends (here at script time 8).

    s.aimMode = Circle;
    s.count1 = 1;
    s.count2 = 1;
    s.flags = ExSpawnEffectShort;
    bm.spawnBulletPattern(tw.world, s);
    int firstFired = -1;
    for (int f = 0; f < 20 && firstFired < 0; ++f) {
        bm.update(tw.world);
        for (int i = 0; i < BulletManager::kMaxBullets; ++i) {
            if (bm.at(i).state == Fired) firstFired = f;
        }
    }
    CHECK(firstFired == 7);  // the template VM already ran its first frame, as in the original
    bm.removeAllBullets(tw.world, true);

    // The original's fixed 640 slots: a 700-way ring fills them and stops.
    s.flags = 0;
    s.count1 = 700;
    bm.spawnBulletPattern(tw.world, s);
    CHECK(bm.activeCount() == BulletManager::kMaxBullets && bm.spawnFailures() == 1 && bm.peakCount() >= 6);
    bm.removeAllBullets(tw.world, true);
    CHECK(bm.activeCount() == 0);

    // Off-screen bullets are culled.
    s.count1 = 1;
    s.speed1 = s.speed2 = 8.0f;
    s.aimMode = Fan;
    s.angle1 = -kZunHalfPi;  // up
    s.angle2 = 0.0f;
    bm.spawnBulletPattern(tw.world, s);
    for (int f = 0; f < 120; ++f) bm.update(tw.world);
    CHECK(bm.activeCount() == 0);
}

static void testItems() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    RecordingSound sound;
    tw.world.sound = &sound;
    ItemManager& im = *tw.items;
    GameState& gs = tw.state;
    Player& pl = tw.player;
    pl.state = Player::Alive;
    pl.positionCenter = {192.0f, 384.0f, 0.0f};

    // Trajectory: rises at 2.2 px/frame, gravity 0.03, falls at most 3 px/frame.
    CHECK(im.spawn(tw.world, {50.0f, 300.0f, 0.0f}, ItemPowerSmall, ItemState::Falling));
    float y = 300.0f, vy = -2.2f;
    bool same = true;
    for (int f = 0; f < 200 && im.at(0).active; ++f) {
        im.update(tw.world);
        y += vy;
        if (y >= 464.0f) break;
        if (vy < 3.0f) vy += 0.03f;
        else vy = 3.0f;
        same = same && im.at(0).pos.y == y && im.at(0).velocity.y == vy;
    }
    CHECK(same);
    const std::int32_t rankBefore = gs.rank;
    for (int f = 0; f < 200 && im.at(0).active; ++f) im.update(tw.world);
    CHECK(!im.at(0).active && gs.rank == rankBefore - 1 && gs.subRank == 97);  // lost item: subrank -3
    gs.setDifficulty(Normal);

    // Grab box 24x24 vs item box 16x16, touching edges count.
    pl.positionCenter = {100.0f, 100.0f, 0.0f};
    CHECK(im.spawn(tw.world, {120.0f, 102.2f, 0.0f}, ItemPoint, ItemState::Falling));    // dx 20: touches
    CHECK(im.spawn(tw.world, {120.5f, 102.2f, 0.0f}, ItemPowerSmall, ItemState::Falling));  // dx 20.5: misses
    im.update(tw.world);
    CHECK(gs.score == 100000 && gs.stagePointItems == 1 && gs.subRank == 30);  // above the line: top value
    CHECK(im.activeCount() == 1 && sound.played.size() == 1 && sound.played[0] == 21);
    CHECK(ItemManager::pointItemValue(Normal, 200.9f) == 52800);
    CHECK(ItemManager::pointItemValue(Lunatic, 127.9f) == 200000);
    CHECK(ItemManager::pointItemValue(Hard, 300.0f) == 69040);
    CHECK(ItemManager::pointItemValue(Extra, 128.0f) == 200000);

    // Power: +1 per small item, a power-up sound when a level threshold is crossed.
    auto collectAt = [&](int type) {
        im.spawn(tw.world, {pl.positionCenter.x, pl.positionCenter.y + 2.2f, 0.0f}, type, ItemState::Falling);
        im.update(tw.world);
    };
    im.reset();
    gs.score = 0;
    sound.played.clear();
    for (int i = 0; i < 7; ++i) collectAt(ItemPowerSmall);
    CHECK(gs.power == 7 && gs.score == 70 && std::count(sound.played.begin(), sound.played.end(), 31) == 0);
    collectAt(ItemPowerSmall);
    CHECK(gs.power == 8 && gs.powerLevel() == 1 && std::count(sound.played.begin(), sound.played.end(), 31) == 1);
    collectAt(ItemPowerBig);
    CHECK(gs.power == 16 && gs.shotTier() == 2);

    // Reaching full power turns every bullet into a homing star.
    EnemyShooter s;
    s.sprite = 3;
    s.position = {192.0f, 50.0f, 0.0f};
    s.aimMode = Circle;
    s.count1 = 10;
    s.count2 = 1;
    s.speed1 = s.speed2 = 1.0f;
    tw.bullets->spawnBulletPattern(tw.world, s);
    CHECK(tw.bullets->activeCount() == 10);
    gs.power = 127;
    collectAt(ItemPowerSmall);
    CHECK(gs.power == 128 && tw.bullets->activeCount() == 0);
    int stars = 0;
    for (int i = 0; i < ItemManager::kMaxItems; ++i) stars += im.at(i).active && im.at(i).type == ItemStar;
    CHECK(stars == 10);
    for (int i = 0; i < ItemManager::kMaxItems; ++i) {
        if (im.at(i).active) CHECK(im.at(i).state == ItemState::Magnet);
    }
    // Full power: small/big items score from the chain table.
    im.reset();
    gs.score = 0;
    gs.fullPowerChain = 0;
    collectAt(ItemPowerSmall);
    CHECK(gs.score == 20 && gs.fullPowerChain == 1);
    collectAt(ItemPowerBig);
    CHECK(gs.score == 20 + 100 && gs.fullPowerChain == 9);
    for (int i = 0; i < 10; ++i) collectAt(ItemPowerBig);
    CHECK(gs.fullPowerChain == 30 && ItemManager::fullPowerScore(30) == 51200);

    // Above the collection line at full power, falling items home in at 8 px/frame.
    im.reset();
    pl.positionCenter = {192.0f, 100.0f, 0.0f};
    im.spawn(tw.world, {192.0f, 300.0f, 0.0f}, ItemPoint, ItemState::Falling);
    im.update(tw.world);
    CHECK(im.at(0).state == ItemState::Magnet && near(im.at(0).pos.y, 292.0f, 1e-4f) && near(im.at(0).pos.x, 192.0f, 1e-4f));
    pl.positionCenter = {192.0f, 200.0f, 0.0f};  // below the line: no homing for new items
    im.spawn(tw.world, {50.0f, 300.0f, 0.0f}, ItemPoint, ItemState::Falling);
    im.update(tw.world);
    CHECK(im.at(1).state == ItemState::Falling);

    // Bombs cap at 8, lives at 8; a 1up plays the extend sound; stars score by graze.
    im.reset();
    gs.bombs = 7;
    collectAt(ItemBomb);
    collectAt(ItemBomb);
    CHECK(gs.bombs == 8);
    gs.livesRemaining = 2;
    sound.played.clear();
    collectAt(ItemLife);
    CHECK(gs.livesRemaining == 3 && std::count(sound.played.begin(), sound.played.end(), 28) == 1);
    gs.score = 0;
    gs.grazes = 31;
    collectAt(ItemStar);
    CHECK(gs.score == 600);

    // Two items collected in one frame: one pickup sound.
    sound.played.clear();
    im.spawn(tw.world, {pl.positionCenter.x, pl.positionCenter.y, 0.0f}, ItemPoint, ItemState::Falling);
    im.spawn(tw.world, {pl.positionCenter.x + 1, pl.positionCenter.y, 0.0f}, ItemPoint, ItemState::Falling);
    im.update(tw.world);
    CHECK(std::count(sound.played.begin(), sound.played.end(), 21) == 1);

    // Only alive or invulnerable players collect.
    pl.state = Player::Spawning;
    im.spawn(tw.world, {pl.positionCenter.x, pl.positionCenter.y, 0.0f}, ItemPoint, ItemState::Falling);
    im.update(tw.world);
    CHECK(im.activeCount() == 1);
    pl.state = Player::Alive;

    // Pool of 512; the 513th spawn fails without drawing random numbers.
    im.reset();
    pl.positionCenter = {10.0f, 440.0f, 0.0f};
    for (int i = 0; i < 512; ++i) im.spawn(tw.world, {300.0f, 50.0f, 0.0f}, ItemPoint, ItemState::Falling);
    const std::uint32_t gens = tw.rng.generations();
    CHECK(!im.spawn(tw.world, {300.0f, 50.0f, 0.0f}, ItemPowerSmall, ItemState::DeathScatter));
    CHECK(tw.rng.generations() == gens && im.activeCount() == 512);

    // Death scatter: two random floats pick the target, then a 60-frame lerp.
    im.reset();
    ZunRng predict = tw.rng;
    const float tx = predict.f32InRange(288.0f) + 48.0f;
    const float ty = predict.f32InRange(192.0f) - 64.0f;
    const Vec3f start{192.0f, 384.0f, 0.0f};
    im.spawn(tw.world, start, ItemPowerBig, ItemState::DeathScatter);
    CHECK(im.at(0).target.x == tx && im.at(0).target.y == ty);
    for (int f = 0; f < 31; ++f) im.update(tw.world);
    const Vec3f mid = Vec3f{tx, ty, 0.0f} * 0.5f + start * (1.0f - 0.5f);
    CHECK(im.at(0).pos.x == mid.x && im.at(0).pos.y == mid.y);

    // Drawing: off the top an item becomes its indicator, pinned to the edge and faded.
    RecordingSink sink;
    im.reset();
    im.spawn(tw.world, {100.0f, -72.0f, 0.0f}, ItemPoint, ItemState::Falling);
    tw.anm.beginFrame();
    im.draw(tw.world, sink, 1.0f);
    CHECK(sink.items.size() == 1);
    if (!sink.items.empty()) {
        CHECK(im.at(0).vm.activeSpriteIndex == ItemManager::kAnmSpriteFirstIndicator + ItemPoint);
        CHECK(sink.items[0].s.y == kGameRegionY + 8.0f);
        CHECK((sink.items[0].s.color >> 24) == 96);  // 255 - trunc((8 + 72) * 255 / 128)
    }
}

static void testItemDrops() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    // Six enemies with random drops die on spawn: exactly two drop, from the table.
    EclBuilder e;
    for (int i = 0; i < 6; ++i) e.spawn(0, timeline_op::EnemyCreate, 0, 50.0f + 40.0f * i, 100.0f, 0, -1, 0);
    e.spawn(1, timeline_op::EnemyCreate, 1, 200.0f, 200.0f, 10, -2, 0);
    e.sub();
    e.ins(1000, ecl_op::EnemyDelete);
    e.sub();
    e.ins(0, ecl_op::DropItems, {3});
    e.ins(0, ecl_op::DropItemId, {ItemLife});
    e.ins(1000, ecl_op::EnemyDelete);
    std::string err;
    CHECK(tw.ecl.load(e.build(), &err));
    ZunRng predict = tw.rng;
    const std::uint16_t r0 = predict.u16InRange(3);
    const std::uint16_t t0 = predict.u16InRange(8);
    tw.enemies->startStage(tw.world);
    CHECK(tw.enemies->randomItemSpawnIndex() == r0 && tw.enemies->randomItemTableIndex() == t0);
    tw.player.positionCenter = {10.0f, 440.0f, 0.0f};  // out of the way
    tw.enemies->update(tw.world);
    CHECK(tw.enemies->activeCount() == 0);
    CHECK(tw.items->activeCount() == 2 && tw.enemies->randomItemSpawnIndex() == r0 + 6);
    static constexpr int kTable[32] = {0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0,
                                       1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 2};
    CHECK(tw.items->at(0).type == kTable[t0] && tw.items->at(1).type == kTable[(t0 + 1) % 32]);

    // DROP_ITEMS: big power then small power, scattered by two random floats each.
    tw.items->reset();
    ZunRng p2 = tw.rng;
    float dx[3], dy[3];
    for (int i = 0; i < 3; ++i) {
        dx[i] = p2.f32InRange(144.0f) - 72.0f;
        dy[i] = p2.f32InRange(144.0f) - 72.0f;
    }
    tw.enemies->update(tw.world);  // timeline time 1: the dropper spawns and runs
    CHECK(tw.items->activeCount() == 4);
    CHECK(tw.items->at(0).type == ItemPowerBig && tw.items->at(1).type == ItemPowerSmall &&
          tw.items->at(2).type == ItemPowerSmall && tw.items->at(3).type == ItemLife);
    // (positions after this frame's item update are not run here: items update in the bullet manager)
    CHECK(tw.items->at(0).pos.x == 200.0f + dx[0] && tw.items->at(0).pos.y == 200.0f + dy[0]);
    CHECK(tw.items->at(2).pos.x == 200.0f + dx[2] && tw.items->at(2).pos.y == 200.0f + dy[2]);
}

static void testLasers() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    BulletManager& bm = *tw.bullets;
    Player& pl = tw.player;
    pl.positionCenter = {10.0f, 440.0f, 0.0f};
    pl.state = Player::Alive;

    // Growth and the three phases with their exact frame counts.
    LaserProps p;
    p.position = {100.0f, 200.0f, 0.0f};
    p.angle = 0.0f;
    p.speed = 4.0f;
    p.startLength = 50.0f;
    p.width = 16.0f;
    p.startTime = 10;
    p.duration = 20;
    p.despawnDuration = 8;
    p.hitboxStartTime = 5;
    p.hitboxEndDelay = 4;
    const int slot = bm.spawnLaser(tw.world, p);
    CHECK(slot == 0 && bm.laser(0).inUse && bm.laser(0).state == LaserStartDelay);
    CHECK(bm.laser(0).vm.activeSpriteIndex == kAnmOffsetBullet3 + 146);  // line laser, colour 0
    CHECK(bm.laser(0).glowVm.activeSpriteIndex == kAnmOffsetBullet3 + 140 && bm.laser(0).glowVm.additive);
    bm.update(tw.world);
    CHECK(near(bm.laser(0).vm.scaleX, 1.2f / 16.0f));  // warning line 1.2 px wide
    for (int f = 2; f <= 6; ++f) bm.update(tw.world);
    CHECK(near(bm.laser(0).vm.scaleX, 0.5f));  // ramping up: timer 5 of 10
    for (int f = 7; f <= 10; ++f) bm.update(tw.world);
    CHECK(bm.laser(0).state == LaserStartDelay && bm.laser(0).timer == 10);
    bm.update(tw.world);  // 11
    CHECK(bm.laser(0).state == LaserActive && bm.laser(0).timer == 1);
    CHECK(bm.laser(0).endOffset == 44.0f && bm.laser(0).startOffset == 0.0f);
    for (int f = 12; f <= 30; ++f) bm.update(tw.world);
    CHECK(bm.laser(0).endOffset == 120.0f && bm.laser(0).startOffset == 70.0f);  // 50 long, tail follows
    CHECK(near(bm.laser(0).vm.scaleY, 50.0f / 16.0f) && near(bm.laser(0).vm.rotation.z, kZunHalfPi));
    bm.update(tw.world);  // 31
    CHECK(bm.laser(0).state == LaserDespawning && bm.laser(0).timer == 1);
    for (int f = 32; f <= 38; ++f) bm.update(tw.world);
    CHECK(bm.laser(0).inUse);
    bm.update(tw.world);  // 39
    CHECK(!bm.laser(0).inUse && bm.activeLaserCount() == 0);

    // Hit test: deadly thickness is half the visual width.
    LaserProps q;
    q.position = {100.0f, 200.0f, 0.0f};
    q.endOffset = q.startLength = 200.0f;
    q.width = 16.0f;
    q.duration = 100;
    bm.spawnLaser(tw.world, q);
    pl.positionCenter = {150.0f, 203.5f, 0.0f};
    bm.update(tw.world);
    CHECK(pl.state == Player::Dead && tw.state.deaths == 1);
    bm.removeAllBullets(tw.world, false);
    for (int f = 0; f < 40; ++f) bm.update(tw.world);
    CHECK(bm.activeLaserCount() == 0);

    // Graze: within 48 px of the box, once every 12 frames, +500 and subrank +6.
    pl.state = Player::Alive;
    pl.positionCenter = {150.0f, 205.3f, 0.0f};
    tw.state.score = 0;
    const std::uint32_t grazesBefore = tw.state.grazes;
    bm.spawnLaser(tw.world, q);
    for (int f = 0; f < 25; ++f) bm.update(tw.world);
    CHECK(pl.state == Player::Alive && tw.state.grazes == grazesBefore + 3 && tw.state.score == 1500);
    bm.removeAllBullets(tw.world, false);
    for (int f = 0; f < 40; ++f) bm.update(tw.world);

    // Rotated laser: the player is rotated into the laser's frame.
    LaserProps r = q;
    r.position = {100.0f, 100.0f, 0.0f};
    r.angle = kZunHalfPi;  // straight down
    bm.spawnLaser(tw.world, r);
    pl.positionCenter = {106.0f, 150.0f, 0.0f};  // 6 px beside it: graze only
    tw.state.grazes = 0;
    bm.update(tw.world);
    CHECK(pl.state == Player::Alive && tw.state.grazes == 1);
    pl.positionCenter = {101.0f, 150.0f, 0.0f};
    bm.update(tw.world);
    CHECK(pl.state == Player::Dead);
    pl.state = Player::Spawning;  // respawning players are neither hit nor graze
    tw.state.grazes = 0;
    pl.positionCenter = {101.0f, 160.0f, 0.0f};
    for (int f = 0; f < 13; ++f) bm.update(tw.world);
    CHECK(pl.state == Player::Spawning && tw.state.grazes == 0);

    // Cancelling into stars: one every 32 px along the visible part.
    tw.items->reset();
    bm.removeAllBullets(tw.world, true);
    CHECK(bm.laser(0).state == LaserDespawning && bm.laser(0).hitboxEndDelay == 0);
    CHECK(tw.items->activeCount() == 7);  // 0, 32, ..., 192 < 200
    for (int f = 0; f < 40; ++f) bm.update(tw.world);

    // Warning-line quirk: the hitbox shrinks to a speck at the laser's midpoint.
    LaserProps w = q;
    w.startTime = 30;
    w.hitboxStartTime = 0;
    bm.spawnLaser(tw.world, w);
    pl.state = Player::Alive;
    pl.positionCenter = {120.0f, 200.0f, 0.0f};  // on the line, far from the middle
    bm.update(tw.world);
    CHECK(pl.state == Player::Alive);
    pl.positionCenter = {200.0f, 200.0f, 0.0f};  // the midpoint
    bm.update(tw.world);
    CHECK(pl.state == Player::Dead);
    bm.removeAllBullets(tw.world, false);
    for (int f = 0; f < 40; ++f) bm.update(tw.world);

    // Drawing: beam centred on the visible part, rotated along the laser; glow at the origin.
    pl.state = Player::Spawning;
    tw.items->reset();
    LaserProps d = q;
    d.angle = kZunHalfPi;
    d.position = {200.0f, 50.0f, 0.0f};
    bm.spawnLaser(tw.world, d);
    bm.update(tw.world);
    RecordingSink sink;
    tw.anm.beginFrame();
    bm.draw(tw.world, sink, 1.0f);
    CHECK(sink.items.size() == 2);
    if (sink.items.size() == 2) {
        const auto& beam = sink.items[0].s;
        CHECK(near(beam.x, kGameRegionX + 200.0f, 1e-3f) && near(beam.y, kGameRegionY + 150.0f, 1e-3f));
        CHECK(near(beam.w, 16.0f, 1e-3f) && near(beam.h, 200.0f, 1e-3f) && near(beam.angle, 0.0f, 1e-6f));
        CHECK(sink.items[1].blend == Blend::AdditiveColorAdd && near(sink.items[1].s.w, 16.0f * 1.6f, 1e-3f));
    }
}

static void testLaserOpcodes() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    tw.player.positionCenter = {192.0f, 384.0f, 0.0f};
    tw.player.state = Player::Spawning;
    EclBuilder e;
    e.spawn(0, timeline_op::EnemyCreate, 0, 192.0f, 100.0f, 100, -2, 0);
    e.sub();
    e.ins(0, ecl_op::LaserIndex, {2});
    const LaserWords lw = laserWords(0, 3, 0.25f, 0.0f, 0.0f, 100.0f, 100.0f, 12.0f, 0, 50, 10, 0, 5, 0);
    e.ins(0, ecl_op::LaserCreateAimed, LW(lw));
    e.ins(0, ecl_op::LaserRotate, {2, fbits(0.5f)});
    e.ins(0, ecl_op::LaserTest, {2});
    e.ins(1, ecl_op::LaserOffset, {2, fbits(10.0f), fbits(20.0f), fbits(0.0f)});
    e.ins(2, ecl_op::LaserCancel, {2});
    e.ins(3, ecl_op::LaserClearAll);
    e.ins(3, ecl_op::LaserTest, {2});
    e.ins(1000, ecl_op::EnemyDelete);
    std::string err;
    CHECK(tw.ecl.load(e.build(), &err));
    tw.enemies->startStage(tw.world);
    Enemy& en = tw.enemies->enemy(0);
    tw.enemies->update(tw.world);  // spawn: time 0 runs, then time 1
    Laser& l = tw.bullets->laser(0);
    CHECK(l.inUse && en.laserRefs[2] == 0 && en.laserRefs[0] == -1);
    CHECK(near(l.angle, kZunHalfPi + 0.25f + 0.5f, 1e-5f));  // aimed straight down, +0.25, rotated +0.5
    CHECK(l.vm.activeSpriteIndex == kAnmOffsetBullet3 + 146 + 3);
    CHECK(l.pos.x == 202.0f && l.pos.y == 120.0f);  // offset by time 1
    tw.enemies->update(tw.world);  // time 2: cancel
    CHECK(l.state == LaserDespawning && l.timer == 0);
    tw.enemies->update(tw.world);  // time 3: references cleared, test fails
    CHECK(en.laserRefs[2] == -1 && en.ctx.compareRegister == 1 && l.inUse);
    static_assert(ecl_op::LaserCreate == 85 && ecl_op::LaserCancel == 92 && ecl_op::DropItems == 119 &&
                  ecl_op::DropItemId == 124 && ecl_op::LaserClearAll == 134);
}

static void testScoreAndLives() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    RecordingSound sound;
    tw.world.sound = &sound;
    GameState& gs = tw.state;

    // The displayed score rolls up by 1/32 of the gap, in multiples of 10, at least 10.
    gs.score = 1000;
    rollScore(tw.world);
    CHECK(gs.displayedScore == 30);
    rollScore(tw.world);
    CHECK(gs.displayedScore == 60);
    for (int f = 0; f < 200; ++f) rollScore(tw.world);
    CHECK(gs.displayedScore == 1000);

    // Extend at 10M of displayed score: one life, sound 28, subrank +200.
    gs.livesRemaining = 2;
    gs.score = 10'000'000;
    const std::int32_t rank = gs.rank;
    int frames = 0;
    while (gs.livesRemaining == 2 && frames < 1000) {
        rollScore(tw.world);
        ++frames;
    }
    CHECK(gs.livesRemaining == 3 && gs.extendIndex == 1 && gs.rank == rank + 2 && frames > 100);
    CHECK(std::count(sound.played.begin(), sound.played.end(), 28) == 1);

    // Stage-clear bonus.
    gs.grazes = 100;
    gs.power = 64;
    gs.stagePointItems = 50;
    gs.difficulty = Normal;
    CHECK(stageClearBonus(gs, 2) == (2000 + 1000 + 6400) * 50);
    gs.difficulty = Hard;
    gs.livesRemaining = 2;
    gs.bombs = 3;
    CHECK(stageClearBonus(gs, 6) == ((6000 + 1000 + 6400) * 50 + 2 * 3'000'000 + 3 * 1'000'000) * 12 / 10);

    // Death: drops after 6 frames (1 big + 5 small, power -16), a life is lost at respawn.
    Player& pl = tw.player;
    InputState idle;
    pl.update(tw.world, idle);  // spawning ends: respawn countdown 6
    pl.state = Player::Alive;
    gs.power = 40;
    gs.livesRemaining = 1;
    gs.bombs = 1;
    tw.items->reset();
    pl.checkCollision(tw.world, pl.positionCenter, {4.0f, 4.0f, 0.0f});
    CHECK(pl.state == Player::Dead);
    InputState none;
    for (int f = 0; f < 5; ++f) pl.update(tw.world, none);
    CHECK(tw.items->activeCount() == 0);
    pl.update(tw.world, none);
    CHECK(tw.items->activeCount() == 6 && gs.power == 24 && tw.items->at(0).type == ItemPowerBig &&
          tw.items->at(0).state == ItemState::DeathScatter);
    for (int f = 0; f < 30; ++f) pl.update(tw.world, none);
    CHECK(pl.state != Player::Dead && gs.livesRemaining == 0 && gs.bombs == gs.startingBombs);
    // Last life: five full-power items, power 0, no more extends.
    pl.state = Player::Alive;
    tw.items->reset();
    pl.checkCollision(tw.world, pl.positionCenter, {4.0f, 4.0f, 0.0f});
    for (int f = 0; f < 6; ++f) pl.update(tw.world, none);
    CHECK(tw.items->activeCount() == 5 && tw.items->at(0).type == ItemFullPower && gs.power == 0 && gs.extendIndex == -1);
}

static void testSoundQueue() {
    RecordingBackend backend;
    SoundPlayer sp(&backend);
    sp.playSound(5);
    sp.playSound(5);  // same SoundIdx twice in a frame: once
    sp.playSound(2);
    sp.playSound(7);
    sp.playSound(9);  // queue of 3 is full: dropped
    sp.playSound(99);
    sp.flush();
    CHECK(backend.plays.size() == 3);
    if (backend.plays.size() == 3) {
        CHECK(backend.plays[0].voice == 5 && backend.plays[0].sample == 3);
        CHECK(backend.plays[1].voice == 2 && backend.plays[1].sample == 1);
        CHECK(backend.plays[2].voice == 7 && backend.plays[2].sample == 5);
        CHECK(near(backend.plays[0].gain, std::pow(10.0f, -500.0f / 2000.0f)));
    }
    sp.flush();
    CHECK(backend.plays.size() == 3);  // the queue was emptied
    CHECK(std::string(SoundPlayer::sampleFile(0)) == "plst00.wav" && SoundPlayer::sampleForSound(28) == 22);
}

static void testFixedTimestep() {
    using FT = FixedTimestep;
    const FT::Fine tick = FT::Tick(1);
    CHECK(tick.count() == 50'000'000);  // 1/60 s in 1/3e9 s units: exact

    // 60 Hz with vsync jitter: exactly one tick per frame, no interpolation.
    FT c60;
    c60.setDisplay(60, true);
    int ticks = 0;
    bool oneEach = true, alphaOne = true;
    const std::chrono::microseconds jitter[4] = {std::chrono::microseconds(300), std::chrono::microseconds(-300),
                                                 std::chrono::microseconds(450), std::chrono::microseconds(-120)};
    for (int f = 0; f < 600; ++f) {
        const int n = c60.advanceBy(tick + FT::Fine(jitter[f % 4]));
        oneEach = oneEach && n == 1;
        alphaOne = alphaOne && c60.alpha() == 1.0f;
        ticks += n;
    }
    CHECK(ticks == 600 && oneEach && alphaOne);

    // 120 Hz: a tick every second frame, rendered at alpha 0.5 then 1.
    FT c120;
    c120.setDisplay(120, true);
    bool pattern = true;
    ticks = 0;
    for (int f = 0; f < 240; ++f) {
        const int n = c120.advanceBy(FT::Fine(25'000'000));
        ticks += n;
        pattern = pattern && n == (f % 2 == 0 ? 1 : 0) && c120.alpha() == (f % 2 == 0 ? 0.5f : 1.0f);
    }
    CHECK(ticks == 120 && pattern);

    // 90 Hz and 144 Hz: frame boundaries from real (rounded) timestamps.
    for (const int hz : {90, 144}) {
        FT c;
        c.setDisplay(hz, true);
        ticks = 0;
        bool alphaOk = true;
        std::int64_t prev = 0;
        const int frames = hz * 10;
        for (int f = 1; f <= frames; ++f) {
            const std::int64_t now = static_cast<std::int64_t>(std::llround(f * 3e9 / hz));
            ticks += c.advanceBy(FT::Fine(now - prev));
            prev = now;
            alphaOk = alphaOk && c.alpha() > 0.0f && c.alpha() <= 1.0f;
        }
        CHECK(ticks == 600 && alphaOk);  // 10 s of display time = 600 ticks, whatever the rate
    }

    // No vsync, 100 Hz loop: still 60 ticks per second, no drift.
    FT noVsync;
    noVsync.setDisplay(60, false);
    ticks = 0;
    for (int f = 0; f < 1000; ++f) ticks += noVsync.advanceBy(std::chrono::milliseconds(10));
    CHECK(ticks == 600);
    CHECK(noVsync.untilNextTick() >= FT::Fine::zero());

    // A long stall runs at most 4 ticks (the game slows down instead of spiralling).
    CHECK(noVsync.advanceBy(std::chrono::seconds(1)) == FT::kMaxTicksPerFrame && noVsync.alpha() == 1.0f);
}

static void testStageFromArchive() {
    const Bytes archive = syntheticStageArchive();
    dumpIfRequested("SYNTH_ST.DAT", archive);

    auto run = [&](int ticks, RecordingSink* sink, std::string* error) {
        struct Result {
            bool ready;
            std::uint32_t score, grazes, deaths;
            int peakBullets;
            std::uint64_t instructions;
            std::size_t draws;
            int stage;
            std::size_t unported;
            std::size_t bulletDraws, enemyDraws, playerDraws;
            int maxLasers;
            std::uint64_t itemsCollected;
        } r{};
        Vfs vfs;
        vfs.mount(open("SYNTH_ST.DAT", archive));
        TextureLibrary lib;
        const UiArt ui = registerUiArt(lib);
        RecordingBackend backend;
        SoundPlayer sound(&backend);
        Stage stage(lib, ui, &vfs, &sound);
        r.ready = stage.load(1, Normal);
        if (!r.ready) {
            if (error) *error = stage.error();
            return r;
        }
        for (int t = 0; t < ticks; ++t) {
            InputState in;
            in.shoot = true;
            const int phase = (t / 60) % 4;  // sweep the player across the field
            in.left = phase == 1;
            in.right = phase == 3;
            stage.tick(in);
            r.maxLasers = std::max(r.maxLasers, stage.bullets().activeLaserCount());
            if (sink && t % 120 == 119) stage.render(*sink, 0.5f, HudInfo{60.0f, 3});
        }
        r.score = stage.state().score;
        r.grazes = stage.state().grazes;
        r.deaths = stage.state().deaths;
        r.peakBullets = stage.bullets().peakCount();
        r.instructions = stage.enemies().instructionsExecuted();
        r.draws = sink ? sink->items.size() : 0;
        if (sink) {
            for (const auto& it : sink->items) {
                const std::string& n = lib.name(it.texture);
                r.bulletDraws += n.starts_with("data/etama");
                r.enemyDraws += n.starts_with("data/stg1enm");
                r.playerDraws += n.starts_with("data/player00");
            }
        }
        r.stage = stage.stageNumber();
        r.unported = stage.unported().counts().size();
        r.itemsCollected = stage.items().collectedTotal();
        for (const auto& [name, n] : stage.unported().counts()) {
            if (name != "particle effects" && name != "score popups and banners" && name != "game over / continue screen" &&
                !name.starts_with("shot types")) {
                std::fprintf(stderr, "  unexpected unported: %s\n", name.c_str());
            }
        }
        return r;
    };

    std::string err;
    RecordingSink sink;
    const auto a = run(1500, &sink, &err);
    CHECK(a.ready);
    if (!a.ready) {
        std::fprintf(stderr, "  stage: %s\n", err.c_str());
        return;
    }
    CHECK(a.instructions > 300);
    CHECK(a.peakBullets > 20);
    CHECK(a.score > 0);  // shot enemies (damage score and kills)
    CHECK(a.bulletDraws > 20 && a.enemyDraws > 0 && a.playerDraws > 0);
    CHECK(a.unported <= 4);
    CHECK(a.maxLasers >= 1 && a.itemsCollected > 0);
    const auto b = run(1500, nullptr, nullptr);
    CHECK(a.score == b.score && a.itemsCollected == b.itemsCollected && a.grazes == b.grazes && a.deaths == b.deaths && a.peakBullets == b.peakBullets &&
          a.instructions == b.instructions);  // deterministic

    // Without the 2002 archives the stage refuses to start and says why.
    Vfs empty;
    TextureLibrary lib;
    Stage none(lib, registerUiArt(lib), &empty, nullptr);
    CHECK(!none.load(1, Normal) && !none.error().empty());
    RecordingSink msg;
    none.render(msg, 1.0f, HudInfo{});
    CHECK(msg.items.size() > 10);
}

static void testSha256() {
    auto hex = [](std::string_view s) {
        return sha256Hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    };
    CHECK(hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// A tiny PE32 image with shot tables at the 1.02h addresses, filled with invented test
// values (not the game's). Every tier of type t fires the same two entries.
static Bytes syntheticShotExe(std::uint8_t badKind = 0) {
    constexpr std::uint32_t kBase = 0x400000, kSectionVa = 0x76000, kRaw = 0x400, kRawSize = 0x3000;
    Bytes b(kRaw + kRawSize, 0);
    auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(b.data() + at, &v, 2); };
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
    auto putf = [&](std::size_t at, float v) { std::memcpy(b.data() + at, &v, 4); };
    b[0] = 'M';
    b[1] = 'Z';
    put32(0x3c, 0x80);
    std::memcpy(b.data() + 0x80, "PE\0\0", 4);
    put16(0x84, 0x14c);
    put16(0x86, 1);      // one section
    put16(0x94, 0xe0);   // optional header size
    put16(0x98, 0x10b);  // PE32
    put32(0x98 + 28, kBase);
    const std::size_t sec = 0x98 + 0xe0;
    put32(sec + 8, kRawSize);
    put32(sec + 12, kSectionVa);
    put32(sec + 16, kRawSize);
    put32(sec + 20, kRaw);
    auto off = [&](std::uint32_t va) { return static_cast<std::size_t>(va - kBase - kSectionVa + kRaw); };
    for (int t = 0; t < 4; ++t) {
        const std::size_t ch = off(exe102h::kCharacterData + t * 0x18);
        putf(ch, 3.0f + t);
        putf(ch + 4, 1.5f + t);
        const std::uint32_t bullets = 0x478600 + static_cast<std::uint32_t>(t) * 0x100;
        for (int tier = 0; tier < 9; ++tier) {
            const std::size_t pd = off(exe102h::kPowerData[t] + tier * 0x0c);
            put32(pd, 2);
            put32(pd + 4, static_cast<std::uint32_t>(10 * (tier + 1)));
            put32(pd + 8, bullets);
        }
        for (int i = 0; i < 2; ++i) {
            const std::size_t e = off(bullets + static_cast<std::uint32_t>(i) * 0x24);
            put16(e, static_cast<std::uint16_t>(i == 0 ? 4 : 30));  // every
            put16(e + 2, 0);                                        // phase
            putf(e + 4, i == 0 ? 0.0f : -6.0f);                     // offset
            putf(e + 8, 2.0f);
            putf(e + 12, 10.0f);                                    // hitbox
            putf(e + 16, 14.0f);
            putf(e + 20, -1.5f);                                    // angle (radians)
            putf(e + 24, 9.0f);                                     // speed
            put16(e + 28, static_cast<std::uint16_t>(20 + i));      // damage
            b[e + 30] = static_cast<std::uint8_t>(i);               // origin: player, left orb
            b[e + 31] = i == 1 && badKind ? badKind : static_cast<std::uint8_t>(i);  // kind: normal, homing
            put16(e + 32, static_cast<std::uint16_t>(0x400 + 64 + i));
            put16(e + 34, static_cast<std::uint16_t>(i == 0 ? 0 : 0xffff));  // sound / none
        }
    }
    return b;
}

static void testShotDataReader() {
    const Bytes exe = syntheticShotExe();
    ShotData d;
    std::string err;
    CHECK(!loadShotData(exe, d, &err) && err.find("1.02h") != std::string::npos);  // not the real program
    CHECK(loadShotData(exe, d, &err, false) && d.loaded);
    if (d.loaded) {
        CHECK(d.types[2].speed == 5.0f && d.types[2].speedFocus == 3.5f);
        const ShotTier& tier = d.types[1].tiers[3];
        CHECK(tier.threshold == 40 && tier.entries.size() == 2);
        const ShotEntry& a = tier.entries[0];
        const ShotEntry& h = tier.entries[1];
        CHECK(a.kind == ShotNormal && a.origin == ShotFromPlayer && a.every == 4 && a.sound && a.damage == 20);
        CHECK(h.kind == ShotHoming && h.origin == ShotFromLeftOrb && h.offsetX == -6.0f && h.height == 14.0f &&
              h.angle == -1.5f && h.speed == 9.0f && h.script == 0x400 + 65 && !h.sound);
    }
    ShotData bad;
    CHECK(!loadShotData(syntheticShotExe(9), bad, &err, false) && !bad.loaded);
    Bytes cut = exe;
    cut.resize(0x500);
    CHECK(!loadShotData(cut, bad, &err, false));
    CHECK(!Vfs::looksLikeGameExecutable(exe));  // too small to be the game: the scan skips it
}

// Shot data made up for these tests: tier 1 a forward shot every 5 frames plus an entry
// that never fires; tier 2 adds a homing amulet from the left orb; Marisa A missiles and
// Marisa B lasers on their own tiers.
static ShotData testShotData() {
    ShotData d;
    d.loaded = true;
    for (int t = 0; t < 4; ++t) {
        ShotType& st = d.types[static_cast<std::size_t>(t)];
        st.speed = t < 2 ? 4.0f : 5.0f;
        st.speedFocus = st.speed / 2.0f;
        const std::int32_t thresholds[9] = {8, 16, 32, 48, 64, 80, 96, 127, 999};
        ShotEntry main;
        main.width = main.height = 12.0f;
        main.angle = -kZunHalfPi;
        main.speed = 12.0f;
        main.damage = 48;
        main.every = 5;
        main.script = 0x400 + 64;
        main.sound = true;
        ShotEntry never = main;
        never.every = 15;
        never.phase = 20;
        for (int i = 0; i < 9; ++i) {
            ShotTier& tier = st.tiers[static_cast<std::size_t>(i)];
            tier.threshold = thresholds[i];
            tier.entries = {main, never};
        }
        ShotEntry special;
        special.origin = ShotFromLeftOrb;
        special.width = special.height = 12.0f;
        special.angle = -2.0f;
        special.speed = 10.0f;
        special.damage = 14;
        special.every = 30;
        special.script = 0x400 + 65;
        if (t == 2) {  // missile, straight up from the player
            special.kind = ShotMissile;
            special.origin = ShotFromPlayer;
            special.angle = -kZunHalfPi;
            special.speed = 3.0f;
            special.damage = 64;
            special.script = 0x400 + 66;
        } else if (t == 3) {  // laser from the left orb, period 100, timer 0
            special.kind = ShotLaser;
            special.angle = -kZunHalfPi;
            special.speed = 3.0f;
            special.width = 10.0f;
            special.height = 480.0f;
            special.damage = 3;
            special.every = 100;
            special.script = 0x400 + 69;
        } else {
            special.kind = ShotHoming;
        }
        st.tiers[1].entries.push_back(special);
    }
    return d;
}

static void testPlayerShots() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    RecordingSound sound;
    tw.world.sound = &sound;
    ShotData data = testShotData();
    tw.world.shots = &data;
    Player& pl = tw.player;
    GameState& gs = tw.state;
    InputState shoot;
    shoot.shoot = true;
    InputState idle;

    // Respawn state resolves to invulnerable on the first update, which can already shoot.
    pl.reset(tw.world);
    CHECK(pl.state == Player::Spawning && pl.fireTimer() == -1);
    pl.update(tw.world, shoot);
    CHECK(pl.state == Player::Invulnerable && pl.activeBulletCount() == 1 && pl.fireTimer() == 1);
    CHECK(sound.played.size() == 1 && sound.played[0] == 0);
    const Player::Bullet& first = pl.bullet(0);
    CHECK(first.state == Player::ShotState::Fired && first.position.x == 192.0f && first.position.y == 384.0f);
    CHECK(near(first.velocity.y, -12.0f) && first.damage == 48);
    for (int f = 1; f < 30; ++f) pl.update(tw.world, shoot);
    CHECK(pl.activeBulletCount() == 6 && pl.fireTimer() == -1);  // t = 0, 5, ..., 25
    pl.update(tw.world, shoot);
    CHECK(pl.activeBulletCount() == 7 && pl.fireTimer() == 1);  // seamless next cycle

    // A one-frame tap still fires the whole cycle.
    pl.reset(tw.world);
    pl.update(tw.world, shoot);
    for (int f = 1; f < 30; ++f) pl.update(tw.world, idle);
    CHECK(pl.activeBulletCount() == 6);

    // Movement speeds come from the shot type; diagonals are speed / sqrt(2).
    gs.shotType = 2;
    pl.reset(tw.world);
    pl.update(tw.world, idle);
    InputState right;
    right.right = true;
    pl.update(tw.world, right);
    CHECK(pl.positionCenter.x == 197.0f);
    InputState both;
    both.left = both.right = both.up = both.down = true;  // right beats left, up beats down
    pl.update(tw.world, both);
    CHECK(near(pl.positionCenter.x, 197.0f + 5.0f / std::sqrt(2.0f)) && near(pl.positionCenter.y, 384.0f - 5.0f / std::sqrt(2.0f)));
    gs.shotType = 0;

    // Option orbs: hidden below power 8, slide in over 8 frames when focusing.
    pl.reset(tw.world);
    gs.power = 0;
    pl.update(tw.world, idle);
    CHECK(pl.orbState() == Player::OrbState::Hidden);
    gs.power = 8;
    pl.update(tw.world, idle);
    CHECK(pl.orbState() == Player::OrbState::Unfocused);
    CHECK(pl.orbPosition(0).x == pl.positionCenter.x - 24.0f && pl.orbPosition(1).x == pl.positionCenter.x + 24.0f);
    InputState focus;
    focus.focus = true;
    const float focusing[8][2] = {{23.75f, -4}, {23, -8}, {21.75f, -12}, {20, -16}, {17.75f, -20}, {15, -24}, {11.75f, -28}, {8, -32}};
    bool table = true;
    for (int k = 0; k < 8; ++k) {
        pl.update(tw.world, focus);
        table = table && near(pl.positionCenter.x - pl.orbPosition(0).x, focusing[k][0]) &&
                near(pl.orbPosition(0).y - pl.positionCenter.y, focusing[k][1]);
    }
    CHECK(table && pl.orbState() == Player::OrbState::Focused);
    pl.update(tw.world, idle);
    CHECK(near(pl.positionCenter.x - pl.orbPosition(0).x, 8.25f) && near(pl.orbPosition(0).y - pl.positionCenter.y, -28.0f));
    for (int k = 0; k < 7; ++k) pl.update(tw.world, idle);
    for (int k = 0; k < 3; ++k) pl.update(tw.world, focus);  // focusing, k = 3: (21.75, -12)
    pl.update(tw.world, idle);                                // reversal: shows unfocusing with k = 8 - 3 = 5
    CHECK(near(pl.positionCenter.x - pl.orbPosition(0).x, 14.25f) && near(pl.orbPosition(0).y - pl.positionCenter.y, -12.0f));
    CHECK(pl.orbState() == Player::OrbState::Unfocusing);

    // Homing amulet (second tier): steers toward the target the enemies chose last frame.
    pl.reset(tw.world);
    gs.power = 8;
    pl.update(tw.world, shoot);  // fires main shot + amulet from the left orb
    int amulet = -1;
    for (int i = 0; i < Player::kMaxBullets; ++i) {
        if (pl.bullet(i).state != Player::ShotState::Inactive && pl.bullet(i).kind == ShotHoming) amulet = i;
    }
    CHECK(amulet >= 0);
    if (amulet >= 0) {
        const Vec3f before = pl.bullet(amulet).velocity;
        pl.considerHomingTarget({300.0f, 100.0f, 0.0f});
        pl.considerHomingTarget({50.0f, 60.0f, 0.0f});  // higher up: not chosen
        CHECK(pl.homingTarget().x == 300.0f);
        pl.update(tw.world, idle);
        const Player::Bullet& h = pl.bullet(amulet);
        CHECK(h.velocity.x > before.x && h.speed <= 10.0f && h.speed >= 1.0f);
        CHECK(pl.homingTarget().x < -100.0f);  // reset for this frame's enemies
        for (int f = 0; f < 3; ++f) pl.update(tw.world, idle);  // no target: speeds back up to 10
        CHECK(pl.bullet(amulet).speed >= 10.0f || pl.bullet(amulet).state == Player::ShotState::Inactive);
    }

    // Normal shots hit one enemy, then play their hit animation and stop counting.
    pl.reset(tw.world);
    gs.power = 0;
    pl.update(tw.world, shoot);
    const Vec3f at = pl.bullet(0).position;
    bool bomb = true;
    CHECK(pl.calcDamageToEnemy(tw.world, at, {12.0f, 12.0f, 0.0f}, &bomb) == 48 && !bomb);
    CHECK(pl.bullet(0).state == Player::ShotState::Collided && pl.bullet(0).sprite.anmFileIndex == 0x400 + 96);
    CHECK(near(pl.bullet(0).velocity.y, -1.5f));
    CHECK(pl.calcDamageToEnemy(tw.world, at, {12.0f, 12.0f, 0.0f}, &bomb) == 0);
    pl.reset(tw.world);
    gs.bombActive = true;  // during a bomb, shots deal a third (at least 1)
    pl.update(tw.world, shoot);
    CHECK(pl.calcDamageToEnemy(tw.world, pl.bullet(0).position, {12.0f, 12.0f, 0.0f}, &bomb) == 16);
    gs.bombActive = false;

    // Missiles (Marisa A): accelerate upwards, then explode with decaying damage.
    gs.shotType = 2;
    gs.power = 8;
    pl.reset(tw.world);
    pl.update(tw.world, shoot);
    int missile = -1;
    for (int i = 0; i < Player::kMaxBullets; ++i) {
        if (pl.bullet(i).state != Player::ShotState::Inactive && pl.bullet(i).kind == ShotMissile) missile = i;
    }
    CHECK(missile >= 0);
    if (missile >= 0) {
        pl.update(tw.world, idle);
        CHECK(near(pl.bullet(missile).velocity.y, -3.3f));
        const Vec3f mp = pl.bullet(missile).position;
        CHECK(pl.calcDamageToEnemy(tw.world, mp, {4.0f, 4.0f, 0.0f}, &bomb) == 64);
        CHECK(pl.bullet(missile).state == Player::ShotState::Collided && pl.bullet(missile).size.x == 42.0f &&
              pl.bullet(missile).damage == 16);
        CHECK(pl.calcDamageToEnemy(tw.world, mp + Vec3f{18.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 0.0f}, &bomb) == 16);  // blast radius
        CHECK(pl.bullet(missile).damage == 4);
    }

    // Lasers (Marisa B): hang from the orb, refire per period, end on ANM interrupt.
    gs.shotType = 3;
    pl.reset(tw.world);
    pl.update(tw.world, shoot);
    int laser = -1;
    for (int i = 0; i < Player::kMaxBullets; ++i) {
        if (pl.bullet(i).state != Player::ShotState::Inactive && pl.bullet(i).kind == ShotLaser) laser = i;
    }
    CHECK(laser >= 0 && pl.laserTimer(0) == 100);
    if (laser >= 0) {
        pl.update(tw.world, shoot);
        const Player::Bullet& lb = pl.bullet(laser);
        CHECK(near(lb.position.x, pl.orbPosition(0).x) && near(lb.position.y, pl.orbPosition(0).y / 2.0f - 3.0f, 1e-3f));
        CHECK(lb.size.y == pl.orbPosition(0).y && lb.size.x == 10.0f);
        auto activeLasers = [&] {
            int n = 0;
            for (int i = 0; i < Player::kMaxBullets; ++i) {
                n += pl.bullet(i).state != Player::ShotState::Inactive && pl.bullet(i).kind == ShotLaser;
            }
            return n;
        };
        // Per-frame damage while the beam overlaps; it never switches to a hit animation.
        const Vec3f beam{pl.orbPosition(0).x, 100.0f, 0.0f};
        CHECK(pl.calcDamageToEnemy(tw.world, beam, {8.0f, 8.0f, 0.0f}, &bomb) == 3);
        CHECK(pl.calcDamageToEnemy(tw.world, beam, {8.0f, 8.0f, 0.0f}, &bomb) == 3 && lb.state == Player::ShotState::Fired);
        int alive = 0;
        for (int f = 2; f < 31; ++f) {  // timer reaches 70 after 30 frames: interrupt 1 ends it
            pl.update(tw.world, shoot);
            if (f == 29) alive = activeLasers();
        }
        CHECK(alive == 1 && activeLasers() == 0 && pl.laserTimer(0) == 70);
        pl.update(tw.world, shoot);
        CHECK(pl.laserTimer(0) == 69 && activeLasers() == 0);  // no new laser before the timer runs out
        pl.state = Player::Alive;
        pl.checkCollision(tw.world, pl.positionCenter, {4.0f, 4.0f, 0.0f});
        CHECK(pl.state == Player::Dead && pl.laserTimer(0) == 2 && pl.laserTimer(1) == 2);
    }
    {  // a fresh laser refires once its period has passed
        pl.reset(tw.world);
        int fired = 0;
        for (int f = 0; f < 101; ++f) {
            pl.update(tw.world, shoot);
            if (f == 0 || f == 100) fired += pl.laserTimer(0) == 100;
        }
        CHECK(fired == 2);
    }
    gs.shotType = 0;

    // Without the game program a basic forward shot is used.
    tw.world.shots = nullptr;
    pl.reset(tw.world);
    for (int f = 0; f < 30; ++f) pl.update(tw.world, shoot);
    CHECK(pl.activeBulletCount() == 6);
}

static void testBombs() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    RecordingSound sound;
    tw.world.sound = &sound;
    ScreenShake shake;
    tw.world.shake = &shake;
    Player& pl = tw.player;
    GameState& gs = tw.state;
    Bomb& bomb = pl.bomb();
    InputState idle, press;
    press.bomb = true;
    auto run = [&](int frames, const InputState& in) {
        for (int f = 0; f < frames; ++f) pl.update(tw.world, in);
    };
    auto len2 = [](const Vec3f& v) { return std::sqrt(v.x * v.x + v.y * v.y); };
    auto played = [&](int idx) { return std::count(sound.played.begin(), sound.played.end(), idx); };
    const SpriteRegion white{};

    // ---- Reimu A: accepted on the very first update of a stage (countdown 8) ----
    gs.bombs = 3;
    pl.reset(tw.world);
    CHECK(pl.respawnCountdown() == 8 && pl.state == Player::Spawning);
    tw.items->spawn(tw.world, {100.0f, 100.0f, 0.0f}, ItemPoint, ItemState::Falling);
    pl.update(tw.world, press);
    CHECK(gs.bombActive && gs.bombs == 2 && gs.bombsUsed == 1 && bomb.timer() == 1 && bomb.duration() == 300);
    CHECK(pl.state == Player::Invulnerable && pl.invulnerabilityTimer() == 359 && pl.respawnCountdown() == 8);
    CHECK(gs.rank == 14 && played(14) == 1);                                  // rank -2, portrait sound
    CHECK(tw.items->at(0).state == ItemState::Magnet);                        // every item homes in
    CHECK(bomb.liveCancelRects() == 1 && bomb.cancelRect(8).size.x == 256.0f);  // step 0 only
    CHECK(bomb.overlayAlpha() == 2);                                          // 1 * 176 / 60
    pl.update(tw.world, press);  // held: no new bomb
    pl.update(tw.world, idle);
    pl.update(tw.world, press);  // a fresh press while one runs is ignored
    CHECK(gs.bombs == 2 && bomb.timer() == 4 && bomb.liveCancelRects() == 0);
    run(76, idle);
    CHECK(bomb.timer() == 80 && bomb.partState(1) == 0 && bomb.overlayAlpha() == 176);
    const std::uint32_t gen = tw.rng.generations();
    pl.update(tw.world, idle);  // step 80: orb 1 launches in a random direction at speed 4
    CHECK(tw.rng.generations() - gen == 2 && played(13) == 1);
    CHECK(bomb.partState(1) == 1 && bomb.partState(2) == 0);
    const Vec3f pc = pl.positionCenter;
    CHECK(near(len2(bomb.partPosition(1) - pc), 4.0f, 1e-4f));
    CHECK(bomb.region(1).size.x == 48.0f && bomb.region(1).damage == 8 && near(bomb.region(1).center.x, pc.x) &&
          bomb.cancelRect(1).size.x == 48.0f);
    bool hit = false;
    CHECK(pl.calcDamageToEnemy(tw.world, pc, {12.0f, 12.0f, 0.0f}, &hit) == 8 && hit && bomb.region(1).accumulated == 8);
    // Steering: the orb turns toward the target the enemy pass picked.
    const Vec3f target{300.0f, 100.0f, 0.0f};
    pl.considerHomingTarget(target);
    const Vec3f p0 = bomb.partPosition(1), v0 = bomb.partVelocity(1);
    pl.update(tw.world, idle);
    const Vec3f dv = bomb.partVelocity(1) - v0, toTarget = target - p0;
    CHECK(dv.x * toTarget.x + dv.y * toTarget.y > 0.0f && len2(bomb.partVelocity(1)) <= 10.0f);
    // 13 enemy hits (raw region damage >= 100) make it explode on the next step.
    for (int i = 0; i < 12; ++i) pl.calcDamageToEnemy(tw.world, bomb.region(1).center, {4.0f, 4.0f, 0.0f}, &hit);
    CHECK(bomb.region(1).accumulated == 104);
    pl.update(tw.world, idle);
    CHECK(bomb.partState(1) == 2 && bomb.region(1).size.x == 256.0f && bomb.region(1).damage == 200 &&
          bomb.cancelRect(1).size.x == 256.0f && played(15) == 1 && shake.activeCount() == 1);
    pl.update(tw.world, idle);
    CHECK(bomb.partState(1) == 3 && bomb.region(1).size.x == 0.0f);  // the blast lasts one step
    run(26, idle);
    CHECK(bomb.partState(1) == 29);
    pl.update(tw.world, idle);
    CHECK(bomb.partState(1) == 0);
    CHECK(bomb.timer() == 111);
    run(159, idle);  // up to step 269: orbs 2..7 are all out and seeking
    int seeking = 0;
    for (int i = 2; i < 8; ++i) seeking += bomb.partState(i) == 1;
    CHECK(seeking == 6 && played(13) == 7);
    pl.update(tw.world, idle);  // step 270: everything still seeking explodes
    int exploding = 0;
    for (int i = 2; i < 8; ++i) exploding += bomb.partState(i) == 2;
    CHECK(exploding == 6);
    {
        RecordingSink sink;
        bomb.draw(tw.world, sink, white, 1.0f, pl.positionCenter);
        CHECK(sink.items.size() == 1 + 6 * 4);  // overlay + 4 layers per orb
    }
    run(29, idle);
    CHECK(gs.bombActive && bomb.timer() == 300 && bomb.overlayAlpha() == 0);
    pl.update(tw.world, idle);  // end step
    CHECK(!gs.bombActive && pl.state == Player::Invulnerable && pl.invulnerabilityTimer() == 59);
    run(58, idle);
    CHECK(pl.state == Player::Invulnerable);
    pl.update(tw.world, idle);
    CHECK(pl.state == Player::Alive);

    // ---- Deathbomb: the 6 frames after a hit ----
    tw.items->reset();
    pl.reset(tw.world);
    gs.bombs = 3;
    gs.power = 40;
    gs.livesRemaining = 2;
    pl.update(tw.world, idle);
    CHECK(pl.respawnCountdown() == 6);
    pl.state = Player::Alive;
    pl.checkCollision(tw.world, pl.positionCenter, {4.0f, 4.0f, 0.0f});
    CHECK(pl.state == Player::Dead && gs.deaths == 1);
    run(5, idle);
    CHECK(pl.respawnCountdown() == 1 && tw.items->activeCount() == 0);
    pl.update(tw.world, press);  // sixth frame after the hit: still in time
    CHECK(gs.bombActive && pl.state == Player::Invulnerable && tw.items->activeCount() == 0 && gs.power == 40);
    run(40, idle);
    CHECK(gs.livesRemaining == 2 && gs.deaths == 1 && pl.respawnCountdown() == 1);  // not restored (original quirk)
    // One frame too late: the death is committed and the press does nothing.
    tw.items->reset();
    pl.reset(tw.world);
    gs.bombs = 3;
    pl.update(tw.world, idle);
    pl.state = Player::Alive;
    pl.checkCollision(tw.world, pl.positionCenter, {4.0f, 4.0f, 0.0f});
    run(6, idle);
    CHECK(pl.respawnCountdown() == 0 && tw.items->activeCount() == 6 && gs.power == 24);
    pl.update(tw.world, press);
    CHECK(!gs.bombActive && gs.bombs == 3);
    // No bombs left: nothing happens.
    pl.reset(tw.world);
    gs.bombs = 0;
    pl.update(tw.world, press);
    CHECK(!gs.bombActive);

    // ---- Reimu B: four beams fixed where the bomb started ----
    gs.shotType = 1;
    gs.bombs = 3;
    shake.clear(tw.world);
    pl.reset(tw.world);
    pl.update(tw.world, idle);
    const Vec3f start = pl.positionCenter;
    pl.update(tw.world, press);
    CHECK(bomb.duration() == 140 && pl.invulnerabilityTimer() == 199 && bomb.liveRegions() == 0 &&
          bomb.liveCancelRects() == 0 && shake.activeCount() == 1 && played(6) >= 1);
    InputState left;
    left.left = true;
    pl.update(tw.world, left);  // step 1
    CHECK(bomb.liveRegions() == 4 && bomb.liveCancelRects() == 4);
    CHECK(bomb.region(0).size.x == 62.0f && bomb.region(0).size.y == 448.0f && bomb.region(1).size.x == 384.0f &&
          bomb.region(1).size.y == 62.0f && bomb.region(1).damage == 8);
    CHECK(near(bomb.region(2).center.x, start.x) && near(bomb.region(2).center.y, 224.0f) &&
          near(bomb.region(1).center.x, 192.0f) && near(bomb.region(1).center.y, start.y));
    pl.update(tw.world, left);  // step 2: no damage, but bullets are still cancelled
    CHECK(bomb.liveRegions() == 0 && bomb.liveCancelRects() == 4 && near(bomb.cancelRect(2).center.x, start.x));
    run(39, left);  // to step 41
    CHECK(pl.positionCenter.x < start.x - 100.0f);
    CHECK(bomb.region(0).center.x > start.x + 30.0f && near(bomb.region(2).center.x, start.x));  // beam 0 slides (script)
    {
        RecordingSink sink;
        bomb.draw(tw.world, sink, white, 1.0f, pl.positionCenter);
        CHECK(sink.items.size() == 1 + 4);
    }
    run(98, idle);
    CHECK(gs.bombActive && bomb.timer() == 140);
    pl.update(tw.world, idle);
    CHECK(!gs.bombActive);

    // ---- Marisa A: eight stars flying outwards at 2 px/frame ----
    gs.shotType = 2;
    gs.bombs = 3;
    shake.clear(tw.world);
    pl.reset(tw.world);
    pl.update(tw.world, idle);
    const Vec3f mc = pl.positionCenter;
    pl.update(tw.world, press);
    CHECK(bomb.duration() == 250 && pl.invulnerabilityTimer() == 299 && bomb.liveRegions() == 0 && shake.activeCount() == 1);
    pl.update(tw.world, idle);  // step 1
    CHECK(bomb.liveRegions() == 8 && bomb.region(0).size.x == 128.0f && near(bomb.region(0).center.x, mc.x + 2.0f) &&
          near(bomb.region(2).center.y, mc.y + 2.0f, 1e-4f) && near(bomb.region(2).center.x, mc.x, 1e-4f));
    run(2, idle);  // step 3: t mod 3 == 0, nothing live
    CHECK(bomb.liveRegions() == 0 && bomb.liveCancelRects() == 0 && near(bomb.partPosition(4).x, mc.x - 6.0f, 1e-4f));
    {
        RecordingSink sink;
        bomb.draw(tw.world, sink, white, 1.0f, pl.positionCenter);
        CHECK(sink.items.size() == 1 + 8 * 3);  // overlay + three copies per star
    }

    // ---- Marisa B: Master Spark ----
    gs.shotType = 3;
    gs.bombs = 3;
    shake.clear(tw.world);
    tw.bullets->removeAllBullets(tw.world, false);
    pl.reset(tw.world);
    pl.update(tw.world, idle);
    const float x0 = pl.positionCenter.x;
    InputState pressRight = press;
    pressRight.right = true;
    pl.update(tw.world, pressRight);  // 0.3x movement from the press frame
    CHECK(near(pl.positionCenter.x, x0 + 1.5f) && bomb.moveMultiplier() == 0.3f && played(19) >= 1);
    InputState shoot;
    shoot.shoot = true;
    pl.update(tw.world, shoot);  // step 1: no new shots while sparking
    CHECK(pl.activeBulletCount() == 0);
    CHECK(bomb.liveRegions() == 1 && bomb.region(0).damage == 12 && bomb.region(0).size.x == 384.0f &&
          near(bomb.region(0).size.y, pl.positionCenter.y) && near(bomb.region(0).center.y, pl.positionCenter.y / 2.0f));
    run(2, idle);
    pl.update(tw.world, idle);  // step 4: off
    CHECK(bomb.liveRegions() == 0 && bomb.liveCancelRects() == 0);
    // Every fired bullet touching a live cancel rectangle becomes a homing point item.
    EnemyShooter s;
    s.sprite = 3;
    s.position = {192.0f, 50.0f, 0.0f};
    s.aimMode = Circle;
    s.count1 = 10;
    s.count2 = 1;
    s.speed1 = s.speed2 = 1.0f;
    tw.bullets->spawnBulletPattern(tw.world, s);
    tw.items->reset();
    pl.update(tw.world, idle);  // step 5: on
    tw.bullets->update(tw.world);
    int stars = 0;
    for (int i = 0; i < ItemManager::kMaxItems; ++i) {
        stars += tw.items->at(i).active && tw.items->at(i).type == ItemStar && tw.items->at(i).state == ItemState::Magnet;
    }
    CHECK(stars == 10);
    {
        RecordingSink sink;
        bomb.draw(tw.world, sink, white, 1.0f, pl.positionCenter);
        CHECK(sink.items.size() == 1 + 4);
    }
    // Enemy bodies touching a cancel rectangle cannot hurt the player.
    pl.update(tw.world, idle);  // step 6: on
    CHECK(pl.checkCollision(tw.world, pl.positionCenter - Vec3f{0.0f, 2.0f, 0.0f}, {4.0f, 4.0f, 0.0f}) ==
          Player::CollisionCanceledByBomb);
    // Name banner: slides in, then leaves after the bomb.
    for (int i = 0; i < 16; ++i) bomb.updateGui();
    CHECK(near(bomb.nameVisibility(), 1.0f) && std::string(bomb.name()).find("MASTER SPARK") != std::string::npos);
    run(293, idle);
    CHECK(gs.bombActive && bomb.timer() == 300);
    pl.update(tw.world, idle);
    CHECK(!gs.bombActive && bomb.moveMultiplier() == 1.0f);
    for (int i = 0; i < 32; ++i) bomb.updateGui();
    CHECK(bomb.nameVisibility() == 0.0f);
    gs.shotType = 0;

    // Graze still scores during a bomb, but the graze counters stay.
    gs.bombActive = true;
    const std::uint32_t grazes = gs.grazes, score = gs.score;
    addGraze(tw.world, tw.player.positionCenter);
    CHECK(gs.grazes == grazes && gs.score == score + 500);
    gs.bombActive = false;

    // A bomb during no spell card: nothing to fail, later cards take no bomb damage.
    tw.enemies->onPlayerBomb();
    CHECK(!tw.enemies->spellcard().usedBomb && !tw.enemies->spellcard().isCapturing);
}

static void testScreenShake() {
    TestWorld tw;
    GameState& gs = tw.state;
    ScreenShake shake;
    shake.add(16, 8.0f, 0.0f);
    bool ok = true;
    for (int f = 1; f < 16; ++f) {
        const std::uint32_t gen = tw.rng.generations();
        shake.update(tw.world);
        const float o = 8.0f - static_cast<float>(f) / 2.0f;
        ok = ok && tw.rng.generations() - gen == 4;  // two 32-bit draws (x, then y)
        ok = ok && (gs.playfieldX == 32.0f || near(gs.playfieldX, 32.0f + o));
        ok = ok && (gs.playfieldW == 384.0f || near(gs.playfieldW, 384.0f - o));
        ok = ok && (gs.playfieldY == 16.0f || near(gs.playfieldY, 16.0f + o));
        ok = ok && (gs.playfieldX == 32.0f || gs.playfieldW < 384.0f);  // shifting always shrinks
    }
    CHECK(ok && shake.activeCount() == 1);
    shake.update(tw.world);
    CHECK(shake.activeCount() == 0 && gs.playfieldX == 32.0f && gs.playfieldY == 16.0f && gs.playfieldW == 384.0f &&
          gs.playfieldH == 448.0f);
    // The shrunken size is what the on-screen tests use.
    gs.playfieldW = 300.0f;
    CHECK(!gs.isInBounds(320.0f, 100.0f, 10.0f, 10.0f) && gs.isInBounds(290.0f, 100.0f, 10.0f, 10.0f));
    gs.resetPlayfield();
}

static void testEffects() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    EffectManager& em = *tw.effects;
    const Vec3f at{100.0f, 120.0f, 0.0f};

    // Ring pool: consecutive free slots from a rolling cursor; busy slots are skipped.
    CHECK(em.spawn(tw.world, EffectParticleWhite, at, 3, 0xffffffffu) == 2 && em.cursor() == 3);
    em.at(4).inUse = true;
    CHECK(em.spawn(tw.world, EffectParticleWhite, at, 2, 0xffffffffu) == 5 && em.at(3).inUse && em.cursor() == 6);
    // The spawn colour replaces what script frame 0 set.
    const int red = em.spawn(tw.world, EffectFlash, at, 1, 0xff4040ffu);
    CHECK(em.at(red).vm.color == 0xff4040ffu && em.at(red).vm.additive && em.at(red).pos.x == 100.0f);
    // A count <= 0 fills every free slot and returns the sink; a full pool drops spawns.
    CHECK(em.spawn(tw.world, EffectFlash, at, 0, 0xffffffffu) == EffectManager::kSink && em.activeCount() == 512);
    const int cursor = em.cursor();
    CHECK(em.spawn(tw.world, EffectFlash, at, 1, 0xffffffffu) == EffectManager::kSink && em.cursor() == cursor);

    // Splash: two draws (x, then y) on the first update, decelerating to rest over 19 frames.
    em.reset();
    const int sp = em.spawn(tw.world, EffectParticleWhite, at, 1, 0xffffffffu);
    ZunRng predict = tw.rng;
    const float vx = (predict.f32InRange(256.0f) - 128.0f) / 12.0f;
    const float vy = (predict.f32InRange(256.0f) - 128.0f) / 12.0f;
    std::uint32_t gen = tw.rng.generations();
    em.update(tw.world);
    CHECK(tw.rng.generations() - gen == 4);
    CHECK(near(em.at(sp).pos.x, at.x + vx, 1e-4f) && near(em.at(sp).pos.y, at.y + vy, 1e-4f));
    for (int f = 1; f < 19; ++f) em.update(tw.world);
    CHECK(near(em.at(sp).pos.x, at.x + 10.0f * vx, 1e-3f) && near(em.at(sp).velocity.x, 0.0f, 1e-4f));
    gen = tw.rng.generations();
    em.update(tw.world);  // later updates draw nothing; script frame 20 deletes it
    CHECK(tw.rng.generations() == gen && !em.at(sp).inUse);

    // Big splash: 4/33 per unit, stops after 20 frames.
    em.reset();
    const int bs = em.spawn(tw.world, EffectGlowBig, at, 1, 0xffffffffu);
    predict = tw.rng;
    const float bx = (predict.f32InRange(256.0f) - 128.0f) * 4.0f / 33.0f;
    em.update(tw.world);
    CHECK(near(em.at(bs).pos.x, at.x + bx, 1e-4f) && near(em.at(bs).acceleration.x, -bx / 20.0f, 1e-5f));

    // Converge: appears 256 px out in a random direction, reaches its spawn point after 60.
    em.reset();
    const int cv = em.spawn(tw.world, EffectConverge, at, 1, 0xffffffffu);
    gen = tw.rng.generations();
    em.update(tw.world);
    const Vec3f out = em.at(cv).pos - at;
    CHECK(tw.rng.generations() - gen == 2 && near(std::sqrt(out.x * out.x + out.y * out.y), 256.0f, 1e-3f));
    CHECK(em.at(cv).prevPos.x == em.at(cv).pos.x);  // no streak from the spawn point
    for (int f = 1; f < 61; ++f) em.update(tw.world);
    CHECK(near(em.at(cv).pos.x, at.x, 1e-3f) && near(em.at(cv).pos.y, at.y, 1e-3f));

    // Orbit: r(cos 2a, 0, -6 sin 2a) about a y axis; the fade lasts 15 drawn frames.
    em.reset();
    const int ob = em.spawn(tw.world, EffectOrbit, at, 1, 0xff2020ffu);
    EffectManager::Effect& o = em.at(ob);
    o.axis = {0.0f, 2.0f, 0.0f};
    o.anchor = at;
    o.radius = 10.0f;
    o.halfAngle = 0.0f;
    em.update(tw.world);
    CHECK(near(o.pos.x, 110.0f, 1e-4f) && near(o.pos.y, 120.0f, 1e-4f) && near(o.pos.z, 0.0f, 1e-4f));
    o.halfAngle = kZunPi / 4.0f;
    em.update(tw.world);
    CHECK(near(o.pos.x, 100.0f, 1e-4f) && near(o.pos.z, -60.0f, 1e-3f));
    o.fading = true;
    em.update(tw.world);
    CHECK(o.inUse && (o.vm.color >> 24) == 239 && near(o.vm.scaleX, 1.0625f) && (o.vm.color & 0xffffffu) == 0x2020ffu);
    for (int k = 2; k < 16; ++k) em.update(tw.world);
    CHECK(o.inUse && (o.vm.color >> 24) == 15);
    em.update(tw.world);
    CHECK(!o.inUse);
    // Depth pulls it toward the playfield centre and shrinks it when drawn.
    em.reset();
    const int dz = em.spawn(tw.world, EffectFlash, {0.0f, 0.0f, 0.0f}, 1, 0xffffffffu);
    em.at(dz).pos = em.at(dz).prevPos = {0.0f, 0.0f, 300.0f};
    em.update(tw.world);
    em.at(dz).pos = em.at(dz).prevPos = {0.0f, 0.0f, 300.0f};
    {
        RecordingSink sink;
        em.draw(tw.world, sink, 1.0f);
        const float d = 224.0f / std::tan(15.0f * kZunPi / 180.0f), sc = d / (d + 300.0f);
        CHECK(sink.items.size() == 1 && near(sink.items[0].s.x, 32.0f + 192.0f - 192.0f * sc, 1e-3f) &&
              near(sink.items[0].s.w, 32.0f * sc, 1e-3f));
    }

    // Spawn sites: shot hits, grazes and deaths.
    em.reset();
    Player& pl = tw.player;
    pl.reset(tw.world);
    InputState shoot;
    shoot.shoot = true;
    pl.update(tw.world, shoot);
    bool bomb = false;
    pl.calcDamageToEnemy(tw.world, pl.bullet(0).position, {12.0f, 12.0f, 0.0f}, &bomb);
    CHECK(em.activeCount() == 1 && em.at(0).type == EffectParticleRed);
    addGraze(tw.world, {50.0f, 60.0f, 0.0f});
    CHECK(em.activeCount() == 2 && em.at(1).type == EffectParticleSmall && em.at(1).pos.x == 50.0f);
    pl.state = Player::Alive;
    pl.checkCollision(tw.world, pl.positionCenter, {4.0f, 4.0f, 0.0f});
    CHECK(em.activeCount() == 2 + 17 && em.at(2).type == EffectFlash && em.at(2).vm.color == 0xff4040ffu &&
          em.at(3).type == EffectParticleGreen);
}

static void testEffectOpcodes() {
    TestWorld tw;
    CHECK(tw.ok);
    if (!tw.ok) return;
    EffectManager& em = *tw.effects;
    EclBuilder e;
    e.spawn(0, timeline_op::EnemyCreate, 0, 192.0f, 100.0f, 100, 3, 0);
    e.sub();
    e.ins(0, ecl_op::SpellcardEffect, {5, fbits(0.0f), fbits(1.0f), fbits(0.0f), fbits(32.0f)});
    e.ins(0, ecl_op::EffectParticle, {static_cast<std::uint32_t>(EffectParticleRed), 3, 0xff00ff00u});
    e.ins(0, ecl_op::AnmDeathEffects, {0x0201u});  // explosion type 1, particle selector 2
    e.ins(1000, ecl_op::EnemyDelete);
    std::string err;
    CHECK(tw.ecl.load(e.build(), &err));
    tw.enemies->startStage(tw.world);
    Enemy& en = tw.enemies->enemy(0);
    tw.enemies->update(tw.world);  // spawn: the circle, three particles
    CHECK(em.activeCount() == 4 && en.effectCount == 1 && en.effectRefs[0] == 0 && en.effectDistance == 32.0f);
    CHECK(em.at(0).type == EffectOrbit && em.at(0).vm.color == 0xffff2020u && em.at(0).axis.y == 1.0f);
    CHECK(em.at(1).vm.color == 0xff00ff00u && en.deathParticle1 == 1 && en.deathParticle2 == 2);
    // The enemy drives its circle: anchor, radius +0.3 a frame (up to 32), angle +pi/100.
    CHECK(near(em.at(0).radius, 0.3f) && near(em.at(0).halfAngle, kZunPi / 100.0f) && em.at(0).anchor.x == 192.0f);
    for (int f = 0; f < 120; ++f) tw.enemies->update(tw.world);
    CHECK(em.at(0).radius >= 32.0f && em.at(0).radius < 32.3f);
    // Killed (drop code 3 = fixed item): 3 particles before the item, then explosion + 4 particles.
    const int before = em.activeCount();
    en.life = 0;
    tw.enemies->update(tw.world);
    CHECK(em.activeCount() == before + 3 + 1 + 4);
    int selected = 0, explosion = 0;
    for (int i = 0; i < EffectManager::kMaxEffects; ++i) {
        selected += em.at(i).inUse && em.at(i).type == 2 + 4;
        explosion += em.at(i).inUse && em.at(i).type == 1;
    }
    CHECK(selected == 7 && explosion == 1);
    // A non-boss death does not release its circle.
    CHECK(!em.at(0).fading);
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
        {"mmap / fd fallback / directory mounting", testMappedFileAndDirectory},
        {"ZUN RNG, angles, rank", testZunMath},
        {"ANM VM: bytecode, interrupts, drawing", testAnmVm},
        {"ECL VM: timeline, subs, vars, calls, masks", testEclVm},
        {"bullets: aim modes, spawn effects, 640 slots", testBullets},
        {"items: physics, pickup rules, power, stars", testItems},
        {"item drops: random table, DROP_ITEMS", testItemDrops},
        {"lasers: phases, hitbox quirks, graze, drawing", testLasers},
        {"laser ECL opcodes", testLaserOpcodes},
        {"score roll, extends, clear bonus, death drops", testScoreAndLives},
        {"SHA-256", testSha256},
        {"shot tables from a PE32 executable", testShotDataReader},
        {"player shots: cycle, orbs, homing, missiles, lasers", testPlayerShots},
        {"bombs: all four, deathbomb, cancel, damage, drawing", testBombs},
        {"screen shake", testScreenShake},
        {"effects: pool, motion, RNG, orbit, drawing, spawn sites", testEffects},
        {"effect ECL opcodes: spell circles, particles, deaths", testEffectOpcodes},
        {"sound queue (PlaySoundByIdx / PlaySounds)", testSoundQueue},
        {"fixed 60 Hz timestep at 60/90/120/144 Hz", testFixedTimestep},
        {"whole stage from a synthetic PBG3 archive", testStageFromArchive},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-55s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
