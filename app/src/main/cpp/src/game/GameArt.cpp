#include "game/GameArt.hpp"

#include <cctype>

#include "core/Log.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

// Script IDs inside the 2002 game's ANM files (from the th06 decompilation's AnmIdx.hpp).
constexpr std::uint32_t kEtamaScriptRingBall = 1;
constexpr std::uint32_t kEtamaScriptBall = 3;
constexpr std::uint32_t kPlayerScriptIdle = 0;
constexpr int kBulletColors = 16;

// Short upper-case reason for the HUD (the built-in font has no lower case).
std::string hudReason(std::string text) {
    for (const char* prefix : {"data/", "ANM: "}) {
        for (auto pos = text.find(prefix); pos != std::string::npos; pos = text.find(prefix)) {
            text.erase(pos, std::char_traits<char>::length(prefix));
        }
    }
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// Bullets of one type in all its colours: the script's first sprite + colour 0..15,
// the same rule the original engine uses when it spawns a coloured bullet.
std::vector<SpriteRegion> bulletColours(const LoadedAnm& etama, const TextureLibrary& lib,
                                        std::uint32_t scriptId) {
    std::vector<SpriteRegion> out;
    const std::int32_t base = etama.anm.scriptFirstSprite(scriptId);
    if (base < 0) return out;
    for (int colour = 0; colour < kBulletColors; ++colour) {
        const int index = etama.anm.spriteIndexById(static_cast<std::int64_t>(base) + colour);
        if (index >= 0) out.push_back(etama.sprite(lib, static_cast<std::size_t>(index)));
    }
    return out;
}

}  // namespace

GameArt buildGameArt(const Vfs* vfs, TextureLibrary& lib) {
    GameArt art;
    art.ui = registerUiArt(lib);
    const FallbackGameArt fallback = registerFallbackGameArt(lib);
    art.bullets = fallback.bullets;
    art.player = fallback.player;
    art.shot = fallback.shot;
    art.spawner = fallback.spawner;

    if (!vfs || vfs->fileCount() == 0) {
        art.dataLabel = "DEMO - NO GAME DATA";
        return art;
    }
    art.dataLabel = vfs->hasFormat("PBG3") ? "CLASSIC 2002 (PBG3)" : "NEW CLASSIC (PKGL)";

    std::string err;
    if (auto etama = loadAnm(*vfs, lib, "data/etama3.anm", &err)) {
        auto palette = bulletColours(*etama, lib, kEtamaScriptBall);
        if (palette.size() < 4) palette = bulletColours(*etama, lib, kEtamaScriptRingBall);
        if (palette.size() >= 4) {
            art.bullets = std::move(palette);
            art.bulletsFromGameData = true;
        } else {
            art.bulletsNote = "NO BALL BULLET SCRIPT IN ETAMA3.ANM";
        }
    } else {
        art.bulletsNote = hudReason(err);
    }
    if (!art.bulletsNote.empty()) log::warn("Bullets: {}", art.bulletsNote);

    err.clear();
    if (auto player = loadAnm(*vfs, lib, "data/player00.anm", &err)) {
        // Idle pose: the first sprite of script 0; else the first 32x48 sprite.
        int index = player->anm.spriteIndexById(player->anm.scriptFirstSprite(kPlayerScriptIdle));
        for (std::size_t i = 0; index < 0 && i < player->anm.sprites.size(); ++i) {
            const AnmSprite& s = player->anm.sprites[i];
            if (s.w == 32.0f && s.h == 48.0f) index = static_cast<int>(i);
        }
        if (index >= 0) {
            art.player = player->sprite(lib, static_cast<std::size_t>(index));
            art.playerFromGameData = true;
        } else {
            art.playerNote = "NO IDLE SPRITE (" + std::to_string(player->anm.sprites.size()) + " SPRITES, " +
                             std::to_string(player->anm.scripts.size()) + " SCRIPTS)";
        }
    } else {
        art.playerNote = hudReason(err);
    }
    if (!art.playerNote.empty()) log::warn("Player: {}", art.playerNote);
    return art;
}

}  // namespace th06
