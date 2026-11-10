// Host tests for Imperishable Night's game code (th08_game) and the engine pieces it added:
// the block cipher (io/ThCrypt), the PBGZ archive and the th08.exe 1.00d check. Synthetic
// data only; with TH08_REAL_DAT=<path to th08.dat> the player's own archive is also opened
// and a few members are checked against the spec's SHA-256 values (never in CI).

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
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
#include "th08/Background.hpp"
#include "th08/Bullets.hpp"
#include "th08/Effects.hpp"
#include "th08/Ending.hpp"
#include "th08/Enemies.hpp"
#include "th08/Gameplay.hpp"
#include "th08/ScreenEffects.hpp"
#include "th08/World.hpp"
#include "th08/ShotData.hpp"
#include "th08/Spellcard.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Flow.hpp"
#include "th08/MainMenu.hpp"
#include "th08/Msg.hpp"
#include "th08/MusicRoom.hpp"
#include "th08/Replay.hpp"
#include "th08/ResultScreen.hpp"
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
    Bytes pscr = scoreRecord("PSCR", 0x178, 2);  // character 5, Stage 2 Hard: 3 plays, best 12345
    pscr[0x0C + 4 * (1 * 5 + 2)] = 3;
    pscr[0xC0 + 4 * (1 * 5 + 2)] = 0x39;
    pscr[0xC0 + 4 * (1 * 5 + 2) + 1] = 0x30;
    pscr[0x174] = 5;
    pscr[0x175] = 1;
    add(pscr);
    const Bytes file = th08::encodeScoreFile(image, 0x53, 0x41);
    const auto decoded = th08::decodeScoreFile(file);
    CHECK(decoded.has_value());
    th08::ScoreData sd;
    CHECK(decoded && th08::parseScoreImage(*decoded, sd));
    CHECK(sd.totalPlays == 42 && sd.lastWords[3] == 208);
    CHECK(sd.practice[5].plays[7] == 3 && sd.practice[5].best[7] == 12345 && sd.practice[4].plays[7] == 0);
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

    // A save rebuilds the image from the records (spec_files §5.4 order) and reads back the same.
    th08::ScoreData sd3;
    CHECK(th08::parseScoreImage(image, sd3));
    CHECK(!sd3.setMusicHeard(-1) && sd3.setMusicHeard(5) && !sd3.setMusicHeard(5) && !sd3.setMusicHeard(32));
    th08::ScoreData back;
    CHECK(th08::parseScoreImage(sd3.imageToSave(), back));
    CHECK(back.musicHeard[5] == 1 && back.musicHeard[0] == 0 && back.totalPlays == 42 && back.lastWords[3] == 208);
    CHECK(back.practice[5].hasData && back.practice[5].best[7] == 12345 && !back.practice[4].hasData);
    CHECK(back.teamHasNoContinueFinalB(1) && back.teamHasFinalA(2) && !back.cardsTagged && back.highScores.empty());
    // No file: a fresh image (TH8K, 13 CLRD, LSNM, FLSP, PLST, VRSM), the size of a fresh PC file.
    th08::ScoreData none;
    CHECK(none.setMusicHeard(0));
    const auto fresh = th08::decodeScoreFile(th08::encodeScoreFile(none.imageToSave(), 7, 9));
    CHECK(fresh.has_value() && fresh->size() == 1144);
    th08::ScoreData sd4;
    CHECK(fresh && th08::parseScoreImage(*fresh, sd4, std::make_pair(840704, static_cast<std::int32_t>(2724749753u))));
    CHECK(sd4.musicHeard[0] == 1 && sd4.musicHeard[1] == 0 && sd4.totalPlays == 0 && !sd4.extraUnlocked());
    CHECK(sd4.clears[12].noContinue[4] == 1 && sd4.clears[0].reached[0] == 1 && std::string(sd4.lastName.data()) == "        ");
    // Statistics, clear bits and cards: written and read back.
    th08::ScoreData rec;
    rec.notePlay(1, 6, true, false);
    rec.noteContinue(1, 6);
    rec.markClear(6, 1, 0x4000, false);
    rec.markClear(6, 1, 0x0001, true);
    rec.cardsTagged = true;
    rec.cards[7].attempts[6] = 3;
    rec.cards[7].captures[12] = 1;
    rec.cards[7].name[0] = 0x82;
    rec.cards[7].level = 1;
    std::memcpy(rec.lastName.data(), "ABC     ", 8);
    th08::ScoreData rec2;
    CHECK(th08::parseScoreImage(rec.imageToSave(), rec2));
    CHECK(rec2.plstValue(1, th08::ScoreData::Plays) == 2 && rec2.plstValue(6, th08::ScoreData::Plays) == 2 && rec2.totalPlays == 2);
    CHECK(rec2.plstValue(6, th08::ScoreData::PerCharacter + 6) == 2 && rec2.plstValue(1, th08::ScoreData::Retries) == 1);
    CHECK(rec2.plstValue(6, th08::ScoreData::Continues) == 1);
    CHECK(rec2.clears[6].reached[1] == 0x4001 && rec2.clears[6].noContinue[1] == 0x0001 && rec2.clears[12].reached[1] == 0x4001);
    CHECK(rec2.cardsTagged && rec2.cards[7].attempts[6] == 3 && rec2.cards[7].captures[12] == 1 && rec2.cards[7].name[0] == 0x82);
    CHECK(rec2.cards[7].level == 1 && rec2.cards[0].present && std::string(rec2.lastName.data()) == "ABC     ");
    // High scores: ten placeholders (100000 ... 10000) per list; ties rank the newer entry first;
    // only the first 10 nodes are written.
    CHECK(rec.rankOf(1, 6, 100000) == 0 && rec.rankOf(1, 6, 95000) == 1 && rec.rankOf(1, 6, 9000) == 10);
    for (std::uint32_t k = 0; k < 12; ++k) {
        th08::HighScoreRecord h{};
        const std::uint32_t score = 200000 + 1000 * k;
        std::memcpy(h.data() + 0x0C, &score, 4);
        h[0x14] = 6;
        h[0x15] = 1;
        h[0x16] = static_cast<std::uint8_t>(k);
        rec.addHighScore(h);
    }
    CHECK(rec.rankOf(1, 6, 211000) == 0 && rec.rankOf(1, 6, 210999) == 1 && rec.rankOf(2, 6, 100000) == 0);
    const auto rows = rec.listRows(1, 6);
    CHECK(rows[0].record && (*rows[0].record)[0x16] == 11 && rows[9].record && (*rows[9].record)[0x16] == 2);
    CHECK(rec.bestScore(1, 6) == 211000 && rec.bestScore(0, 0) == 100000);
    th08::ScoreData rec3;
    CHECK(th08::parseScoreImage(rec.imageToSave(), rec3) && rec3.highScores.size() == 10);
}

// musiccmt.txt: path, title, comment lines (line 0 always empty), blank lines dropped, a
// line starting with '@' ends a track, at most 32 tracks.
void testMusicComments() {
    const std::string text =
        "a header line before the first track\r\n"
        "@bgm/a.mid\r\nTitle A\r\nNo.1 A\r\n\r\ncomment\r\n\r\n"
        "@bgm/B.mid\nTitle B\nonly line\n";
    const auto tracks = th08::parseMusicComments(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    CHECK(tracks.size() == 2);
    if (tracks.size() == 2) {
        CHECK(tracks[0].path == "bgm/a.mid" && tracks[0].title == "Title A");
        CHECK(tracks[0].lines[0].empty() && tracks[0].lines[1] == "No.1 A" && tracks[0].lines[2] == "comment" && tracks[0].lines[3].empty());
        CHECK(tracks[1].path == "bgm/B.mid" && tracks[1].lines[1] == "only line");
        CHECK(th08::musicTrackIndex(tracks, "BGM\\b.mid") == 1 && th08::musicTrackIndex(tracks, "bgm/c.mid") == -1);
    }
    std::string many;
    for (int i = 0; i < 40; ++i) many += "@bgm/x.mid\nT\nL\n";
    CHECK(th08::parseMusicComments(std::span(reinterpret_cast<const std::uint8_t*>(many.data()), many.size())).size() == 32);
}

// Screen effects (spec_bombs_familiars 1.4): a type-1 shake of d frames draws two u32 per
// frame for d - 1 frames, from the frame after the request; a ramped shake lasts up + hold +
// down - 1 frames; both wait while the spell-card freeze is set; flashes draw nothing.
void testScreenEffects() {
    th08::ScreenEffects fx;
    th08::World w;
    ZunRng rng(3);
    w.rng = &rng;
    w.stageFrame = 100;
    fx.shake(16, 8.0f, 0.0f);
    fx.flash(8, 1, 0x8FFFFFFFu);
    std::uint32_t before = rng.generations();
    for (int i = 0; i < 30; ++i) fx.update(w);
    CHECK(rng.generations() - before == 15 * 4 && fx.jobs() == 0);
    fx.rampedShake(16.0f, 120, 60, 120);
    w.mode = th08::ModeSpellFreeze;
    before = rng.generations();
    for (int i = 0; i < 10; ++i) fx.update(w);
    CHECK(rng.generations() == before && fx.jobs() == 1);
    w.mode = 0;
    for (int i = 0; i < 400; ++i) fx.update(w);
    CHECK(rng.generations() - before == 299 * 4 && fx.jobs() == 0);
    // The offset goes from `from` to `to` and is one of 0, +offset, -offset per axis.
    fx.shake(10, 8.0f, 0.0f);
    fx.update(w);
    CHECK(fx.offsetX() == 0.0f || std::fabs(fx.offsetX()) == 7.2f);
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
        // Back from a practice game: the menus walk straight to the stage list (spec 4.7).
        {
            th08::GameConfig pc;
            th08::ScoreData sd;
            sd.clears[1].reached.fill(0x7);
            ctx.config = &pc;
            ctx.scores = &sd;
            ctx.previous = th08::Scene::ResultAfterGame;
            ctx.practice = true;
            ctx.character = 1;
            ctx.stage = 2;
            th08::MainMenu back(ctx);
            CHECK(back.create(&err));
            th08::MenuInput ib;
            for (int i = 0; i < 4; ++i) {
                ib.sample(0);
                back.update(ib);
            }
            CHECK(back.state() == th08::MainMenu::State::PracticeStage && back.cursor() == 2);
            ctx.config = nullptr;
            ctx.scores = nullptr;
            ctx.previous = th08::Scene::Init;
            ctx.practice = false;
            ctx.character = ctx.stage = 0;
        }
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

        // The Music Room: the title's row 6 requests scene 8; locked tracks show "？？？？"
        // and cannot be played; Cancel requests the title, which comes back on row 6.
        th08::Scene requested = th08::Scene::MainMenu;
        ctx.request = [&](th08::Scene sc) { requested = sc; };
        std::vector<std::string> played;
        ctx.musicPlayed = [&](std::string_view p) { played.emplace_back(p); };
        std::array<std::uint8_t, 32> heardFlags{};
        heardFlags[0] = heardFlags[2] = 1;
        ctx.musicHeard = &heardFlags;
        th08::MainMenu menu4(ctx);
        CHECK(menu4.create(&err));
        th08::MenuInput in4;
        auto pass4 = [&](std::uint16_t buttons) {
            in4.sample(buttons);
            menu4.update(in4);
        };
        for (int i = 0; i < 20; ++i) pass4(0);
        CHECK(!played.empty() && played[0] == "bgm/th08_01.mid");  // the title theme is heard
        while (menu4.cursor() != th08::MainMenu::MusicRoom) {
            pass4(th08::BtnDown);
            pass4(0);
        }
        pass4(th08::BtnShoot);
        CHECK(requested == th08::Scene::MusicRoom && menu4.removed());
        ctx.previous = th08::Scene::MainMenu;
        th08::MusicRoom room(ctx);
        CHECK(room.create(&err));
        CHECK(room.tracks().size() == 21);
        if (room.tracks().size() == 21) {
            CHECK(room.tracks()[0].path == "bgm/th08_01.mid" && room.tracks()[1].path == "bgm/th08_00.mid");
            CHECK(room.tracks()[15].path == "bgm/th08_13b.mid" && room.tracks()[18].path == "bgm/th08_16.mid");
            CHECK(room.tracks()[0].lines[0].empty() && room.tracks()[0].lines[1].rfind("No.1 ", 0) == 0);
            CHECK(th08::musicTrackIndex(room.tracks(), "bgm/th08_17.mid") == 19);
        }
        CHECK(room.unlocked(0) && !room.unlocked(1) && room.unlocked(2));
        th08::MenuInput in5;
        auto pass5 = [&](std::uint16_t buttons) {
            in5.sample(buttons);
            room.update(in5);
        };
        for (int i = 0; i < 8; ++i) pass5(0);
        pass5(th08::BtnUp);
        pass5(0);
        CHECK(room.cursor() == 20 && room.top() == 11);  // wraps to the last track
        pass5(th08::BtnDown);
        pass5(0);
        CHECK(room.cursor() == 0 && room.top() == 0);
        pass5(th08::BtnDown);
        pass5(0);
        pass5(th08::BtnShoot);  // track 2 (No.2) is locked
        pass5(0);
        CHECK(room.cursor() == 1 && room.selected() == -1);
        pass5(th08::BtnDown);
        pass5(0);
        pass5(th08::BtnShoot);
        pass5(0);
        CHECK(room.selected() == 2);
        for (int i = 0; i < 9; ++i) {
            pass5(th08::BtnDown);
            pass5(0);
        }
        CHECK(room.cursor() == 11 && room.top() == 2);
        RecordingSink sink5;
        room.draw(sink5);
        CHECK(sink5.items.size() >= 12);  // background, deco, ten titles and the comment
        std::printf("  (Music Room: %zu tracks, %zu sprites drawn)\n", room.tracks().size(), sink5.items.size());
        pass5(th08::BtnBomb);
        CHECK(requested == th08::Scene::MainMenu && room.done());
        room.release();
        CHECK(!lib.loaded(th08::MusicRoom::kSlotMusic));
        ctx.previous = th08::Scene::MusicRoom;
        th08::MainMenu menu5(ctx);
        CHECK(menu5.create(&err) && menu5.cursor() == th08::MainMenu::MusicRoom);
    }
    // Every stage background (normal and spell practice) loads and runs 3000 frames: the
    // ribbons of 4A/4B/Extra, the per-stage cull distance, the sway of stage 5, op 34's clear
    // colour, and a draw of each pass.
    {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        TextureLibrary textures;
        th08::AnmLibrary lib;
        CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
        ZunRng rng(11);
        int loaded = 0;
        bool swayed = false;
        for (int spell = 0; spell < 2; ++spell) {
            for (int st = 0; st < th08::kStageCount; ++st) {
                th08::Background bg;
                const bool ok = bg.load(vfs, textures, lib, rng, st, spell != 0, &err);
                CHECK(ok);
                if (!ok) {
                    std::fprintf(stderr, "  %s\n", err.c_str());
                    continue;
                }
                ++loaded;
                const bool bamboo = st == 3 || st == 4 || st == 8;
                CHECK((bg.ribbonCount() == 28) == bamboo);
                const float cull = st == 5 ? 1350.0f : (st == 6 || st == 7) ? 1800.0f : 1150.0f;
                CHECK(bg.cullDistanceSquared() == cull * cull);
                int glows = 0;
                bg.spawnGlow = [&](const Vec3f&) { ++glows; };
                RecordingSink sink;
                th08::AnmDrawState ds;
                for (int f = 0; f < 3000; ++f) {
                    bg.update(lib, rng);
                    if (bg.swayMode() != 0) swayed = true;
                    if (f % 500 == 499) {
                        bg.drawHigh(sink, ds);
                        bg.drawLow(sink, ds);
                    }
                }
                CHECK(bg.frames() == 3000);
                CHECK(glows == 12 * (spell ? 1000 : 766));  // every third frame from 700 (spell practice: all)
                bg.update(lib, rng, true);  // frozen: nothing runs
                CHECK(bg.frames() == 3000);
                // 4A: op 34 at frame 0 drives the clear colour; sway mode 1 from frame 290.
                if (st == 3 && !spell) CHECK(bg.swayMode() == 1 && bg.eyeOffset().x != 0.0f && bg.clearColor() != 0);
                bg.release(lib);
            }
        }
        CHECK(loaded == 18 && swayed);
        std::printf("  (18 stage backgrounds ran 3000 frames each)\n");
    }
    // A game starts on the real files: stage 1, the HUD files, the stage songs.
    {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        TextureLibrary textures;
        th08::AnmLibrary lib;
        CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
        CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
        ZunRng rng(5);
        th08::FlowContext ctx;
        ctx.vfs = &vfs;
        ctx.textures = &textures;
        ctx.anm = &lib;
        ctx.rng = &rng;
        std::vector<std::string> heard;
        ctx.musicPlayed = [&](std::string_view p) { heard.emplace_back(p); };
        th08::Gameplay game(ctx);
        th08::Gameplay::Setup setup;
        setup.difficulty = 4;
        setup.stage = 8;
        setup.character = 7;
        CHECK(game.start(setup, &err));
        CHECK(game.state().lives == 2.0f && game.state().nightSteps == 6 && game.state().hiScore == 100000);
        th08::MenuInput in;
        for (int i = 0; i < 120; ++i) {
            in.sample(0);
            game.tick(in);
        }
        CHECK(game.stageFrame() == 120 && game.outcome() == th08::Gameplay::Outcome::None);
        CHECK(!heard.empty());  // the stage theme is heard (not practice)
        RecordingSink sink;
        game.draw(sink);
        CHECK(sink.items.size() > 40);  // the panel at least
        // Menu pauses; Q quits to the title after the 20-pass closing.
        in.sample(th08::BtnMenu);
        game.tick(in);
        CHECK(game.menu().pauseMenu() && game.outcome() == th08::Gameplay::Outcome::None);
        in.sample(th08::BtnQ);
        game.tick(in);
        for (int i = 0; i < 20; ++i) {
            in.sample(0);
            game.tick(in);
        }
        CHECK(game.outcome() == th08::Gameplay::Outcome::Title);
        game.end();
        CHECK(!lib.loaded(th08::Background::kSlotBackground) && !lib.loaded(th08::Gui::kSlotFront));
        setup = {};
        setup.practice = true;
        setup.stage = 1;
        heard.clear();
        CHECK(game.start(setup, &err) && game.state().lives == 8.0f && game.state().power == 112.0f);
        in.sample(0);
        game.tick(in);
        CHECK(heard.empty());  // practice marks nothing heard
    }
    // The shot files: eight files, six levels (eight in ply02as), the shared thresholds.
    {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        for (int c = 0; c < 12; ++c) {
            for (int f = 0; f < 2; ++f) {
                const auto bytes = vfs.read(th08::shotFileName(c, f != 0));
                th08::ShotFile sf;
                CHECK(bytes && sf.parse(*bytes, &err));
                const bool remilia = th08::shotFileName(c, f != 0) == "ply02as.sht";
                CHECK(sf.levels.size() == (remilia ? 8u : 6u));
                if (sf.levels.size() >= 6) CHECK(sf.levels[0].threshold == 8 && sf.levels[4].threshold == 128 && sf.levels[5].threshold == 999);
                CHECK(sf.bombsPerLife == 3.0f && sf.levelFor(0) == 0 && sf.levelFor(128) == 5);
            }
        }
        CHECK(th08::shotFileName(9, false) == "ply02as.sht" && th08::shotFileName(8, true) == "ply02a.sht");
        CHECK(th08::shotFileName(2, false) == "ply02a.sht" && th08::shotFileName(2, true) == "ply02as.sht");
    }
    // The player on the real files: direction priorities, the focused file's speeds, the
    // clamps, the team switch with its 7-frame "youkai in front" lag, the familiars, the
    // 20-frame fire cycle.
    {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        TextureLibrary textures;
        th08::AnmLibrary lib;
        CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
        CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
        th08::ExeTables tables;
        bool haveTables = false;
        if (const char* exePath = std::getenv("TH08_REAL_EXE")) {
            auto exe = MappedFile::open(exePath, &err);
            haveTables = exe && th08::loadExeTables(exe->bytes(), tables, &err);
        }
        ZunRng rng(9);
        th08::FlowContext ctx;
        ctx.vfs = &vfs;
        ctx.textures = &textures;
        ctx.anm = &lib;
        ctx.rng = &rng;
        ctx.tables = haveTables ? &tables : nullptr;
        auto game = std::make_unique<th08::Gameplay>(ctx);
        game->setInputDelay(false);  // (the checks below expect each pass's own input)
        th08::MenuInput in;
        auto run = [&](std::uint16_t buttons, int frames) {
            for (int i = 0; i < frames; ++i) {
                in.sample(buttons);
                game->tick(in);
            }
        };
        th08::Gameplay::Setup setup;
        setup.character = 3;  // Youmu & Yuyuko
        CHECK(game->start(setup, &err));
        const th08::Player& p = game->player();
        run(0, 1);
        CHECK(p.state() == th08::Player::State::Invulnerable);  // 120 >= 30: straight through the spawn-in
        CHECK(p.familiar(2).kind == th08::Player::FamiliarKind::PhantomTeam);  // unfocused: Youmu's phantom
        const Vec3f start = p.position();
        run(th08::BtnUp | th08::BtnDown, 1);  // Down beats Up
        CHECK(p.direction() == th08::Player::Direction::Down && p.position().y == start.y + 5.0f);
        run(th08::BtnLeft | th08::BtnRight, 1);  // Left beats Right
        CHECK(p.direction() == th08::Player::Direction::Left && p.position().x == start.x - 5.0f);
        run(th08::BtnFocus | th08::BtnRight, 1);  // focused: the youkai file's 1.9
        CHECK(p.focused() && std::fabs(p.position().x - (start.x - 5.0f + 1.9f)) < 1e-4f);
        CHECK(p.familiar(2).kind == th08::Player::FamiliarKind::None && p.familiar(0).kind == th08::Player::FamiliarKind::Yuyuko);
        CHECK(!p.youkaiInFront());
        run(th08::BtnFocus, 6);
        CHECK(!p.youkaiInFront());  // counter 6
        run(th08::BtnFocus, 1);
        CHECK(p.youkaiInFront());  // counter 7
        run(th08::BtnUp | th08::BtnLeft, 1);  // unfocused: Yuyuko's familiars dismissed, the phantom back
        CHECK(p.familiar(0).state == 3 && p.familiar(2).kind == th08::Player::FamiliarKind::PhantomTeam);
        run(th08::BtnUp | th08::BtnLeft, 199);
        CHECK(p.position().x == 8.0f && p.position().y == 16.0f);  // the clamps
        CHECK(p.familiar(0).state == 0 && p.familiar(0).kind == th08::Player::FamiliarKind::None);  // gone after 18 updates
        // The fire cycle: a tap fires one 20-frame cycle.
        run(th08::BtnShoot, 1);
        run(0, 1);
        CHECK(p.firing() && p.liveShots() > 0);
        run(0, 19);
        CHECK(!p.firing());
        // Solo Remilia: four permanent familiars, each spawning a trail effect (8 steps)
        // every movement step, plus the player sprite and the rest.
        setup.character = 9;
        CHECK(game->start(setup, &err));
        run(0, 2);
        for (int k = 0; k < 4; ++k) CHECK(game->player().familiar(k).kind == th08::Player::FamiliarKind::RemiliaSolo);
        if (haveTables) {
            const std::uint32_t before = rng.generations();
            run(0, 1);
            CHECK(rng.generations() - before >= 32);
            CHECK(game->effects().active() >= 4);
        }
        // Every character starts and shoots for a second at full power without trouble.
        for (int c = 0; c < 12; ++c) {
            setup.character = c;
            setup.practice = true;
            setup.stage = 5;
            CHECK(game->start(setup, &err));
            run(th08::BtnShoot, 60);
            run(th08::BtnShoot | th08::BtnFocus, 60);
            CHECK(game->state().power == 128.0f);
            RecordingSink sink;
            game->draw(sink);
            CHECK(sink.items.size() > 40);
        }
        // Bombs: every character's bomb on both sides runs for its duration, with its
        // invulnerability and cost, and pushes the gauge to that side's limit.
        if (haveTables) {
            const th08::Player& p = game->player();
            static constexpr int kDuration[8] = {200, 150, 300, 210, 250, 240, 220, 300};  // by character
            for (int c = 0; c < 12; ++c) {
                for (int side = 0; side < 2; ++side) {
                    setup.character = c;
                    setup.practice = true;
                    setup.stage = 5;
                    CHECK(game->start(setup, &err));
                    const std::uint16_t focus = side ? th08::BtnFocus : 0;
                    run(focus, 3);
                    run(focus | th08::BtnBomb, 1);
                    const bool started = p.bombActive();
                    CHECK(started);
                    if (!started) continue;
                    const int person = c < 4 ? c * 2 + side : c - 4;
                    const int d = p.bomb().duration;
                    CHECK(d == kDuration[person] && p.bomb().spell == th08::Player::spellFor(c, side));
                    CHECK(p.state() == th08::Player::State::Invulnerable && game->state().bombs == 2.0f && p.bombing());
                    if (person == 2) CHECK(game->screenEffects().jobs() == 1);  // Master Spark's ramped shake
                    run(focus, d - 1);
                    CHECK(p.bombActive());
                    RecordingSink sink;
                    game->draw(sink);
                    run(focus, 1);
                    CHECK(!p.bombActive() && p.bombing());  // the flag holds through the end frame
                    run(focus, 1);
                    CHECK(!p.bombing());
                    // (Familiars the bomb kills pull the gauge back by a twelfth each, even
                    // during the bomb.)
                    const int gauge = game->state().gauge;
                    CHECK(side ? gauge > game->state().gaugeMax / 2 : gauge < game->state().gaugeMin / 2);
                }
            }
            // A hit, then Bomb: the first frame of the window gives an ordinary bomb [QUIRK],
            // later frames the Last Spell of the character not in front (cost 2).
            for (int late = 0; late < 2; ++late) {
                setup.character = 0;
                setup.practice = true;
                setup.stage = 0;
                CHECK(game->start(setup, &err));
                run(th08::BtnLeft, 260);
                const bool alive = p.state() == th08::Player::State::Alive;
                CHECK(alive);
                if (!alive) continue;
                game->world().player->bulletHit(game->world(), p.position(), {4.0f, 4.0f, 0.0f});
                CHECK(p.state() == th08::Player::State::Dead && p.deathbombCountdown() > 2);
                if (late) run(0, 1);
                run(th08::BtnBomb, 1);
                CHECK(p.bombActive() && p.state() == th08::Player::State::Invulnerable);
                if (late) CHECK(p.bomb().index == 3 && p.bomb().spell == th08::Player::Spell::YukariLast && game->state().bombs == 1.0f);
                else CHECK(p.bomb().index == 0 && p.bomb().spell == th08::Player::Spell::ReimuBomb && game->state().bombs == 2.0f);
                run(0, 320);
                CHECK(!p.bombActive() && game->state().lives == 8.0f);  // the death never commits
            }
            // A hit during a boss's Last Spell (mode bits 7-8): the Dissolve spell starts by
            // itself the next frame, costs nothing, and ends the boss's attack.
            CHECK(game->start(setup, &err));
            run(th08::BtnLeft, 260);
            if (p.state() == th08::Player::State::Alive) {
                game->world().mode |= 1u << 7;
                game->world().player->bulletHit(game->world(), p.position(), {4.0f, 4.0f, 0.0f});
                run(0, 1);  // the death step counts the window down to 1
                CHECK(!p.bombActive());
                run(0, 1);
                CHECK(p.bombActive() && p.bomb().spell == th08::Player::Spell::Dissolve && p.bomb().duration == 120);
                CHECK(game->state().bombs == 3.0f);
                run(0, 120);
                CHECK(!p.bombActive() && (game->world().mode & th08::ModeDissolve) == 0 && game->state().lives == 8.0f);
            }
        }
        // The stage end (spec_flow 6.3/6.4): from Stage 3 the main game goes on to 4B with
        // Reimu & Yukari, waits for Shoot on the loading screen, and keeps the score and lives.
        {
            th08::Gameplay::Setup main;
            main.character = 0;
            main.difficulty = 1;
            main.lives = 2;
            main.stage = 2;
            CHECK(game->start(main, &err));
            run(0, 10);
            game->world().state->score = 12345;
            game->stageCleared();
            run(0, 1);
            CHECK(game->stageEndState() == 1 && game->state().stage == 4);
            run(0, 30);
            CHECK(game->stageEndState() == 1 && game->stageFrame() == 0);
            run(th08::BtnShoot, 1);
            CHECK(game->stageEndState() == 0);
            run(0, 10);
            CHECK(game->stageFrame() == 10 && game->state().score >= 12345 && game->state().lives == 2.0f);
            // Practice ends on Shoot with the results (scene 6).
            main.practice = true;
            main.stage = 1;
            CHECK(game->start(main, &err));
            run(0, 5);
            game->stageCleared();
            run(0, 5);
            CHECK(game->outcome() == th08::Gameplay::Outcome::None && game->stageEndState() == 3);
            run(th08::BtnShoot, 1);
            CHECK(game->outcome() == th08::Gameplay::Outcome::Results);
        }
        // The dialogue (spec_msg_stage_end): Stage 1's opening dialogue cancels the bullets, holds
        // the timeline's op 7 and runs to its end on Skip; the post-boss dialogue shows the results,
        // whose count-up ends the stage.
        if (haveTables) {
            th08::Gameplay::Setup main;
            main.character = 0;
            main.difficulty = 1;
            main.lives = 2;
            main.stage = 0;
            CHECK(game->start(main, &err));
            run(0, 10);
            th08::Dialogue* d = game->world().msg;
            CHECK(d && d->loaded());
            if (d && d->loaded()) {
                d->start(game->world(), 0);
                CHECK(d->running() && game->world().dialogue && d->mustWait());
                int frames = 0;
                bool released = false;
                while (d->running() && frames < 6000) {
                    run(th08::BtnSkip, 1);
                    released = released || !d->mustWait();
                    ++frames;
                }
                CHECK(!d->running() && !game->world().dialogue && released);
                std::printf("  (Stage 1's opening dialogue: %d frames on Skip)\n", frames);
                const std::int64_t before = game->state().score;
                d->start(game->world(), 1);
                frames = 0;
                bool tally = false;
                while (game->stageEndState() == 0 && frames < 20000) {
                    run(th08::BtnSkip, 1);
                    tally = tally || game->gui().resultsState() != 0;
                    ++frames;
                }
                std::printf("  (Stage 1's closing dialogue and results: %d frames on Skip)\n", frames);
                run(0, 1);
                CHECK(tally && game->stageEndState() == 1 && game->state().stage == 1);
                // No time points: the quota is missed and the night moves on an hour.
                CHECK(game->state().score > before && game->state().nightSteps == 2);
                // The next stage starts under the old frame's 168 tiles (a capture is asked for).
                CHECK(game->gui().transitionActive() && game->gui().takeCaptureRequest());
                run(th08::BtnShoot, 1);
                run(0, 5);
                CHECK(game->stageEndState() == 0 && game->state().stage == 1 && game->stageFrame() == 5);
                run(0, 200);
                CHECK(!game->gui().transitionActive());
            }
        }
        // ECL ex routines (claude/th08-specs/spec_ecl_ex_routines.md) on Stage 4A.
        if (haveTables) {
            th08::Gameplay::Setup main;
            main.character = 0;
            main.difficulty = 1;
            main.lives = 2;
            main.stage = 3;
            CHECK(game->start(main, &err));
            run(0, 10);
            const th08::World& wd = game->world();
            th08::EnemyManager& em = *wd.enemies;
            th08::BulletManager& bm = *wd.bullets;
            th08::Enemy& host = em.enemy(400);
            th08::Shooter sh;
            sh.type = 16;
            sh.color = 0;
            sh.pos = {200.0f, 100.0f, 0.0f};
            sh.speed1 = 2.0f;
            sh.flags = 0x100000;
            bm.fire(wd, sh);
            int slot = -1;
            for (int i = 0; i < th08::BulletManager::kBullets; ++i) {
                if (bm.bullet(i).state != 0 && bm.bullet(i).allowed == 0x100000u) slot = i;
            }
            CHECK(slot >= 0);
            if (slot >= 0) {
                th08::BulletManager::Bullet& b = bm.bullet(slot);
                // Ex 4: leaving the inner square (E/N) through its right edge scales the bullet
                // out by 135.76451 / 67.882256 about (192, 208) and turns it round.
                b.pos = {261.0f, 208.0f, 0.1f};
                b.velocity = {3.0f, 0.0f, 0.0f};
                b.angle = 0.0f;
                em.callExRoutine(wd, host, 4, 0);
                float x = 261.0f - 192.0f;
                x = x * std::bit_cast<float>(0x4307C3B7u);
                x = x / std::bit_cast<float>(0x4287C3B7u);
                x = x + 192.0f;
                CHECK(b.tag == 2 && b.pos.x == x && b.pos.y == 208.0f && b.velocity.x == -3.0f);
                CHECK(std::fabs(std::fabs(b.angle) - 3.1415927f) < 1e-6f);
                // The two-call cooldown, then nothing to do (no crossing).
                em.callExRoutine(wd, host, 4, 0);
                em.callExRoutine(wd, host, 4, 0);
                CHECK(b.tag == 0 && b.pos.x == x);
                em.callExRoutine(wd, host, 4, 0);
                CHECK(b.tag == 0 && b.pos.x == x);
                // Ex 12: the flagged group is held (additive, sprite + 16, no collision, the
                // velocity from 10016/10017) and the host's familiars freeze; then released.
                th08::Enemy& fam = em.enemy(401);
                host.next = &fam;
                fam.parent = &host;
                host.ctx.args.intA[0] = 0x100000;
                host.ctx.args.intA[1] = 0;
                host.ctx.args.floatA[0] = 0.0f;
                host.ctx.args.floatA[1] = 0.5f;
                b.vm.autoRotate = 1;
                b.speed = 2.0f;
                b.angle = 1.0f;
                const std::int16_t sprite = b.vm.activeSprite;
                em.callExRoutine(wd, host, 12, 0);
                CHECK(b.vm.autoRotate == 0 && b.noCollision && b.vm.blend() == 1 && b.velocity.x == 0.5f && fam.alwaysFrozen);
                CHECK(b.vm.activeSprite == sprite + 16 || b.vm.activeSprite == sprite);  // (n + 16 exists in etama)
                host.ctx.args.intA[1] = 1;
                em.callExRoutine(wd, host, 12, 0);
                CHECK(b.vm.autoRotate == 1 && !b.noCollision && b.vm.blend() == 0 && !fam.alwaysFrozen && b.vm.activeSprite == sprite);
                CHECK(std::fabs(b.velocity.x - 2.0f * std::cos(1.0f)) < 1e-5f);
                host.next = nullptr;
                fam.parent = nullptr;
            }
            // Ex 30: the screen-effect counter removes a shake while it is non-zero.
            em.callExRoutine(wd, host, 30, 2);
            CHECK(wd.screenSuppress == 2);
            wd.screen->shake(30, 2.0f, 2.0f);
            run(0, 1);
            CHECK(wd.screen->jobs() == 0 && wd.screenSuppress == 1);
            run(0, 1);
            CHECK(wd.screenSuppress == 0);
            // Ex 26: time stop on the low byte only.
            em.callExRoutine(wd, host, 26, 0x100);
            CHECK(!wd.timeStop);
            em.callExRoutine(wd, host, 26, 1);
            CHECK(wd.timeStop);
            em.callExRoutine(wd, host, 26, 0);
            CHECK(!wd.timeStop);

            // The spell card object (claude/th08-specs/spec_spellcard_hud.md): Begin, the drain,
            // a capture in the first seventh (P = 1000), the end effect paying P as time orbs from
            // t = 9 and the bonus at t = 130.
            th08::Spellcard& sc = *wd.spellcard;
            th08::Enemy& boss = em.enemy(450);
            boss = th08::Enemy{};
            boss.active = true;
            boss.slot = 450;
            boss.timerThreshold = 1800;
            boss.pos = {192.0f, 100.0f, 0.0f};
            std::array<std::uint8_t, 0xF4> card{};
            card[0x0E] = 7;                                    // card 7
            const std::uint32_t bonus = 2000000;
            std::memcpy(card.data() + 0x10, &bonus, 4);
            for (int i = 0; i < 48; ++i) card[static_cast<std::size_t>(0x14 + i)] = static_cast<std::uint8_t>((i < 4 ? 'A' + i : 0) ^ 0xAA);
            sc.begin(wd, boss, card.data());
            CHECK(sc.active() && sc.capturable() && sc.id() == 7 && sc.name() == "ABCD" && sc.capture() == bonus && sc.cap() == bonus);
            CHECK(sc.drain() == (bonus - bonus / 7) / 30 && sc.timer().current == 1800 && sc.circle() != nullptr);
            CHECK(sc.circle() && sc.circle()->segments == 64 && sc.circle()->radius == 256.0f);
            sc.update(wd);
            const std::uint32_t step = sc.drain() / 60;
            std::uint32_t expect = bonus - step;
            expect -= expect % 10;
            CHECK(sc.capture() == expect && sc.timer().current == 1799);
            // A time orb raises the value up to the cap, else the drain grows by 66.
            const std::uint32_t drain = sc.drain();
            sc.orbBonus(8000);
            CHECK(sc.capture() == bonus && sc.drain() == drain);
            sc.end(wd);
            const std::uint32_t scoreBefore = wd.state->score;
            CHECK(!sc.active() && sc.has(th08::Spellcard::Pending) && sc.has(th08::Spellcard::LastCaptured));
            CHECK(sc.pendingTime() == 1000 && sc.awarded() == bonus && sc.endEffect() != nullptr && sc.circle() == nullptr);
            CHECK(wd.state->cardsCaptured == 1 && wd.player->bombLockout() == 16);
            int frames = 0, orbFrames = 0;
            std::int32_t lastP = sc.pendingTime();
            while (sc.endEffect() && frames < 200) {
                sc.update(wd);
                if (sc.pendingTime() != lastP) ++orbFrames, lastP = sc.pendingTime();
                wd.effects->update(wd);
                ++frames;
            }
            CHECK(!sc.endEffect() && !sc.has(th08::Spellcard::Pending) && sc.pendingTime() == 0);
            CHECK(frames == 131 && orbFrames == 72);  // (1000 points: 14 a frame from t = 9)
            CHECK(wd.state->score == scoreBefore + bonus / 10 && wd.gui->messageOn(2));
            // A bomb fails the card (and marks it bombed); End then releases the circle with the
            // "Spell Bonus Failed" message, and pays nothing.
            sc.begin(wd, boss, card.data());
            sc.bombFailure();
            CHECK(!sc.capturable() && sc.capture() == 0 && sc.has(th08::Spellcard::Bombed));
            sc.end(wd);
            CHECK(!sc.active() && sc.circle() == nullptr && sc.endEffect() == nullptr && !sc.has(th08::Spellcard::Pending));
            CHECK(wd.gui->messageOn(1) && wd.state->cardsCaptured == 1);
            boss.active = false;
            // The gauge HUD's zone interrupt (spec_spellcard_hud 10.3): 2 over the gauge, 3 after.
            wd.gui->hudPlayerZone({100.0f, 420.0f, 0.0f});
            CHECK(wd.gui->hudInterrupt() == 2);
            wd.gui->hudPlayerZone({160.0f, 420.0f, 0.0f});  // (x = 160 keeps the state)
            CHECK(wd.gui->hudInterrupt() == 2);
            wd.gui->hudPlayerZone({200.0f, 420.0f, 0.0f});
            CHECK(wd.gui->hudInterrupt() == 3);
        }
        // The pause menu stops the game; Shoot on "resume" closes it after 20 passes.
        {
            th08::Gameplay::Setup main;
            main.character = 0;
            main.difficulty = 1;
            main.lives = 2;
            CHECK(game->start(main, &err));
            run(0, 30);
            const std::uint32_t frame = game->stageFrame();
            run(th08::BtnMenu, 1);
            CHECK(game->menu().active() && game->menu().pauseMenu());
            run(0, 10);
            CHECK(game->stageFrame() == frame);
            run(th08::BtnShoot, 1);
            run(0, 20);
            CHECK(!game->menu().active());
            run(0, 5);
            CHECK(game->stageFrame() > frame && game->outcome() == th08::Gameplay::Outcome::None);
            // Out of lives: the continue menu. "No" is the default; Up, then Shoot continues
            // after 90 passes: a continue costs 30 minutes of the night.
            if (haveTables) {
                run(th08::BtnLeft, 260);
                game->world().state->lives = 0.0f;
                game->world().state->bombs = 0.0f;
                const th08::Player& p = game->player();
                if (p.state() == th08::Player::State::Alive) {
                    game->world().player->bulletHit(game->world(), p.position(), {4.0f, 4.0f, 0.0f});
                    run(0, 40);
                    CHECK(game->menu().active() && !game->menu().pauseMenu());
                    run(0, 40);
                    run(th08::BtnUp, 1);
                    run(0, 5);
                    run(th08::BtnShoot, 1);
                    run(0, 95);
                    CHECK(!game->menu().active() && game->state().continues == 1 && game->state().lives == 2.0f && game->state().nightSteps == 1);
                    run(0, 40);
                    CHECK(p.state() == th08::Player::State::Invulnerable && game->outcome() == th08::Gameplay::Outcome::None);
                }
            }
        }
        game->end();
    }
    // Every stage's ECL (normal and spell practice) loads and runs 2400 frames of play: the
    // timelines spawn enemies, the enemies shoot, nothing runs away.
    if (const char* exePath = std::getenv("TH08_REAL_EXE")) {
        Vfs vfs;
        auto again = MappedFile::open(path, &err);
        CHECK(again && vfs.mount(openArchive("th08.dat", std::move(again), &err)));
        TextureLibrary textures;
        th08::AnmLibrary lib;
        CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
        CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
        th08::ExeTables tables;
        auto exe = MappedFile::open(exePath, &err);
        CHECK(exe && th08::loadExeTables(exe->bytes(), tables, &err));
        // The bomb names, read through each routine's push (spec_bomb_names): Shift-JIS, 14-34 bytes.
        for (const std::string& n : tables.bombNames) CHECK(n.size() >= 14 && n.size() <= 34);
        CHECK(tables.bombNames[0].size() == 16 && tables.bombNames[16].size() == 20 && tables.fakeCardName.size() > 4);
        CHECK(tables.bulletScripts[0][0] == 0 && tables.bulletScripts[0][1] == 18 && tables.bulletScripts[10][0] == 25 &&
              tables.bulletScripts[20][0] == 115 && tables.bulletScripts[20][4] == 17);
        // The scoring tables: Stage 1 Normal's time quota, the Extra entry that reads into the
        // rank table [QUIRK], the first extend.
        CHECK(tables.timeQuota[1] == 2500 && tables.timeQuota[36] == tables.rank[0].start && tables.rank[0].start == 10);
        CHECK(tables.extendMain[0] == 100 && tables.extendExtra[0] == 200 && tables.rank[4].start == 16);
        ZunRng rng(1);
        th08::FlowContext ctx;
        ctx.vfs = &vfs;
        ctx.textures = &textures;
        ctx.anm = &lib;
        ctx.rng = &rng;
        ctx.tables = &tables;
        auto game = std::make_unique<th08::Gameplay>(ctx);
        th08::MenuInput in;
        int stagesWithEnemies = 0, stagesWithBullets = 0;
        for (int sp = 0; sp < 2; ++sp) {
            for (int st = 0; st < th08::kStageCount; ++st) {
                th08::Gameplay::Setup setup;
                setup.stage = st;
                setup.practice = true;  // eight lives
                setup.spellPractice = sp != 0;
                setup.difficulty = st == 8 ? 4 : 2;
                setup.character = st % 12;
                const bool ok = game->start(setup, &err);
                CHECK(ok);
                if (!ok) {
                    std::fprintf(stderr, "  stage %d sp %d: %s\n", st, sp, err.c_str());
                    continue;
                }
                int maxEnemies = 0, maxBullets = 0;
                for (int f = 0; f < 2400 && game->outcome() == th08::Gameplay::Outcome::None; ++f) {
                    in.sample(f % 2 ? th08::BtnShoot : th08::BtnShoot | th08::BtnLeft);
                    game->tick(in);
                    maxEnemies = std::max(maxEnemies, game->enemies().liveCount());
                    maxBullets = std::max(maxBullets, game->bullets().liveBullets());
                }
                if (!sp) {
                    stagesWithEnemies += maxEnemies > 0 ? 1 : 0;
                    stagesWithBullets += maxBullets > 0 ? 1 : 0;
                }
                RecordingSink sink;
                game->draw(sink);
            }
        }
        CHECK(stagesWithEnemies == 9 && stagesWithBullets >= 8);
        std::printf("  (ECL: %d of 9 stages spawned enemies, %d fired bullets in 2400 frames)\n", stagesWithEnemies, stagesWithBullets);
        game->end();

        // Effects (spec_effects): the RNG cost per particle (script draws, then the init's),
        // the shared step budget and the sink, and the lifetimes the routines decide.
        if (!lib.loaded(th08::Effects::kSlotEtama)) CHECK(lib.load(vfs, textures, "etama.anm", th08::Effects::kSlotEtama, &err));
        auto fx = std::make_unique<th08::Effects>();
        th08::World w;
        ZunRng r(7);
        w.rng = &r;
        w.anm = &lib;
        w.tables = &tables;
        w.effects = fx.get();
        const Vec3f at{192, 224, 0};
        auto cost = [&](int type, int count, const Vec3f* custom = nullptr) {
            fx->reset();
            const std::uint32_t before = r.generations();
            if (custom) fx->spawnWithVector(w, type, at, *custom, count, 0xFFFFFFFFu);
            else fx->spawn(w, type, at, count, 0xFFFFFFFFu);
            return r.generations() - before;
        };
        const Vec3f none{-999, 0, 0}, angle{1.0f, 0, 0};
        CHECK(cost(3, 1) == 4 && cost(4, 1) == 4 && cost(11, 2) == 8);
        CHECK(cost(17, 1) == 6 && cost(18, 1) == 6 && cost(27, 1) == 6);
        CHECK(cost(38, 1) == 10 && cost(40, 1) == 4 && cost(47, 1) == 8);
        CHECK(cost(51, 1) == 26 && cost(63, 16) == 416);
        CHECK(cost(26, 1, &none) == 10 && cost(26, 1, &angle) == 8 && cost(21, 1, &none) == 8 && cost(21, 1) == 6);
        CHECK(cost(0, 1) == 0 && cost(13, 1) == 0 && cost(60, 1, &angle) == 0 && cost(39, 1) == 0);
        {
            fx->reset();
            const std::uint32_t before = r.generations();
            fx->spawnSpecial(w, 22, at, 2, 1, 0xFFFFFFFFu);
            CHECK(r.generations() - before == 2);  // every focus costs 2 steps
        }
        // A nearly full ring: of 16 sparks only 2 get slots (8 steps), and the sink comes back.
        fx->reset();
        fx->spawn(w, 0, at, 510, 0xFFFFFFFFu);
        std::uint32_t before = r.generations();
        CHECK(fx->isSink(fx->spawn(w, 4, at, 16, 0xFFFFFFFFu)) && r.generations() - before == 8);
        before = r.generations();
        CHECK(fx->isSink(fx->spawn(w, 4, at, 1, 0xFFFFFFFFu)) && r.generations() == before);
        // A spark slows to a stop after 19 passes; its script ends it at 30.
        fx->reset();
        th08::Effect& spark = fx->spawn(w, 4, at, 1, 0xFFFFFFFFu);
        const Vec3f v0 = spark.velocity;
        for (int k = 0; k < 19; ++k) fx->update(w);
        CHECK(spark.inUse && std::fabs(spark.velocity.x) < 1e-4f + std::fabs(v0.x) * 1e-3f && std::fabs(spark.velocity.y) < 1e-4f + std::fabs(v0.y) * 1e-3f);
        for (int k = 0; k < 16; ++k) fx->update(w);
        CHECK(!spark.inUse);
        // A released spell circle holds its slot for 16 more passes; the Dissolve ring lives
        // until its timer reaches 120.
        fx->reset();
        th08::Effect& circle = fx->spawn(w, 13, at, 1, 0xFF6060D0u);
        CHECK(circle.layer == 2);
        for (int k = 0; k < 50; ++k) fx->update(w);
        CHECK(circle.inUse);
        circle.fade = 1;
        for (int k = 0; k < 15; ++k) fx->update(w);
        CHECK(circle.inUse);
        fx->update(w);
        CHECK(!circle.inUse);
        th08::Effect& dissolve = fx->spawnSpecial(w, 50, at, 4, 1, 0xFFFFFFFFu);
        CHECK(dissolve.ring && dissolve.segments == 24);
        for (int k = 0; k < 120; ++k) fx->update(w);
        CHECK(dissolve.inUse && dissolve.segments == 32);
        fx->update(w);
        CHECK(!dissolve.inUse);
    }
    // The player's score.dat backup (a fresh file) decodes, with this exe's VRSM; the port's
    // fresh image is the same size, and saving it again changes only the flags it sets.
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
            CHECK(sd.musicHeard[0] == 1 && sd.musicHeard[1] == 0);  // the title theme
            CHECK(image && th08::freshScoreImage().size() == image->size());
            CHECK(sd.setMusicHeard(18) && sd.setMusicHeard(19));  // the ending themes
            const Bytes again = sd.imageToSave();
            std::size_t changed = 0;
            for (std::size_t i = 0; image && i < again.size() && i < image->size(); ++i) changed += again[i] != (*image)[i] ? 1 : 0;
            CHECK(image && again.size() == image->size() && changed == 2);
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

// The ring strip's positions (spec_visual_gaps 3.7 worked numbers).
void testRings() {
    th08::Effect e;
    e.segments = 64;
    e.width = 15.0f;
    e.radius = 256.0f;
    th08::Effects::RingPoints p{};
    CHECK(th08::Effects::ringGeometry(e, p) == 64);
    CHECK(std::fabs(p[0] - 303.01810f) < 1e-4f && p[1] == 16.0f && std::fabs(p[2] - 272.98190f) < 1e-4f && p[3] == 16.0f);
    // The last pair closes the ring (up to rounding).
    CHECK(std::fabs(p[64 * 4] - p[0]) < 1e-2f && std::fabs(p[64 * 4 + 1] - p[1]) < 1e-2f);
    // The stage-4A square: 4 segments from pi/4.
    e.segments = 4;
    e.width = 16.0f;
    e.radius = 192.0f;
    e.angle = kZunPi / 4.0f;
    e.centre = {192.0f, 128.0f, 0.0f};
    CHECK(th08::Effects::ringGeometry(e, p) == 4);
    CHECK(std::fabs(p[0] - 375.76450f) < 1e-3f && std::fabs(p[1] - 295.76450f) < 1e-3f);
    CHECK(std::fabs(p[2] - 343.76450f) < 1e-3f && std::fabs(p[3] - 263.76453f) < 1e-3f);
    // Wavy: six lobes; the radius at the start angle is R + R2 (phase 0), at half a lobe R - R2.
    e = th08::Effect{};
    e.segments = 64;
    e.width = 1.0f;
    e.radius = 100.0f;
    e.radius2 = 20.0f;
    e.waves = 6.0f;
    CHECK(th08::Effects::ringGeometry(e, p) == 64);
    const float rOuter0 = std::hypot(p[0] - 32.0f, p[1] - 16.0f);
    CHECK(std::fabs(rOuter0 - (120.0f + 1.0012f)) < 0.01f);
    // (64 / 12: pair 5.33 is half a lobe; check pair 16 = a whole 1.5 lobes -> trough)
    const float rTrough = std::hypot(p[16 * 4] - 32.0f, p[16 * 4 + 1] - 16.0f);
    CHECK(std::fabs(rTrough - (80.0f + 1.0012f)) < 0.01f);
    // An ellipse (second radius non-zero, no waves) ignores the start angle.
    e.waves = 0.0f;
    e.angle = 1.0f;
    th08::Effects::RingPoints q{};
    (void)th08::Effects::ringGeometry(e, q);
    e.angle = 0.0f;
    (void)th08::Effects::ringGeometry(e, p);
    CHECK(p == q && std::fabs(p[0] - (32.0f + 100.0f + 1.0012f)) < 0.01f);
}

// Spell Card Practice (spec_spell_practice): the exe's tables, the Last Word unlocks, a Last
// Word game from its own files, the end through the continue menu.
void testSpellPractice() {
    const char* path = std::getenv("TH08_REAL_DAT");
    const char* exePath = std::getenv("TH08_REAL_EXE");
    if (!path || !*path || !exePath || !*exePath) return;
    std::string err;
    th08::ExeTables tables;
    auto exe = MappedFile::open(exePath, &err);
    CHECK(exe && th08::loadExeTables(exe->bytes(), tables, &err));
    static constexpr std::size_t kCounts[10] = {13, 19, 22, 23, 23, 19, 28, 44, 14, 17};
    for (std::size_t s = 0; s < 10; ++s) CHECK(tables.stageCards[s].size() == kCounts[s]);
    CHECK(tables.stageCards[0][0] == 0 && tables.stageCards[9][0] == 205 && tables.stageCards[9][16] == 221);
    CHECK(tables.bossFinal.size() == 43 && tables.bossFinal.back() == 204);
    CHECK(tables.bossMusic.size() == 18 && tables.bossMusic[17].lastCard >= 221 && tables.bossMusic[17].track == 20 &&
          tables.bossMusic[17].file.find("th08_20") != std::string::npos && tables.bossMusic[11].restart == 1);
    CHECK(tables.stageLabels[9].rfind("Last Word", 0) == 0 && tables.lastWordEcl[11].find("ecldata_sk") != std::string::npos);
    CHECK(tables.lastWordEffects[0].find("eff09sk") != std::string::npos && !tables.practiceText.welcome.empty());
    CHECK(th08::MainMenu::cardDifficulty(&tables, 2) == 0 && th08::MainMenu::cardDifficulty(&tables, 3) == 1 &&
          th08::MainMenu::cardDifficulty(&tables, 195) == 4 && th08::MainMenu::cardDifficulty(&tables, 210) == 5);
    CHECK(th08::MainMenu::lastWordStage(208) == 5 && th08::MainMenu::lastWordStage(216) == 3);
    // Last Words: two characters with a no-continue Final B open No.206; a card seen opens No.205's
    // neighbours only through their own rules; nothing is ever locked again.
    {
        th08::ScoreData sd;
        sd.clears[0].noContinue[1] |= 0x4000;
        sd.updateLastWords(tables.bossFinal, tables.cardCategories[1]);
        CHECK(!sd.lastWordUnlocked(205));
        sd.clears[5].noContinue[3] |= 0x4000;
        sd.updateLastWords(tables.bossFinal, tables.cardCategories[1]);
        CHECK(sd.lastWordUnlocked(205) && !sd.lastWordUnlocked(206) && !sd.lastWordUnlocked(204));
        sd.cards[204].attempts[12] = 1;
        CHECK(sd.lastWordUnlocked(204));
        sd.clears[5].noContinue[3] = 1;
        sd.updateLastWords(tables.bossFinal, tables.cardCategories[1]);
        CHECK(sd.lastWordUnlocked(205));
    }
    // A Last Word game: Sakuya's (No.217) from ecldata_sk, the card declared through ex 19.
    Vfs vfs;
    auto file = MappedFile::open(path, &err);
    CHECK(file && vfs.mount(openArchive("th08.dat", std::move(file), &err)));
    TextureLibrary textures;
    th08::AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
    CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
    ZunRng rng(9);
    th08::FlowContext ctx;
    ctx.vfs = &vfs;
    ctx.textures = &textures;
    ctx.anm = &lib;
    ctx.rng = &rng;
    ctx.tables = &tables;
    th08::ScoreData records;
    ctx.records = &records;
    auto game = std::make_unique<th08::Gameplay>(ctx);
    th08::Gameplay::Setup setup;
    setup.spellPractice = true;
    setup.card = 216;
    setup.stage = th08::MainMenu::lastWordStage(216);
    setup.difficulty = 1;
    setup.character = 2;
    CHECK(game->start(setup, &err));
    if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
    CHECK(game->state().card == 216 && game->state().power == 128.0f && game->state().lives == 8.0f && game->state().nightSteps == 0);
    CHECK(records.practice[2].plays[3 * 5 + 1] == 1 && records.totalPlays == 0);
    th08::MenuInput in;
    int declared = -1;
    for (int f = 0; f < 600 && declared < 0; ++f) {
        in.sample(0);
        game->tick(in);
        if (game->world().spellcard && game->world().spellcard->active()) declared = game->world().spellcard->id();
    }
    CHECK(declared == 216 && records.cards[216].practiceAttempts[2] == 1 && records.cards[216].attempts[2] == 0);
    std::printf("  (spell practice No.217 declared: card %d)\n", declared);
    game->end();
}

// The four attract demos as a determinism test (spec_replays 6): each plays its stage from the
// recorded state and seed with the recorded input; the stage results (MSG op 9) must come on
// frame F - 3 with the recorded score, misses and bombs.
void testDemoReplays() {
    const char* path = std::getenv("TH08_REAL_DAT");
    const char* exePath = std::getenv("TH08_REAL_EXE");
    if (!path || !*path || !exePath || !*exePath) return;
    std::string err;
    th08::ExeTables tables;
    auto exe = MappedFile::open(exePath, &err);
    CHECK(exe && th08::loadExeTables(exe->bytes(), tables, &err));
    Vfs vfs;
    auto file = MappedFile::open(path, &err);
    CHECK(file && vfs.mount(openArchive("th08.dat", std::move(file), &err)));
    struct Want {
        const char* name;
        int stage, character;
        std::uint16_t seed;
        std::size_t entries;
        std::uint32_t score;
        int misses, bombs;
    };
    static constexpr Want kDemos[4] = {
        {"demorpy0.rpy", 5, 0, 0x8D07, 11084, 7065227, 11, 1},
        {"demorpy1.rpy", 3, 1, 0x684D, 7741, 5656648, 9, 1},
        {"demorpy2.rpy", 2, 3, 0xD39A, 9273, 6858553, 10, 2},
        {"demorpy3.rpy", 1, 2, 0x0322, 9264, 7433064, 9, 1},
    };
    for (const Want& w : kDemos) {
        const auto bytes = vfs.read(w.name);
        CHECK(bytes.has_value());
        if (!bytes) continue;
        const auto rf = th08::ReplayFile::parse(*bytes, &err);
        CHECK(rf.has_value());
        if (!rf) {
            std::fprintf(stderr, "  %s: %s\n", w.name, err.c_str());
            continue;
        }
        CHECK(rf->firstStage() == w.stage && rf->summary.character() == w.character && rf->summary.difficulty() == 3);
        const th08::ReplayStage& st = rf->stages[static_cast<std::size_t>(w.stage)];
        CHECK(st.seed == w.seed && st.entries.size() == w.entries && st.lives == 8 && st.rank == 8 && rf->summary.score() == w.score);
        CHECK(st.entries[0] == 0 && !st.fps.empty());
        // A round trip through the writer reads back the same blocks.
        {
            th08::ReplayFile copy = *rf;
            ZunRng wr(5);
            const auto again = th08::ReplayFile::parse(copy.serialize(wr, "test"), &err);
            CHECK(again && again->stages[static_cast<std::size_t>(w.stage)].entries == st.entries && again->summary.score() == w.score);
        }
        // The playback.
        TextureLibrary textures;
        th08::AnmLibrary lib;
        CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
        CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
        ZunRng rng(1);
        th08::FlowContext ctx;
        ctx.vfs = &vfs;
        ctx.textures = &textures;
        ctx.anm = &lib;
        ctx.rng = &rng;
        ctx.tables = &tables;
        th08::ScoreData records;
        ctx.records = &records;
        auto game = std::make_unique<th08::Gameplay>(ctx);
        th08::Gameplay::Setup setup;
        setup.replay = &*rf;
        setup.stage = w.stage;
        CHECK(game->start(setup, &err));
        CHECK(game->stageSeed() == w.seed);
        th08::MenuInput in;
        std::size_t resultsFrame = 0;
        const std::size_t f = w.entries - 5;
        for (std::size_t n = 0; n < f + 400 && game->outcome() == th08::Gameplay::Outcome::None; ++n) {
            in.sample(0);
            game->tick(in);
            if (resultsFrame == 0 && (game->world().mode & th08::ModeResults)) resultsFrame = game->replayFrame();
        }
        const th08::GameState& g = game->state();
        std::printf("  (%s: results at entry %zu (want %zu), score %u (want %u), misses %d (%d), bombs %d (%d), frame %zu)\n", w.name,
                    resultsFrame, f - 3 + 1, g.score, w.score, static_cast<int>(g.misses), w.misses, static_cast<int>(g.bombsUsedGame), w.bombs,
                    game->replayFrame());
        // (The port does not stay in sync with the original's demos yet: these are reported,
        // not checked. A port-recorded replay must play back exactly: testReplayRoundTrip.)
        (void)resultsFrame;
        game->end();
    }
}

// A run recorded by the port plays back exactly (spec_replays 2-3): a practice stage under
// scripted input, written, read back and replayed, reaches the same state frame for frame.
void testReplayRoundTrip() {
    const char* path = std::getenv("TH08_REAL_DAT");
    const char* exePath = std::getenv("TH08_REAL_EXE");
    if (!path || !*path || !exePath || !*exePath) return;
    std::string err;
    th08::ExeTables tables;
    auto exe = MappedFile::open(exePath, &err);
    CHECK(exe && th08::loadExeTables(exe->bytes(), tables, &err));
    Vfs vfs;
    auto file = MappedFile::open(path, &err);
    CHECK(file && vfs.mount(openArchive("th08.dat", std::move(file), &err)));
    TextureLibrary textures;
    th08::AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
    CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
    ZunRng rng(77);
    th08::FlowContext ctx;
    ctx.vfs = &vfs;
    ctx.textures = &textures;
    ctx.anm = &lib;
    ctx.rng = &rng;
    ctx.tables = &tables;
    th08::ScoreData records;
    ctx.records = &records;
    struct Snap {
        std::uint32_t score;
        float x, y, lives, bombs, power;
        std::uint16_t seed;
        int enemies, bullets;
        bool operator==(const Snap&) const = default;
    };
    auto snap = [&](th08::Gameplay& g) {
        return Snap{g.state().score, g.player().position().x, g.player().position().y, g.state().lives, g.state().bombs, g.state().power,
                    rng.currentSeed(), g.enemies().liveCount(), g.bullets().liveBullets()};
    };
    struct Run {
        int stage, character, card;
    };
    for (const Run run : {Run{0, 0, -1}, Run{3, 6, -1}, Run{0, 1, 3}, Run{3, 2, 216}}) {
        const int stage = run.stage;
        // The live run (a practice stage, or a spell practice card).
        auto live = std::make_unique<th08::Gameplay>(ctx);
        th08::Gameplay::Setup setup;
        setup.character = run.character;
        setup.difficulty = run.card >= 205 ? 1 : 2;
        setup.practice = run.card < 0;
        setup.spellPractice = run.card >= 0;
        setup.card = run.card;
        setup.stage = stage;
        CHECK(live->start(setup, &err));
        th08::MenuInput in;
        std::uint32_t lcg = 12345;
        auto buttons = [&](int frame) {
            lcg = lcg * 1103515245u + 12345u;
            std::uint16_t b = th08::BtnShoot;
            const std::uint32_t r = lcg >> 16;
            if (frame % 40 < 20) b |= (r & 1) ? th08::BtnLeft : th08::BtnRight;
            if (r & 4) b |= th08::BtnFocus;
            if ((r & 0x3F0) == 0x100) b |= th08::BtnUp;
            if ((r & 0xFFF) == 7) b |= th08::BtnBomb;
            return b;
        };
        constexpr int kFrames = 3000;
        std::vector<Snap> want;
        for (int f = 0; f < kFrames && live->outcome() == th08::Gameplay::Outcome::None; ++f) {
            in.sample(buttons(f));
            live->tick(in);
            want.push_back(snap(*live));
        }
        CHECK(live->recording());
        live->closeRecording();
        th08::ReplayFile rec = live->recorded();
        live->end();
        CHECK(rec.hasStage(stage) && rec.stages[static_cast<std::size_t>(stage)].entries.size() <= want.size() + 2);  // (menus are not recorded)
        ZunRng writer(3);
        const auto bytes = rec.serialize(writer, "user");
        const auto back = th08::ReplayFile::parse(bytes, &err);
        CHECK(back.has_value());
        if (!back) continue;
        // The playback, from another RNG state.
        rng.seed(999);
        auto play = std::make_unique<th08::Gameplay>(ctx);
        th08::Gameplay::Setup ps;
        ps.replay = &*back;
        ps.stage = stage;
        CHECK(play->start(ps, &err));
        std::size_t firstDiff = want.size();
        for (std::size_t f = 0; f < want.size() && play->outcome() == th08::Gameplay::Outcome::None; ++f) {
            in.sample(0);
            play->tick(in);
            if (firstDiff == want.size() && !(snap(*play) == want[f])) firstDiff = f;
        }
        std::printf("  (stage %d card %d: %zu frames recorded, first difference at %zu)\n", stage, run.card, want.size(), firstDiff);
        CHECK(firstDiff == want.size());
        play->end();
    }
}

// The result screen and the endings on the player's files (spec_result_ending 3, 5, 7).
void testResultAndEnding() {
    const char* path = std::getenv("TH08_REAL_DAT");
    if (!path || !*path) return;
    std::string err;
    Vfs vfs;
    auto file = MappedFile::open(path, &err);
    CHECK(file && vfs.mount(openArchive("th08.dat", std::move(file), &err)));
    TextureLibrary textures;
    th08::AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
    CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
    ZunRng rng(5);
    th08::FlowContext ctx;
    ctx.vfs = &vfs;
    ctx.textures = &textures;
    ctx.anm = &lib;
    ctx.rng = &rng;
    int saves = 0;
    th08::Scene asked = th08::Scene::Init;
    ctx.saveScores = [&](bool gameRng) { saves += gameRng ? 1 : 100; };
    ctx.request = [&](th08::Scene s) { asked = s; };
    th08::MenuInput in;
    th08::GameState g;
    g.character = 2;
    g.difficulty = 1;
    g.stage = 3;
    g.score = g.guiScore = 1234567;
    g.runFrames = 5000;
    g.runPointItems = 321;
    g.misses = 2;
    g.pauses = 3;
    g.humanFrames = 300;
    g.countedFrames = 400;
    g.runCaptures[7] = 1;
    // After a game: rank 0 among the placeholders, the name "A" through the grid, END.
    {
        th08::ScoreData records;
        th08::ResultScreen rs(ctx, th08::ResultScreen::Mode::AfterGame, g, records);
        CHECK(rs.create(&err));
        auto pass = [&](std::uint16_t b) {
            in.sample(b);
            rs.update(in);
        };
        pass(0);
        CHECK(rs.state() == th08::ResultScreen::State::NameEntry && rs.rank() == 0 && rs.keyCursor() == 0);
        CHECK(records.highScores.size() == 1 && records.highScores[0][9] == 1);
        pass(th08::BtnShoot);  // (pass 1: input starts at pass 10)
        CHECK(rs.nameCursor() == 0);
        for (int i = 0; i < 9; ++i) pass(0);
        pass(th08::BtnShoot);  // 'A'
        pass(0);
        pass(th08::BtnRight);
        pass(0);
        pass(th08::BtnShoot);  // 'B'
        pass(0);
        CHECK(rs.nameCursor() == 2 && rs.name()[0] == 'A' && rs.name()[1] == 'B' && rs.keyCursor() == 1);
        pass(th08::BtnBomb);  // backspace
        pass(0);
        CHECK(rs.nameCursor() == 1 && rs.name()[1] == ' ');
        pass(th08::BtnLeft);
        pass(0);
        pass(th08::BtnUp);  // 0 - 16 wraps to 80
        pass(0);
        CHECK(rs.keyCursor() == 80);
        pass(th08::BtnLeft);  // within the row: 80 -> 95 (END)
        pass(0);
        CHECK(rs.keyCursor() == th08::ResultScreen::kKeyEnd);
        pass(th08::BtnShoot);
        CHECK(rs.state() == th08::ResultScreen::State::Statistics);
        const th08::HighScoreRecord& r = records.highScores[0];
        std::uint32_t score = 0, frames = 0;
        std::memcpy(&score, r.data() + 0x0C, 4);
        std::memcpy(&frames, r.data() + 0x64, 4);
        std::int32_t rate = 0;
        std::memcpy(&rate, r.data() + 0x84, 4);
        CHECK(std::memcmp(r.data(), "HSCR", 4) == 0 && r[9] == 0 && score == 1234567 && frames == 5000 && rate == 7500);
        CHECK(r[0x14] == 2 && r[0x15] == 1 && r[0x16] == 3 && r[0x88 + 7] == 1 && std::memcmp(r.data() + 0x17, "A       ", 8) == 0);
        CHECK(records.hasLastName && std::memcmp(records.lastName.data(), "A       ", 8) == 0);
        int n = 0;
        for (; n < 200 && rs.state() == th08::ResultScreen::State::Statistics; ++n) pass(n % 2 ? th08::BtnShoot : 0);
        CHECK(n >= 90 && rs.state() == th08::ResultScreen::State::StatisticsOut);
        RecordingSink sink;
        rs.draw(sink);
        CHECK(!sink.items.empty());
        for (n = 0; n < 100 && !rs.done(); ++n) pass(0);
        CHECK(rs.done() && saves == 1 && asked == th08::Scene::MainMenu);
        // The list as drawn: this run first, then the placeholders.
        const auto rows = records.listRows(1, 2);
        CHECK(rows[0].record != nullptr && rows[1].record == nullptr && rows[1].placeholderScore == 100000);
        // A second run that does not qualify goes straight to the statistics.
        th08::GameState low = g;
        low.score = low.guiScore = 5;
        saves = 0;
        th08::ResultScreen again(ctx, th08::ResultScreen::Mode::AfterGame, low, records);
        CHECK(again.create(&err));
        in.sample(0);
        again.update(in);
        CHECK(again.state() == th08::ResultScreen::State::Statistics && again.rank() == -1 && records.highScores.size() == 2);
        CHECK(records.highScores[1][9] == 0);
    }
    // A practice result: the best is raised, then the screen leaves.
    {
        th08::ScoreData records;
        th08::GameState p = g;
        p.practice = true;
        p.character = 3;
        p.stage = 2;
        records.practice[3].best[2 * 5 + 1] = 99;
        saves = 0;
        asked = th08::Scene::Init;
        th08::ResultScreen rs(ctx, th08::ResultScreen::Mode::PracticeEnd, p, records);
        CHECK(rs.create(&err));
        CHECK(records.practice[3].best[11] == 1234567 && records.practice[3].hasData && rs.state() == th08::ResultScreen::State::Leave);
        for (int n = 0; n < 40 && !rs.done(); ++n) {
            in.sample(0);
            rs.update(in);
        }
        CHECK(rs.done() && saves == 1 && asked == th08::Scene::MainMenu && records.highScores.empty());
    }
    // Player Data (spec 2) with the exe's tables: the menu, the statistics page's lines, the
    // spell list's counts, the score table's character, the leave.
    if (const char* exePath = std::getenv("TH08_REAL_EXE")) {
        th08::ExeTables tables;
        auto exe = MappedFile::open(exePath, &err);
        CHECK(exe && th08::loadExeTables(exe->bytes(), tables, &err));
        CHECK(tables.cardCategories[0].size() == 42 && tables.cardCategories[5].size() == 222 && tables.cardCategories[5][221] == 221);
        CHECK(!tables.fullNames[12].empty() && tables.dataText.playsRow.find("%6d") != std::string::npos && tables.dataText.unknownCard.size() == 10);
        std::printf("  (Easy cards start %d %d; Extra %d-%d)\n", tables.cardCategories[0][0], tables.cardCategories[0][1], tables.cardCategories[4][0],
                    tables.cardCategories[4][13]);
        ctx.tables = &tables;
        th08::ScoreData records;
        th08::CardRecord& card = records.cards[static_cast<std::size_t>(tables.cardCategories[0][1])];
        card.present = true;
        card.captures[2] = 1;
        card.attempts[12] = 2;
        std::memcpy(card.name.data(), "TEST", 4);
        records.plstAdd(1, th08::ScoreData::PerCharacter + 2, 7);
        records.plstAdd(1, th08::ScoreData::Clears, 3);
        th08::ResultScreen rs(ctx, th08::ResultScreen::Mode::Viewer, th08::GameState{}, records);
        CHECK(rs.create(&err) && rs.state() == th08::ResultScreen::State::Menu);
        CHECK(records.plstValue(6, th08::ScoreData::Clears) == 3);  // (the totals recomputed)
        CHECK(rs.capturedCount(5, 2) == 1 && rs.capturedCount(5, 12) == 0 && rs.capturedCount(0, 2) == 1);
        auto pass = [&](std::uint16_t b) {
            in.sample(b);
            rs.update(in);
        };
        for (int i = 0; i < 21; ++i) pass(0);
        pass(th08::BtnDown);
        pass(th08::BtnDown);  // (held: no second move)
        pass(0);
        pass(th08::BtnDown);
        pass(0);
        CHECK(rs.menuCursor() == 2);
        pass(th08::BtnShoot);
        CHECK(rs.state() == th08::ResultScreen::State::DataIn);
        for (int i = 0; i < 45; ++i) pass(0);
        CHECK(rs.state() == th08::ResultScreen::State::DataShown);
        CHECK(rs.textLine(3).rfind(tables.fullNames[0], 0) == 0 && rs.textLine(5).find("      7") != std::string::npos);
        CHECK(rs.textLine(16).find("     3") != std::string::npos);
        pass(th08::BtnShoot);
        for (int i = 0; i < 25; ++i) pass(0);
        CHECK(rs.state() == th08::ResultScreen::State::Menu && rs.menuCursor() == 2);
        // The spell list: category 0 (Easy), column 2; card 3 (No.04) shows its name.
        for (int i = 0; i < 21; ++i) pass(0);
        pass(th08::BtnUp);
        pass(0);
        pass(th08::BtnShoot);
        CHECK(rs.state() == th08::ResultScreen::State::SpellCategory);
        for (int i = 0; i < 7; ++i) pass(0);
        pass(th08::BtnDown);  // 5 (all) wraps to 0 (Easy)
        pass(0);
        CHECK(rs.cursor() == 0);
        pass(th08::BtnShoot);
        CHECK(rs.state() == th08::ResultScreen::State::SpellColumn && rs.category() == 0);
        for (int i = 0; i < 7; ++i) pass(0);
        for (int k = 0; k < 3; ++k) {  // 12 wraps to 0, then 2
            pass(th08::BtnDown);
            pass(0);
        }
        pass(th08::BtnShoot);
        CHECK(rs.state() == th08::ResultScreen::State::SpellList && rs.column() == 2);
        for (int i = 0; i < 12; ++i) pass(0);
        CHECK(rs.textLine(0) == tables.dataText.unknownCard && rs.textLine(1) == "TEST" && rs.textLine(10).rfind("  1/ 42", 0) == 0);
        RecordingSink sink;
        rs.draw(sink);
        CHECK(!sink.items.empty());
        // Back out to the menu and leave: score.dat is written, the title follows.
        pass(th08::BtnBomb);
        for (int i = 0; i < 12; ++i) pass(0);
        CHECK(rs.state() == th08::ResultScreen::State::SpellColumn);
        for (int i = 0; i < 7; ++i) pass(0);  // (input after 6 passes)
        pass(th08::BtnBomb);
        CHECK(rs.state() == th08::ResultScreen::State::SpellCategory);
        for (int i = 0; i < 7; ++i) pass(0);
        pass(th08::BtnBomb);
        CHECK(rs.state() == th08::ResultScreen::State::Menu && rs.menuCursor() == 1);
        for (int i = 0; i < 21; ++i) pass(0);
        pass(th08::BtnBomb);  // the cursor to Exit
        pass(0);
        pass(th08::BtnBomb);  // and out
        saves = 0;
        asked = th08::Scene::Init;
        for (int i = 0; i < 30 && !rs.done(); ++i) pass(0);
        CHECK(rs.done() && saves == 1 && asked == th08::Scene::MainMenu);
        ctx.tables = nullptr;
    }
    // The replay prompt after a practice game (spec 4): Yes, slot 1 (empty), the name grid's
    // END, and the replay is written under the remembered name.
    {
        th08::ScoreData records;
        std::memcpy(records.lastName.data(), "ME      ", 8);
        records.hasLastName = true;
        th08::GameState p = g;
        p.practice = true;
        int savedSlot = 0;
        std::string savedName;
        ctx.replayToSave = true;
        ctx.replaySlot = [](int) { return std::optional<std::array<std::uint8_t, 0xCC>>{}; };
        ctx.saveReplay = [&](int slot, const std::string& name) {
            savedSlot = slot;
            savedName = name;
            return true;
        };
        saves = 0;
        th08::ResultScreen rs(ctx, th08::ResultScreen::Mode::PracticeEnd, p, records);
        CHECK(rs.create(&err) && rs.state() == th08::ResultScreen::State::ReplaySave);
        auto pass = [&](std::uint16_t b) {
            in.sample(b);
            rs.update(in);
        };
        for (int i = 0; i < 13; ++i) pass(0);
        pass(th08::BtnShoot);  // Yes
        CHECK(rs.state() == th08::ResultScreen::State::ReplaySlots);
        for (int i = 0; i < 21; ++i) pass(0);
        pass(th08::BtnShoot);  // slot 1
        CHECK(rs.state() == th08::ResultScreen::State::ReplayName && rs.keyCursor() == th08::ResultScreen::kKeyEnd);
        for (int i = 0; i < 31; ++i) pass(0);
        pass(th08::BtnShoot);  // END (the remembered name)
        CHECK(savedSlot == 1 && savedName == "ME      " && rs.state() == th08::ResultScreen::State::Leave);
        for (int i = 0; i < 25 && !rs.done(); ++i) pass(0);
        CHECK(rs.done() && saves == 1);
        ctx.replayToSave = false;
        ctx.replaySlot = nullptr;
        ctx.saveReplay = nullptr;
    }
    // An ending script: lines into the text VMs, @w waits, @r clears, @z ends.
    {
        th08::Ending e(ctx);
        const char script[] = "@s4\0 2\0\r\nLine one\r\nLine two\r\n@w30\0 10\0\r\n@r5\0 0\0\r\nLine three\r\n@z\r\n";
        e.startScript(std::vector<std::uint8_t>(script, script + sizeof script - 1), "test.end");
        asked = th08::Scene::Init;
        int frames = 0;
        for (; frames < 400 && !e.done(); ++frames) {
            in.sample(0);
            e.update(in);
            if (frames == 1) {
                CHECK(e.lineSlot() == 1 && e.lastLine() == "Line one");
            }
        }
        CHECK(e.done() && asked == th08::Scene::ResultAfterGame && e.lastLine() == "Line three");
        // (4 + 1 + 4 + 1, the @w 30 + 1, the @r 5 + 1, the last line's 4)
        CHECK(frames > 40 && frames < 60);
    }
    // A good ending on the real files: Final A for team 0, the records written first; Shoot
    // advances it to the staff roll and the end.
    {
        th08::ScoreData records;
        th08::Ending e(ctx);
        th08::Ending::Run run;
        run.character = 0;
        run.difficulty = 2;
        run.stage = 6;
        run.finished = true;
        CHECK(e.create(run, &records, &err));
        CHECK(e.script() == "end00b.end" && !e.seen());
        CHECK((records.clears[0].noContinue[2] & 0x8000) != 0 && (records.clears[0].reached[2] & 0x8000) != 0);
        CHECK(records.musicHeard[18] == 1 && records.musicHeard[19] == 1 && records.plstValue(2, th08::ScoreData::Clears) == 1 &&
              records.plstValue(6, th08::ScoreData::Clears) == 1);
        asked = th08::Scene::Init;
        bool staff = false;
        int frames = 0;
        for (; frames < 200000 && !e.done(); ++frames) {
            in.sample(frames % 2 ? th08::BtnShoot : 0);
            e.update(in);
            staff = staff || e.script() == "data/staff00b.end";
        }
        CHECK(e.done() && staff && asked == th08::Scene::ResultAfterGame);
        std::printf("  (Final A ending and staff roll: %d frames with Shoot tapped)\n", frames);
        // A bad ending: no records but the theme flag; the bad ending file.
        th08::Ending bad(ctx);
        run.finished = false;
        run.stage = 5;
        run.character = 7;  // a solo: team 1
        CHECK(bad.create(run, &records, &err) && bad.script() == "end01a.end" && records.musicHeard[18] == 0x12);
    }
}

}  // namespace

// The title's Replay menu (claude/th08-specs/spec_replay_menu.md §1) on the player's files, with
// the archive's demo replays standing in for saved ones.
void testReplayMenu() {
    const char* path = std::getenv("TH08_REAL_DAT");
    const char* exePath = std::getenv("TH08_REAL_EXE");
    if (!path || !*path || !exePath || !*exePath) return;
    std::string err;
    th08::ExeTables tables;
    auto exe = MappedFile::open(exePath, &err);
    CHECK(exe && th08::loadExeTables(exe->bytes(), tables, &err));
    CHECK(tables.replayDifficulties[3] == "Lunatic " && tables.replayStages[3] == "Stage4A" && tables.replayStages[8] == "StageEX");
    Vfs vfs;
    auto file = MappedFile::open(path, &err);
    CHECK(file && vfs.mount(openArchive("th08.dat", std::move(file), &err)));
    TextureLibrary textures;
    th08::AnmLibrary lib;
    CHECK(lib.load(vfs, textures, "text.anm", th08::kSlotText, &err, true));
    CHECK(lib.load(vfs, textures, "ascii.anm", th08::kSlotAscii, &err));
    ZunRng rng(5);
    auto demo = [&](int i) { return *vfs.read(std::format("demorpy{}.rpy", i)); };
    // A spell-practice replay (card 5) and a file from another executable, made from the demos.
    auto remade = [&](int i, auto&& change) {
        auto r = th08::ReplayFile::parse(demo(i));
        CHECK(r.has_value());
        change(*r);
        ZunRng w(9);
        return r->serialize(w);
    };
    const auto spell = remade(3, [](th08::ReplayFile& r) {
        r.summary.setCard(5);
        std::array<std::uint8_t, 48> name{};
        std::memcpy(name.data(), "Test Sign", 9);
        r.summary.setCardName(name);
    });
    const auto foreign = remade(0, [](th08::ReplayFile& r) { r.summary.setExe(1234, 5678); });
    std::map<std::string, std::vector<std::uint8_t>> files = {
        {"replay/th8_01.rpy", demo(0)},
        {"replay/th8_03.rpy", demo(1)},
        {"replay/th8_04.rpy", {1, 2, 3}},
        {"replay/th8_05.rpy", foreign},
        {"replay/th8_ud0001.rpy", spell},
    };
    std::vector<std::string> order = {"replay/th8_01.rpy", "replay/th8_03.rpy", "replay/th8_04.rpy", "replay/th8_05.rpy", "replay/th8_ud0001.rpy"};
    th08::FlowContext ctx;
    ctx.vfs = &vfs;
    ctx.textures = &textures;
    ctx.anm = &lib;
    ctx.rng = &rng;
    ctx.tables = &tables;
    std::vector<th08::Scene> requests;
    ctx.request = [&](th08::Scene s) { requests.push_back(s); };
    ctx.replayFiles = [&]() { return order; };
    ctx.readReplay = [&](const std::string& p) {
        const auto it = files.find(p);
        return it == files.end() ? std::vector<std::uint8_t>{} : it->second;
    };
    th08::MainMenu menu(ctx);
    CHECK(menu.create(&err));
    th08::MenuInput in;
    auto pass = [&](std::uint16_t buttons) {
        in.sample(buttons);
        menu.update(in);
    };
    auto tap = [&](std::uint16_t buttons) {
        pass(buttons);
        pass(0);
    };
    for (int i = 0; i < 20; ++i) pass(0);
    for (int i = 0; i < 5; ++i) tap(th08::BtnUp);  // Game Start -> Quit, Option, Music Room, Player Data, Replay
    CHECK(menu.cursor() == th08::MainMenu::Replay);
    pass(th08::BtnShoot);
    CHECK(menu.state() == th08::MainMenu::State::Replays && menu.subState() == 0);
    pass(0);  // the entry pass E: the scan
    const auto& list = menu.replayEntries();
    CHECK(list.size() == 3);  // (a broken file and another version's are left out)
    if (list.size() == 3) {
        CHECK(list[0].label == "No.01" && list[1].label == "No.03" && list[2].label == "User ");
        CHECK(list[1].present[3] && !list[1].present[0] && list[2].summary.card() == 5);
    }
    for (int i = 1; i <= 7; ++i) pass(i == 3 ? th08::BtnDown : 0);  // E+1 .. E+7: nothing (not even a move)
    CHECK(menu.subState() == 0 && menu.cursor() == 0);
    pass(0);  // E+8
    CHECK(menu.subState() == 1);
    pass(th08::BtnDown);  // E+9: moves
    CHECK(menu.cursor() == 1 && menu.chosenReplay() == 1);
    for (int i = 10; i <= 17; ++i) pass(i == 16 ? th08::BtnShoot : 0);  // Select not before E+18
    CHECK(menu.subState() == 1);
    {
        const auto text = menu.replayMenuText();
        CHECK(text.size() == 4 && text[0].text == "No.   Name       Date  Player   Rank");
        if (text.size() == 4) {
            CHECK(text[2].text == "No.03 ZUN        07/25 Ms & Al  Lunatic ");
            CHECK(text[2].selected && text[2].color == 0xFFFFFFFFu && !text[1].selected && text[1].color == 0xFF808080u);
            CHECK(text[3].text.starts_with("User ") && text[3].text.ends_with("Spell No.  6"));
        }
    }
    pass(th08::BtnShoot);  // E+18
    CHECK(menu.subState() == 2 && menu.chosenStage() == 3);  // the demo's only stage, 4A
    tap(th08::BtnDown);
    CHECK(menu.cursor() == 3);  // (absent slots are skipped all the way round)
    {
        const auto text = menu.replayMenuText();
        CHECK(text.size() == 4 + 2 + 9);
        if (text.size() == 15) {
            CHECK(text[4].text.starts_with("       ") && text[4].text.ends_with("%") && text[5].text == "Stage    LastScore");
            CHECK(text[6].text == "Stage1  ----------" && text[9].text == "Stage4A   56566480" && text[9].selected);
            CHECK(text[14].text == "StageEX ----------" && text[6].color == 0xFF808080u);
        }
    }
    tap(th08::BtnShoot);
    CHECK(menu.subState() == 3 && menu.cursor() == 0);
    {
        const auto text = menu.replayMenuText();
        CHECK(text.size() == 15 && text[9].color == 0x60FFFFFFu && !text[9].selected && text[6].color == 0x60808080u);
    }
    tap(th08::BtnDown);
    tap(th08::BtnDown);
    CHECK(menu.cursor() == 2);
    tap(th08::BtnDown);
    CHECK(menu.cursor() == 0);  // three modes for a normal replay
    tap(th08::BtnBomb);
    CHECK(menu.subState() == 2 && menu.cursor() == 3);
    tap(th08::BtnBomb);
    CHECK(menu.subState() == 1 && menu.cursor() == 1);
    tap(th08::BtnDown);
    tap(th08::BtnShoot);  // (the state timer was not reset: Select works at once)
    CHECK(menu.subState() == 2 && menu.chosenReplay() == 2);
    {
        const auto text = menu.replayMenuText();
        bool spellLine = false;
        for (const auto& l : text) spellLine = spellLine || (l.text.starts_with("No.006 ") && l.text.ends_with("0") && l.text.size() == 17);
        CHECK(spellLine && text.size() == 4 + 3);
    }
    const int spellStage = menu.chosenStage();
    tap(th08::BtnShoot);
    CHECK(menu.subState() == 3);
    tap(th08::BtnDown);
    tap(th08::BtnDown);
    CHECK(menu.cursor() == 0);  // two modes for a spell-practice replay
    tap(th08::BtnDown);
    CHECK(requests.empty());
    pass(th08::BtnShoot);
    CHECK(menu.removed() && requests.size() == 1 && requests[0] == th08::Scene::Game);
    CHECK(ctx.replayFlag && ctx.replayPath == "replay/th8_ud0001.rpy" && ctx.replayStage == spellStage && ctx.replayMode == 1);
    CHECK(ctx.spellPractice && ctx.card == 5 && !ctx.practice);
    // After the playback the title opens the replay menu, on the first file, character 0.
    ctx.character = 3;
    th08::MainMenu after(ctx);
    CHECK(after.create(&err));
    CHECK(ctx.character == 0);
    th08::MenuInput ia;
    ia.sample(0);
    after.update(ia);
    CHECK(after.state() == th08::MainMenu::State::Replays && !ctx.replayFlag);
    for (int i = 0; i < 19; ++i) after.update(ia);
    CHECK(after.subState() == 1 && after.cursor() == 0 && after.replayEntries().size() == 3);
    ia.sample(th08::BtnBomb);
    after.update(ia);
    CHECK(after.subState() == 4);
    ia.sample(0);
    for (int i = 0; i < 30; ++i) after.update(ia);
    CHECK(after.state() == th08::MainMenu::State::Title && after.cursor() == th08::MainMenu::Replay);
    // After a demo: the title.
    ctx.replayFlag = ctx.demo = true;
    th08::MainMenu demoAfter(ctx);
    CHECK(demoAfter.create(&err));
    CHECK(!ctx.replayFlag && !ctx.demo);
    ia.sample(0);
    demoAfter.update(ia);
    CHECK(demoAfter.state() == th08::MainMenu::State::Title);
    // Pages of 15 with more files: Right and Left wrap by the count.
    order.clear();
    for (int i = 1; i <= 15; ++i) order.push_back(std::format("replay/th8_{:02d}.rpy", i));
    for (int i = 0; i < 5; ++i) order.push_back(std::format("replay/th8_ud{:04d}.rpy", i));
    for (const auto& p : order) files[p] = demo(2);
    th08::MainMenu paged(ctx);
    CHECK(paged.create(&err));
    th08::MenuInput ip;
    auto ppass = [&](std::uint16_t b) {
        ip.sample(b);
        paged.update(ip);
    };
    for (int i = 0; i < 20; ++i) ppass(0);
    for (int i = 0; i < 5; ++i) {
        ppass(th08::BtnUp);
        ppass(0);
    }
    ppass(th08::BtnShoot);
    for (int i = 0; i < 12; ++i) ppass(0);
    CHECK(paged.replayEntries().size() == 20 && paged.subState() == 1);
    for (int i = 0; i < 7; ++i) {
        ppass(th08::BtnDown);
        ppass(0);
    }
    CHECK(paged.cursor() == 7);
    ppass(th08::BtnRight);
    CHECK(paged.cursor() == 2);
    ppass(0);
    ppass(th08::BtnLeft);
    CHECK(paged.cursor() == 7);
    ppass(0);
    ppass(th08::BtnUp);
    ppass(0);
    for (int i = 0; i < 7; ++i) {
        ppass(th08::BtnUp);
        ppass(0);
    }
    CHECK(paged.cursor() == 19);
    const auto text = paged.replayMenuText();
    CHECK(text.size() == 1 + 5 && text.back().selected && text[1].text.starts_with("User "));
    RecordingSink sink;
    paged.draw(sink);
    CHECK(sink.items.size() > 20);
    // Playback mode 1: a recorded 25 fps runs every second pass; the sample is read along.
    th08::ScoreData records;
    ctx.records = &records;
    auto slow = th08::ReplayFile::parse(demo(1));
    CHECK(slow.has_value());
    if (!slow) return;
    for (auto& b : slow->stages[3].fps) b = 25;
    slow->stages[3].fps[3] = 0x80 | 25;
    std::size_t frames[2] = {};
    for (int mode = 0; mode < 2; ++mode) {
        auto g = std::make_unique<th08::Gameplay>(ctx);
        th08::Gameplay::Setup ps;
        ps.replay = &*slow;
        ps.stage = 3;
        ps.playbackMode = mode;
        CHECK(g->start(ps, &err));
        th08::MenuInput gi;
        for (int f = 0; f < 300 && g->outcome() == th08::Gameplay::Outcome::None; ++f) {
            gi.sample(0);
            g->tick(gi);
            if (g->replayFrame() == 61) CHECK(g->replayFps() == 25 && g->replayFpsFlagged());
        }
        frames[mode] = g->replayFrame();
        CHECK(g->replayFps() == 25);
        RecordingSink gs;
        g->draw(gs);
        g->end();
    }
    std::printf("  (300 passes: %zu entries read in mode 0, %zu in mode 1)\n", frames[0], frames[1]);
    CHECK(frames[0] >= 290 && frames[1] + 2 >= frames[0] / 2 && frames[1] <= frames[0] / 2 + 2);
}

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"block cipher: vectors, limits, round trips", testCipher},
        {"PBGZ archive (th08.dat)", testPbgz},
        {"th08.exe 1.00d check", testExeCheck},
        {"screen effects: shake RNG, ramps, freeze", testScreenEffects},
        {"menu input: pressed, auto-repeat, select and cancel", testMenuInput},
        {"th08.cfg: layout, validation, re-initialisation", testGameConfig},
        {"score.dat: cipher, rolling XOR, checksum, records, unlocks, Music Room flags, saving", testScoreFile},
        {"musiccmt.txt: tracks, comment lines, track lookup", testMusicComments},
        {"ANM v3: files, sprites by position, opcodes 9/33/82-89, interrupt return, freeze", testAnm},
        {"version file: every matching line, exe checksum", testVersionFile},
        {"ring strip: flat, square, wavy and ellipse positions", testRings},
        {"the player's th08.dat (TH08_REAL_DAT)", testRealArchive},
        {"result screen and endings (TH08_REAL_DAT)", testResultAndEnding},
        {"spell card practice: tables, Last Words, a Last Word game (TH08_REAL_DAT, TH08_REAL_EXE)", testSpellPractice},
        {"the four demo replays played back (TH08_REAL_DAT, TH08_REAL_EXE)", testDemoReplays},
        {"a recorded run played back frame for frame (TH08_REAL_DAT, TH08_REAL_EXE)", testReplayRoundTrip},
        {"the replay menu: scan, timing, lists, modes, start, return, pages (TH08_REAL_DAT, TH08_REAL_EXE)", testReplayMenu},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
