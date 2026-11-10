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
#include "th08/GameFiles.hpp"

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

void testExeCheck() {
    std::string why;
    CHECK(!th08::isExe100d(Bytes(100, 0), &why));
    CHECK(why.find("840704") != std::string::npos);
    CHECK(!th08::isExe100d(Bytes(th08::kExeSize, 0), &why));
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
