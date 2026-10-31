#include "game/GameArt.hpp"

#include "core/Log.hpp"
#include "io/Vfs.hpp"

namespace th06 {

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
    // Bullet palette: every 16x16 sprite on the bullet sheet, in file order. Chosen by
    // shape rather than by hard-coded IDs, so it only depends on the sprite table.
    if (auto etama = loadAnm(*vfs, lib, "data/etama3.anm", &err)) {
        std::vector<SpriteRegion> palette;
        for (std::size_t i = 0; i < etama->anm.sprites.size() && palette.size() < 64; ++i) {
            const AnmSprite& s = etama->anm.sprites[i];
            if (s.w == 16.0f && s.h == 16.0f) palette.push_back(etama->sprite(lib, i));
        }
        if (palette.size() >= 4) {
            art.bullets = std::move(palette);
            art.bulletsFromGameData = true;
        } else {
            log::warn("etama3.anm has no 16x16 bullet sprites; using generated bullets");
        }
    } else {
        log::warn("Bullet sheet unavailable ({}); using generated bullets", err);
    }

    err.clear();
    if (auto player = loadAnm(*vfs, lib, "data/player00.anm", &err)) {
        for (std::size_t i = 0; i < player->anm.sprites.size(); ++i) {
            const AnmSprite& s = player->anm.sprites[i];
            if (s.w == 32.0f && s.h == 48.0f) {
                art.player = player->sprite(lib, i);
                art.playerFromGameData = true;
                break;
            }
        }
    } else {
        log::warn("Player sprites unavailable ({}); using generated player", err);
    }
    return art;
}

}  // namespace th06
