#include "game/ScreenShake.hpp"

#include "game/World.hpp"

namespace th06 {

void ScreenShake::add(int length, float from, float to) { jobs_.push_back({length, from, to, 0}); }

void ScreenShake::update(World& world) {
    GameState& gs = *world.state;
    if (gs.timeStop != 0) {  // held: the playfield sits still, the shake resumes afterwards
        gs.resetPlayfield();
        return;
    }
    for (std::size_t i = 0; i < jobs_.size();) {
        Job& j = jobs_[i];
        if (++j.timer >= j.length) {
            gs.resetPlayfield();
            jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const float o = static_cast<float>(j.timer) * (j.to - j.from) / static_cast<float>(j.length) + j.from;
        switch (world.rng->u32InRange(3)) {
            case 0:
                gs.playfieldX = kGameRegionX;
                gs.playfieldW = kGameRegionWidth;
                break;
            case 1:
                gs.playfieldX = kGameRegionX + o;
                gs.playfieldW = kGameRegionWidth - o;
                break;
            default:
                gs.playfieldX = kGameRegionX;
                gs.playfieldW = kGameRegionWidth - o;
                break;
        }
        switch (world.rng->u32InRange(3)) {
            case 0:
                gs.playfieldY = kGameRegionY;
                gs.playfieldH = kGameRegionHeight;
                break;
            case 1:
                gs.playfieldY = kGameRegionY + o;
                gs.playfieldH = kGameRegionHeight - o;
                break;
            default:
                gs.playfieldY = kGameRegionY;
                gs.playfieldH = kGameRegionHeight - o;
                break;
        }
        ++i;
    }
}

void ScreenShake::clear(World& world) {
    jobs_.clear();
    world.state->resetPlayfield();
}

}  // namespace th06
