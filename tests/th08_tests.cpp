// Host tests for Imperishable Night's game code (th08_game) and the engine pieces it added:
// the block cipher (io/ThCrypt), the PBGZ archive and the th08.exe 1.00d check. Synthetic
// data only; with TH08_REAL_DAT=<path to th08.dat> the player's own archive is also opened
// and a few members are checked against the spec's SHA-256 values (never in CI).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/Sha256.hpp"
#include "io/Archive.hpp"
#include "io/Lzss.hpp"
#include "io/MappedFile.hpp"
#include "io/ThCrypt.hpp"
#include "io/Vfs.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"
#include "th08/Anm.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Flow.hpp"
#include "th08/MainMenu.hpp"
#include "th08/ScoreFile.hpp"
#include "th08/GameConfig.hpp"
#include "th08/GameFiles.hpp"
#include "th08/Version.hpp"

using namespace touhou;
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

Bytes hex(const char* s) {
    Bytes out;
    for (const char* p = s; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return out;
}

Bytes iota(std::size_t n) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(i);
    return b;
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

// ---- synthetic version-3 ANM files ---------------------------------------------------------

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
    std::vector<std::pair<int, std::vector<Ins>>> scripts;  // (id, code): ids are ignored by TH08
    int width = 16, height = 16, format = 1;                // THTX format; 0: blank '@' texture
};
Bytes anmFile(const std::vector<EntryDef>& entries) {
    Bytes file;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        const EntryDef& d = entries[e];
        Bytes ent(0x40 + 4 * d.sprites.size() + 8 * d.scripts.size(), 0);
        set32(ent, 0x00, static_cast<std::uint32_t>(d.sprites.size()));
        set32(ent, 0x04, static_cast<std::uint32_t>(d.scripts.size()));
        set32(ent, 0x0C, static_cast<std::uint32_t>(d.width));
        set32(ent, 0x10, static_cast<std::uint32_t>(d.height));
        set32(ent, 0x14, static_cast<std::uint32_t>(d.format));
        set32(ent, 0x28, 3);
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
            for (char c : std::string("THTX")) ent.push_back(static_cast<std::uint8_t>(c));
            put16(ent, 0);
            put16(ent, static_cast<std::uint16_t>(d.format));
            put16(ent, static_cast<std::uint16_t>(d.width));
            put16(ent, static_cast<std::uint16_t>(d.height));
            put32(ent, 0);
            for (int i = 0; i < d.width * d.height; ++i) put32(ent, 0xFF000000u | static_cast<std::uint32_t>(e));
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

void testCipher() {
    // The archive header of the player's th08.dat (spec_files 1.2).
    {
        Bytes b = hex("1b 9f 8b 27 f7 cd 6d 26 1c 1a 40 05");
        thcrypt::decrypt(b, {0x1B, 0x37, 12, 0x400});
        CHECK(b == hex("7d e3 01 00 10 e7 cf 02 ba cd 08 00"));
    }
    // The spec's vectors (1.3): stored 00..0b and 00..0c decrypted with the given parameters.
    {
        Bytes b = iota(12);
        thcrypt::decrypt(b, {0x1B, 0x37, 12, 0x400});
        CHECK(b == hex("73 2b 4b f3 03 c3 db 8b 9b 53 63 1b"));
        thcrypt::encrypt(b, {0x1B, 0x37, 12, 0x400});
        CHECK(b == iota(12));
    }
    {
        Bytes b = iota(13);
        thcrypt::decrypt(b, {0x1B, 0x37, 8, 0x400});
        CHECK(b == hex("9b c3 63 8b 2b 53 f3 1b 73 03 4b db 0c"));
        CHECK(b[12] == 0x0C);  // the odd 13th byte passes
    }
    {
        Bytes b(10, 0);
        thcrypt::decrypt(b, {0x35, 0x97, 0x80, 0x2800});
        CHECK(b == Bytes(10, 0));  // 10 < 0x80 / 4: nothing is touched
    }
    // The limit: only the first block(s) up to the limit change.
    {
        Bytes b(0x100, 0);
        thcrypt::decrypt(b, {0x3E, 0x9B, 0x80, 0x80});
        bool tailUntouched = true;
        for (std::size_t i = 0x80; i < b.size(); ++i) tailUntouched = tailUntouched && b[i] == 0;
        CHECK(tailUntouched);
        CHECK(b[0x7F] == 0x3E);  // the first stored byte becomes the block's last
    }
    // Round trips over sizes, blocks and limits.
    std::mt19937 rng(8);
    bool all = true;
    for (int c = 0; c < 2000; ++c) {
        const std::size_t n = rng() % 300;
        const thcrypt::Params p{static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()),
                                static_cast<std::uint32_t>(1 + rng() % 64), static_cast<std::uint32_t>(rng() % 400)};
        Bytes b(n);
        for (auto& x : b) x = static_cast<std::uint8_t>(rng());
        const Bytes orig = b;
        thcrypt::encrypt(b, p);
        thcrypt::decrypt(b, p);
        all = all && b == orig;
    }
    CHECK(all);
}

struct Member {
    std::string name;
    char tag;
    thcrypt::Params crypt;
    Bytes data;
};

// A PBGZ archive built the way the spec describes it.
Bytes pbgz(const std::vector<Member>& members) {
    Bytes out(16, 0);
    std::memcpy(out.data(), "PBGZ", 4);
    Bytes table;
    for (const Member& m : members) {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        Bytes body = m.data;
        thcrypt::encrypt(body, m.crypt);
        Bytes plain = {'e', 'd', 'z', static_cast<std::uint8_t>(m.tag)};
        plain.insert(plain.end(), body.begin(), body.end());
        const Bytes packed = lzss::compress(plain);
        out.insert(out.end(), packed.begin(), packed.end());
        table.insert(table.end(), m.name.begin(), m.name.end());
        table.push_back(0);
        put32(table, offset);
        put32(table, static_cast<std::uint32_t>(plain.size()));
        put32(table, 0);
    }
    const std::uint32_t tableOffset = static_cast<std::uint32_t>(out.size());
    table.resize(table.size() + 37, 0);  // zero padding, as in the shipped table
    Bytes packed = lzss::compress(table);
    thcrypt::encrypt(packed, {0x3E, 0x9B, 0x80, 0x400});
    out.insert(out.end(), packed.begin(), packed.end());
    Bytes head;
    put32(head, static_cast<std::uint32_t>(members.size()) + 123456u);
    put32(head, tableOffset + 345678u);
    put32(head, static_cast<std::uint32_t>(table.size()) + 567891u);
    thcrypt::encrypt(head, {0x1B, 0x37, 12, 0x400});
    std::memcpy(out.data() + 4, head.data(), 12);
    return out;
}

std::unique_ptr<Archive> archiveOf(const std::vector<std::pair<std::string, Bytes>>& files) {
    std::vector<Member> members;
    for (const auto& [name, data] : files) members.push_back({name, '-', {0x35, 0x97, 0x80, 0x2800}, data});
    std::string err;
    return openArchive("th08.dat", MappedFile::fromMemory(pbgz(members)), &err);
}

void testPbgz() {
    std::mt19937 rng(17);
    auto random = [&](std::size_t n) {
        Bytes b(n);
        for (auto& x : b) x = static_cast<std::uint8_t>(rng() % 7);  // compressible
        return b;
    };
    const std::vector<Member> members = {
        {"title01.anm", 'A', {0xC1, 0x51, 0x1400, 0x2000}, random(30000)},
        {"ecldata1.ecl", 'E', {0xAB, 0xCD, 0x200, 0x1000}, random(5001)},
        {"msg1a.dat", '-', {0x35, 0x97, 0x80, 0x2800}, random(777)},
        {"musiccmt.txt", 'T', {0x51, 0xE9, 0x40, 0x3000}, random(64)},
        {"se_ok00.wav", 'W', {0x12, 0x34, 0x400, 0x2800}, random(3)},
    };
    const Bytes file = pbgz(members);
    std::string err;
    auto archive = openArchive("th08.dat", MappedFile::fromMemory(file), &err);
    CHECK(archive != nullptr);
    if (!archive) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return;
    }
    CHECK(archive->format() == "PBGZ");
    CHECK(archive->entries().size() == members.size());
    for (std::size_t i = 0; i < members.size() && i < archive->entries().size(); ++i) {
        CHECK(archive->entries()[i].name == members[i].name);
        CHECK(archive->entries()[i].size == members[i].data.size());  // without the prefix
        Bytes out;
        CHECK(archive->extract(i, out, &err));
        CHECK(out == members[i].data);
    }
    Vfs vfs;
    CHECK(vfs.mount(std::move(archive)));
    CHECK(vfs.hasFormat("PBGZ"));
    const auto msg = vfs.read("data/MSG1A.DAT");
    CHECK(msg.has_value() && *msg == members[2].data);

    // An unknown type tag or a damaged prefix is refused.
    {
        std::vector<Member> bad = {{"x.bin", 'Q', {0x35, 0x97, 0x80, 0x2800}, random(100)}};
        auto a = openArchive("th08.dat", MappedFile::fromMemory(pbgz(bad)), &err);
        CHECK(a != nullptr);
        Bytes out;
        CHECK(a && !a->extract(0, out, &err));
    }
    // A damaged header (zero entries after un-biasing) does not open.
    {
        Bytes broken = file;
        Bytes head;
        put32(head, 123456u);
        put32(head, 16 + 345678u);
        put32(head, 16 + 567891u);
        thcrypt::encrypt(head, {0x1B, 0x37, 12, 0x400});
        std::memcpy(broken.data() + 4, head.data(), 12);
        CHECK(openArchive("th08.dat", MappedFile::fromMemory(broken), &err) == nullptr);
    }
}

// An archive holding the given members (all with the '-' parameters).
std::unique_ptr<Archive> archiveOf(const std::vector<std::pair<std::string, Bytes>>& files);

void testAnm() {
    using th08::AnmVm;
    // Two entries; scripts and sprites are numbered by position across the whole file, and
    // the id fields (negative here, as in the shipped files) are ignored.
    EntryDef e0;
    e0.sprites = {{-5, 0, 0, 8, 4}, {-4, 8, 0, 8, 8}};
    e0.scripts = {{-27, {{3, 0, {0}}, {7, 0, {fbits(2.0f), fbits(3.0f)}}, {9, 0, {0x11, 0x22, 0x33}}, {8, 0, {0x80}}, {20, 0, {}},
                         {21, 0, {5}}, {9, 0, {10, 20, 30}}, {89, 0, {}}, {-1, 0, {}}}},
                  {-26, {{3, 0, {1}}, {82, 0, {2}}, {16, 1, {7}}, {82, 2, {3}}, {83, 2, {3}}, {88, 2, {0x100}}, {84, 2, {1, 2, 3}},
                         {85, 2, {200}}, {86, 2, {4, 0, 41, 42, 43}}, {87, 2, {4, 0, 100}}, {33, 2, {4, 0, 250, 251, 252}},
                         {20, 3, {}}, {-1, 0, {}}}}};
    EntryDef e1;
    e1.sprites = {{900, 0, 0, 16, 16}};
    e1.scripts = {{7, {{3, 0, {2}}, {89, 1, {}}, {-1, 0, {}}}}};
    const Bytes anm = anmFile({e0, e1});
    std::string err;
    Vfs vfs;
    CHECK(vfs.mount(archiveOf({{"test.anm", anm}})));
    TextureLibrary textures;
    th08::AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "test.anm", 4, &err));
    const th08::AnmFile* f = lib.file(4);
    CHECK(f && f->entries == 2 && f->sprites.size() == 3 && f->scripts.size() == 3);
    CHECK(f && f->sprites[2].pixelWidth == 16 && f->textures.size() == 2 && f->sprites[2].texture == f->textures[1]);
    CHECK(!lib.load(vfs, textures, "test.anm", 25, &err));  // 25 usable slots

    ZunRng rng(1);
    AnmVm vm;
    vm.start(lib, rng, 4, 0);
    // Sprite 0 selected (its size copied into the VM), scale, RGB from three arguments,
    // alpha, then parked on a stop.
    CHECK(vm.sprite == &f->sprites[0] && vm.spriteWidth == 8 && vm.spriteHeight == 4);
    CHECK(vm.color == 0x80112233u && vm.has(AnmVm::Stopped));
    // Interrupt 5 saves the return point; opcode 89 goes back to the stop.
    vm.interrupt(5);
    vm.tick(rng);
    CHECK(vm.color == 0x800A141Eu && vm.has(AnmVm::Stopped) && !vm.finished());
    // A frozen VM does nothing, not even its clock.
    const std::int32_t clockBefore = vm.clock.current;
    vm.set(AnmVm::Frozen, true);
    vm.interrupt(5);
    vm.tick(rng);
    CHECK(vm.clock.current == clockBefore && vm.pendingInterrupt == 5);
    vm.set(AnmVm::Frozen, false);

    // Blend modes, draw mode, the alternative colour and its channels.
    AnmVm b;
    b.start(lib, rng, 4, 1);
    CHECK(b.blend() == 2);
    b.tick(rng);
    CHECK(b.blend() == 1);  // opcode 16: any non-zero is 1
    b.tick(rng);
    CHECK(b.blend() == 3 && b.drawMode == 3 && b.has(AnmVm::UseAltColor));
    CHECK((b.altColor & 0x00FFFFFFu) != 0x00010203u);  // channel 5 started from (1, 2, 3)
    for (int i = 0; i < 6; ++i) b.tick(rng);
    CHECK(b.altColor == ((100u << 24) | 0x292A2Bu) && (b.color & 0x00FFFFFFu) == 0xFAFBFCu);

    // Opcode 89 without an interrupt: the original crashes; the port finishes the script.
    AnmVm c;
    c.start(lib, rng, 4, 2);
    CHECK(c.sprite == &f->sprites[2]);
    c.tick(rng);
    CHECK(c.finished());

    // A missing script or file clears the VM.
    AnmVm d;
    d.start(lib, rng, 5, 0);
    CHECK(d.finished() && d.color == 0 && d.file == nullptr);

    // Drawing: sizes from the VM copy; blend 2/3 keep the last destination blend.
    RecordingSink sink;
    th08::AnmDrawState state;
    AnmVm v;
    v.start(lib, rng, 4, 0);
    v.position = {100, 50, 0};
    v.color = 0xFFFFFFFFu;
    v.spriteWidth = 20;  // game code resizes the quad without touching the sprite
    v.setBlend(1);
    th08::drawVm(sink, v, state);
    v.setBlend(2);
    th08::drawVm(sink, v, state);
    CHECK(sink.items.size() == 2 && sink.items[0].s.w == 40 && sink.items[0].s.h == 12);
    CHECK(sink.items[0].blend == Blend::Additive && sink.items[1].blend == Blend::Additive);
    th08::noteAlphaBlendDrawn();
    th08::drawVm(sink, v, state);
    CHECK(sink.items.size() == 3 && sink.items[2].blend == Blend::Alpha);

    // A file that is not version 3 is refused.
    Bytes old = anm;
    set32(old, 0x28, 2);
    Vfs vfs2;
    CHECK(vfs2.mount(archiveOf({{"old.anm", old}})));
    CHECK(!lib.load(vfs2, textures, "old.anm", 6, &err) && !lib.loaded(6));
}

void testMenuInput() {
    th08::MenuInput in;
    in.sample(th08::BtnDown);
    CHECK(in.pressed(th08::BtnDown) && in.pressedOrRepeat(th08::BtnDown));
    int repeats = 0, first = -1;
    for (int pass = 1; pass <= 60; ++pass) {
        in.sample(th08::BtnDown);
        CHECK(!in.pressed(th08::BtnDown));
        if (in.pressedOrRepeat(th08::BtnDown)) {
            ++repeats;
            if (first < 0) first = pass;
        }
    }
    CHECK(first == 33 && repeats == 4);  // the first repeat 33 passes after the press, then every 8
    in.sample(0);
    in.sample(th08::BtnShoot | th08::BtnDown);
    CHECK(in.pressed(th08::kSelect) && !in.pressed(th08::kCancel));
}

// A score image: header, TH8K, then the given records.
Bytes scoreRecord(const char* tag, std::uint16_t len, std::uint8_t version) {
    Bytes r(len, 0);
    std::memcpy(r.data(), tag, 4);
    r[4] = static_cast<std::uint8_t>(len), r[5] = static_cast<std::uint8_t>(len >> 8);
    r[6] = r[4], r[7] = r[5];
    r[8] = version;
    return r;
}

void testScoreFile() {
    Bytes image(0x1C, 0);
    auto add = [&](const Bytes& r) { image.insert(image.end(), r.begin(), r.end()); };
    add(scoreRecord("TH8K", 0x0C, 1));
    for (int i = 0; i < 13; ++i) {
        Bytes c = scoreRecord("CLRD", 0x24, 4);
        for (int d = 0; d < 5; ++d) c[0x0C + 2 * d] = c[0x16 + 2 * d] = 1;
        c[0x21] = static_cast<std::uint8_t>(i);
        if (i == 1) c[0x0C + 2 * 2 + 1] = 0x40;  // team 1: Final B on Hard without a continue
        if (i == 2) c[0x16 + 2 * 0 + 1] = 0x80;  // team 2: Final A on Easy (with continues)
        add(c);
    }
    Bytes plst = scoreRecord("PLST", 0x228, 2);
    plst[0x2C + 6 * 0x44] = 42;
    add(plst);
    Bytes flsp = scoreRecord("FLSP", 0x20, 1);
    flsp[0x0C + 3] = 208;
    add(flsp);
    const Bytes file = th08::encodeScoreFile(image, 0x53, 0x41);
    const auto decoded = th08::decodeScoreFile(file);
    CHECK(decoded.has_value());
    th08::ScoreData sd;
    CHECK(decoded && th08::parseScoreImage(*decoded, sd));
    CHECK(sd.totalPlays == 42 && sd.lastWords[3] == 208);
    CHECK(sd.extraUnlocked() && sd.spellPracticeUnlocked() && !sd.solosUnlocked());
    CHECK(sd.teamHasNoContinueFinalB(1) && !sd.teamHasNoContinueFinalB(0) && sd.teamHasFinalA(2));
    // Damage is caught: one flipped byte breaks the checksum.
    Bytes bad = file;
    bad[0x30] ^= 0x10;
    CHECK(!th08::decodeScoreFile(bad).has_value());
    // No TH8K: not a score file. A VRSM of another program voids it.
    Bytes noKey(0x1C, 0);
    const Bytes c0 = scoreRecord("CLRD", 0x24, 4);
    noKey.insert(noKey.end(), c0.begin(), c0.end());
    th08::ScoreData sd2;
    CHECK(!th08::parseScoreImage(noKey, sd2) && !sd2.extraUnlocked());
    Bytes withVrsm = image;
    Bytes v = scoreRecord("VRSM", 0x1C, 1);
    std::memcpy(v.data() + 0x0C, "0100d", 5);
    v[0x14] = 1;  // size 1
    withVrsm.insert(withVrsm.end(), v.begin(), v.end());
    CHECK(th08::parseScoreImage(withVrsm, sd2));  // no exe to check against
    CHECK(!th08::parseScoreImage(withVrsm, sd2, std::make_pair(840704, 1)));
}

void testExeCheck() {
    std::string why;
    CHECK(!th08::isExe100d(Bytes(100, 0), &why));
    CHECK(why.find("840704") != std::string::npos);
    CHECK(!th08::isExe100d(Bytes(th08::kExeSize, 0), &why));
}

void testGameConfig() {
    using th08::GameConfig;
    GameConfig c;
    CHECK(!c.load({}, true));  // no file: defaults
    CHECK(c.music == GameConfig::MusicWav && c.lives == 2 && c.bombs == 3 && c.colourMode == 0 && c.shotSlow == 0);
    CHECK(c.bgmVolume == 100 && c.seVolume == 80 && c.optionBits == 1 && c.version == 0x00080001);
    CHECK(!c.load({}, false) && c.music == GameConfig::MusicMidi);
    Bytes saved = c.save();
    CHECK(saved.size() == 60);
    // A valid file round-trips, including the volumes and the option bits (bit 7 forced off).
    c.lives = 6;
    c.bgmVolume = 48;
    c.seVolume = 12;
    c.optionBits = 0x4081;
    c.unused[3] = 9;
    saved = c.save();
    GameConfig d;
    CHECK(d.load(saved, true));
    CHECK(d.lives == 6 && d.bgmVolume == 48 && d.seVolume == 12 && d.optionBits == 0x4001 && d.unused[3] == 9);
    // Out of range (lives 7), the wrong version or size: zeroed and re-initialised.
    saved[0x1C] = 7;
    CHECK(!d.load(saved, true));
    CHECK(d.lives == 2 && d.unused[3] == 0 && d.optionBits == 1 && d.bgmVolume == 100);
    Bytes shortFile = c.save();
    shortFile.pop_back();
    CHECK(!d.load(shortFile, true));
    Bytes oldVersion = c.save();
    oldVersion[0x14] = 2;  // 0x00080002
    CHECK(!d.load(oldVersion, true));
    // The volumes are not validated.
    Bytes loud = c.save();
    loud[0x27] = 0x7F;
    CHECK(d.load(loud, true) && d.bgmVolume == 127);
}

void testVersionFile() {
    const std::string ver = "0009_ 100 1\n0100c 840704 1\n0100c 840704 2724749753\n0100d 840704 2724749753\n";
    const Bytes b(ver.begin(), ver.end());
    CHECK(th08::findVersionLines(b, "0100c").size() == 2);
    CHECK(th08::versionMatches(b, "0100c", {840704, static_cast<std::int32_t>(2724749753u)}));  // the second line
    CHECK(th08::versionMatches(b, "0100d", {840704, static_cast<std::int32_t>(2724749753u)}));
    CHECK(!th08::versionMatches(b, "0100d", {840704, 1}));
    Bytes exe(16, 0);
    exe[0] = 1, exe[4] = 2, exe[8] = 3, exe[12] = 100;  // the last word is left out
    CHECK(th08::exeChecksum(exe) == 6);
}

// The player's own th08.dat, when TH08_REAL_DAT names it (spec_files 1.9).
void testRealArchive() {
    const char* path = std::getenv("TH08_REAL_DAT");
    if (!path || !*path) return;
    std::string err;
    auto file = MappedFile::open(path, &err);
    CHECK(file != nullptr);
    if (!file) return;
    auto archive = openArchive("th08.dat", std::move(file), &err);
    CHECK(archive != nullptr);
    if (!archive) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return;
    }
    CHECK(archive->entries().size() == 317);
    struct Want {
        const char* name;
        std::size_t size;
        const char* sha;
    };
    const Want wants[] = {
        {"ascii.anm", 405420, "f703d05264e942b31b8b5af92b4f9c1c6b97649ea933fa893844130c19db4947"},
        {"ecldata1.ecl", 0, "6b44a0ea36648edcdeae522a2ac16d1f09bf2097d3ddaa1a61c8c1703bad68ea"},
        {"msg1a.dat", 0, "e8ee11141619839e00dafbcc609a620a497c2ecc095f6c30a39bdbcec172830f"},
        {"stage1.std", 0, "73a540bf6fa05029b17a88a01fe939b7f1ade2e9a38c5b75162d505f2b5e1111"},
        {"thbgm.fmt", 1109, "b1a0b2b45a1e97c55b42bdd673ca1f06b87cd6630733486ca23420a99bccc144"},
        {"title01.anm", 7386444, nullptr},
    };
    for (const Want& w : wants) {
        bool found = false;
        for (std::size_t i = 0; i < archive->entries().size(); ++i) {
            if (archive->entries()[i].name != w.name) continue;
            found = true;
            Bytes out;
            const bool ok = archive->extract(i, out, &err);
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  %s\n", err.c_str());
            if (w.size) CHECK(out.size() == w.size);
            if (w.sha) CHECK(sha256Hex(out) == w.sha);
        }
        CHECK(found);
    }
    // Every member decodes.
    std::size_t decoded = 0;
    for (std::size_t i = 0; i < archive->entries().size(); ++i) {
        Bytes out;
        if (archive->extract(i, out, &err)) ++decoded;
        else std::fprintf(stderr, "  %s\n", err.c_str());
    }
    CHECK(decoded == 317);
    std::printf("  (the player's th08.dat: %zu members decoded)\n", decoded);
    // Every .anm loads into a slot and each of its scripts runs for 300 ticks.
    {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        TextureLibrary textures;
        th08::AnmLibrary lib;
        ZunRng rng(7);
        std::size_t files = 0, scripts = 0, failed = 0;
        for (const ArchiveEntry& e : archive->entries()) {
            if (e.name.size() < 4 || e.name.substr(e.name.size() - 4) != ".anm") continue;
            if (!lib.load(vfs, textures, e.name, 9, &err)) {
                ++failed;
                std::fprintf(stderr, "  %s\n", err.c_str());
                continue;
            }
            ++files;
            const th08::AnmFile* f = lib.file(9);
            for (std::size_t s = 0; s < f->scripts.size(); ++s) {
                th08::AnmVm vm;
                vm.start(lib, rng, 9, static_cast<int>(s));
                for (int i = 0; i < 300 && !vm.finished(); ++i) {
                    if (i % 50 == 49) vm.interrupt(1 + i % 7);
                    vm.tick(rng);
                }
                ++scripts;
            }
        }
        CHECK(files == 113 && failed == 0);
        std::printf("  (%zu .anm files, %zu scripts run)\n", files, scripts);
    }
    // The title menu on the real files: the cursor skips the locked rows; Quit exits.
    {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        TextureLibrary textures;
        th08::AnmLibrary lib;
        CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
        CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
        ZunRng rng(3);
        th08::FlowContext ctx;
        ctx.vfs = &vfs;
        ctx.textures = &textures;
        ctx.anm = &lib;
        ctx.rng = &rng;
        bool exited = false;
        ctx.request = [&](th08::Scene s) { exited = exited || s == th08::Scene::Exit; };
        th08::MainMenu menu(ctx);
        CHECK(menu.create(&err));
        th08::MenuInput in;
        auto pass = [&](std::uint16_t buttons) {
            in.sample(buttons);
            menu.update(in);
        };
        for (int i = 0; i < 20; ++i) pass(0);
        CHECK(menu.subState() == 1 && menu.cursor() == th08::MainMenu::GameStart);
        pass(th08::BtnDown);
        pass(0);
        CHECK(menu.cursor() == th08::MainMenu::PracticeStart);  // Extra and Spell Practice locked
        pass(th08::BtnUp);
        pass(0);
        CHECK(menu.cursor() == th08::MainMenu::GameStart);
        pass(th08::BtnUp);
        pass(0);
        CHECK(menu.cursor() == th08::MainMenu::Quit);  // wraps
        pass(th08::BtnShoot);
        for (int i = 0; i < 70 && !exited; ++i) pass(0);
        CHECK(exited && menu.removed());
        RecordingSink sink;
        menu.draw(sink);
        CHECK(sink.items.empty());
        // Unlocked rows are reachable.
        ctx.extraUnlocked = ctx.spellPracticeUnlocked = true;
        th08::MainMenu menu2(ctx);
        CHECK(menu2.create(&err));
        th08::MenuInput in2;
        for (int i = 0; i < 20; ++i) {
            in2.sample(0);
            menu2.update(in2);
        }
        in2.sample(th08::BtnDown);
        menu2.update(in2);
        CHECK(menu2.cursor() == th08::MainMenu::ExtraStart);
        // (Drawn before the next menu reloads title01.anm into the same slot.)
        RecordingSink sink2;
        menu2.draw(sink2);
        CHECK(sink2.items.size() > 8);  // the background, the logo and the rows
        std::printf("  (title: %zu sprites drawn after 21 passes)\n", sink2.items.size());
        // Option: the lives wrap within 0-4, a volume press moves 5, Exit goes back.
        th08::GameConfig cfg;
        ctx.config = &cfg;
        th08::MainMenu menu3(ctx);
        CHECK(menu3.create(&err));
        th08::MenuInput in3;
        auto pass3 = [&](std::uint16_t buttons) {
            in3.sample(buttons);
            menu3.update(in3);
        };
        for (int i = 0; i < 20; ++i) pass3(0);
        pass3(th08::BtnUp);  // Quit
        pass3(0);
        pass3(th08::BtnUp);  // Option
        pass3(0);
        pass3(th08::BtnShoot);
        for (int i = 0; i < 40; ++i) pass3(0);
        CHECK(menu3.state() == th08::MainMenu::State::Options && menu3.cursor() == 0);
        pass3(th08::BtnLeft);
        pass3(0);
        CHECK(cfg.lives == 1);
        for (int i = 0; i < 3; ++i) {
            pass3(th08::BtnRight);
            pass3(0);
        }
        CHECK(cfg.lives == 4);
        pass3(th08::BtnRight);
        pass3(0);
        CHECK(cfg.lives == 0);  // wraps within 0-4 (no play count yet)
        for (int i = 0; i < 4; ++i) {
            pass3(th08::BtnDown);
            pass3(0);
        }
        CHECK(menu3.cursor() == th08::MainMenu::SeVolume);
        pass3(th08::BtnLeft);
        pass3(0);
        CHECK(cfg.seVolume == 75);
        pass3(th08::BtnBomb);  // cursor to Exit
        pass3(0);
        CHECK(menu3.cursor() == th08::MainMenu::OptionExit);
        pass3(th08::BtnBomb);  // leave
        pass3(0);
        CHECK(menu3.state() == th08::MainMenu::State::Title);
        // Game Start: difficulty, then the four teams; Cancel steps back.
        for (int i = 0; i < 20; ++i) pass3(0);
        while (menu3.cursor() != th08::MainMenu::GameStart) {
            pass3(th08::BtnUp);
            pass3(0);
        }
        pass3(th08::BtnShoot);
        for (int i = 0; i < 10; ++i) pass3(0);
        CHECK(menu3.state() == th08::MainMenu::State::Difficulty && menu3.cursor() == 1);
        pass3(th08::BtnDown);
        pass3(0);
        pass3(th08::BtnShoot);
        for (int i = 0; i < 10; ++i) pass3(0);
        CHECK(cfg.difficulty == 2 && menu3.state() == th08::MainMenu::State::Character);
        pass3(th08::BtnLeft);
        pass3(0);
        CHECK(menu3.cursor() == 3);  // four teams, wrapping
        pass3(th08::BtnBomb);
        pass3(0);
        CHECK(menu3.state() == th08::MainMenu::State::Difficulty && ctx.character == 3);

    }
    // The player's score.dat backup (a fresh file) decodes, with this exe's VRSM.
    {
        std::string dir(path);
        dir = dir.substr(0, dir.find_last_of('/') + 1);
        auto bak = MappedFile::open(dir + "backup/score_1.240313.bak", &err);
        if (bak) {
            const auto image = th08::decodeScoreFile(bak->bytes());
            CHECK(image.has_value() && image->size() == 1144);
            th08::ScoreData sd;
            CHECK(image && th08::parseScoreImage(*image, sd, std::make_pair(840704, static_cast<std::int32_t>(2724749753u))));
            CHECK(sd.totalPlays == 0 && !sd.extraUnlocked() && sd.clears[12].reached[0] == 1);
        }
    }
    // The version file has the 1.00d line (and its exe checksum, when TH08_REAL_EXE names it).
    for (std::size_t i = 0; i < archive->entries().size(); ++i) {
        if (archive->entries()[i].name != th08::kVersionFile) continue;
        Bytes ver;
        CHECK(archive->extract(i, ver, &err));
        CHECK(th08::versionMatches(ver, th08::kVersionTag, {840704, static_cast<std::int32_t>(2724749753u)}));
        if (const char* exePath = std::getenv("TH08_REAL_EXE")) {
            auto exe = MappedFile::open(exePath, &err);
            CHECK(exe && th08::exeChecksum(exe->bytes()) == static_cast<std::int32_t>(2724749753u));
            CHECK(exe && th08::isExe100d(exe->bytes()));
            th08::ExeTables tables;
            const bool ok = exe && th08::loadExeTables(exe->bytes(), tables, &err);
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  %s\n", err.c_str());
            // Every sample is in the archive; the menu texts are there.
            Vfs v;
            auto again = MappedFile::open(path, &err);
            CHECK(again && v.mount(openArchive("th08.dat", std::move(again), &err)));
            int found = 0;
            for (const std::string& s : tables.sampleFiles) found += v.exists(s) ? 1 : 0;
            CHECK(found == th08::ExeTables::kSamples);
            CHECK(tables.sampleFiles[0].find("se_plst00") != std::string::npos);
            bool texts = true;
            for (const auto& m : tables.titleText) texts = texts && !m.sjis.empty();
            for (const auto& m : tables.optionText) texts = texts && !m.sjis.empty();
            for (const auto& m : tables.keyConfigText) texts = texts && !m.sjis.empty();
            CHECK(texts);
            CHECK(tables.sounds[10].sample >= 0 && tables.sampleFiles[static_cast<std::size_t>(tables.sounds[10].sample)].find("se_ok00") != std::string::npos);
            std::printf("  (sample 0 %s; sound 10 -> %s volume %d)\n", tables.sampleFiles[0].c_str(),
                        tables.sampleFiles[static_cast<std::size_t>(tables.sounds[10].sample)].c_str(), tables.sounds[10].volume);
        }
    }
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"block cipher: vectors, limits, round trips", testCipher},
        {"PBGZ archive (th08.dat)", testPbgz},
        {"th08.exe 1.00d check", testExeCheck},
        {"menu input: pressed, auto-repeat, select and cancel", testMenuInput},
        {"th08.cfg: layout, validation, re-initialisation", testGameConfig},
        {"score.dat: cipher, rolling XOR, checksum, records, unlocks", testScoreFile},
        {"ANM v3: files, sprites by position, opcodes 9/33/82-89, interrupt return, freeze", testAnm},
        {"version file: every matching line, exe checksum", testVersionFile},
        {"the player's th08.dat (TH08_REAL_DAT)", testRealArchive},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
