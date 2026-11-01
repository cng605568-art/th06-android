#pragma once

#include <string>
#include <vector>

#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"

namespace th06 {

class Vfs;

// Sprites the demo stage draws. Taken from the player's archives when present
// (bullets from etama3.anm, the player from player00.anm), else generated stand-ins.
struct GameArt {
    UiArt ui;
    std::vector<SpriteRegion> bullets;
    SpriteRegion player;
    SpriteRegion shot;
    SpriteRegion spawner;
    bool bulletsFromGameData = false;
    bool playerFromGameData = false;
    std::string dataLabel;    // shown in the HUD
    std::string bulletsNote;  // why game bullets were not used (empty = fine)
    std::string playerNote;   // why the game's player sprite was not used
};

GameArt buildGameArt(const Vfs* vfs, TextureLibrary& lib);

}  // namespace th06
