#pragma once

// Player bombs (player spell cards), owned by the player and run inside its update:
//   Reimu A  "Dream Seal"           up to 7 homing orbs that explode
//   Reimu B  "Evil-Sealing Circle"  4 beams fixed where the bomb started
//   Marisa A "Stardust Reverie"     8 stars flying outwards
//   Marisa B "Master Spark"         a full-width beam above the player, 0.3x movement
//
// Each frame the bomb writes damage regions (32 slots, used by the enemy pass) and cancel
// rectangles (16 slots, used by the bullet pass and against enemy contact). Both are wiped
// at the start of every player update, so a box lives only on frames where the bomb writes
// it again. Sprites come from the player's own ANM file (player00.anm / player01.anm).

#include <array>
#include <cstdint>

#include "core/ZunMath.hpp"
#include "gfx/AnmManager.hpp"

namespace th06 {

class Player;
class SpriteSink;
struct SpriteRegion;
struct World;

class Bomb {
  public:
    static constexpr int kRegions = 32;
    static constexpr int kCancelRects = 16;
    static constexpr int kParts = 8;

    struct Region {
        Vec3f center, size;  // live while size.x > 0
        int damage = 0;
        int accumulated = 0;  // raw damage this region has dealt (Reimu A orbs explode at 100)
    };
    struct Rect {
        Vec3f center, size;  // live while size.x != 0
    };

    void reset();
    // Start of every player update: all regions and rectangles stop being live (only their
    // widths are cleared; positions persist, which Reimu B relies on).
    void clearBoxes();
    void start(World& world, Player& player);
    void step(World& world, Player& player);
    // A frozen frame (time stop): nothing moves; the boxes stay as the last step left them.
    void holdFrame() { partPrevPos_ = partPos_; }

    // Bomb sprites and the darkening overlay (player layer, before the player sprite).
    void draw(World& world, SpriteSink& sink, const SpriteRegion& white, float alpha, const Vec3f& playerPos);
    // GUI pass: the bomb name banner's own timers.
    void updateGui();

    // Damage from all live regions overlapping the enemy box [tl, br]. Each overlap also
    // adds to that region's accumulated damage. Sets *hit when a region touched the enemy
    // while the bomb is active. `particleCounter` is the player's shared hit-spark counter.
    int damageEnemy(World& world, const Vec3f& enemyPos, const Vec3f& tl, const Vec3f& br, bool* hit,
                    std::uint8_t& particleCounter);
    // True if the box [tl, br] overlaps any live cancel rectangle.
    bool cancels(const Vec3f& tl, const Vec3f& br) const;

    float moveMultiplier() const { return moveMultiplier_; }
    void resetMoveMultiplier() { moveMultiplier_ = 1.0f; }

    // Inspection (tests, HUD).
    int timer() const { return timer_; }
    int duration() const { return duration_; }
    int type() const { return type_; }
    const Region& region(int i) const { return regions_[static_cast<std::size_t>(i)]; }
    const Rect& cancelRect(int i) const { return cancel_[static_cast<std::size_t>(i)]; }
    int liveRegions() const;
    int liveCancelRects() const;
    int partState(int i) const { return partState_[static_cast<std::size_t>(i)]; }
    const Vec3f& partPosition(int i) const { return partPos_[static_cast<std::size_t>(i)]; }
    const Vec3f& partVelocity(int i) const { return partVel_[static_cast<std::size_t>(i)]; }
    int overlayAlpha() const;
    // Name banner: the English name, and how far it has slid in / faded out (0..1).
    const char* name() const;
    float nameVisibility() const;

  private:
    void showName(World& world);
    void stepReimuA(World& world, Player& player, int t);
    void stepReimuB(World& world, Player& player, int t);
    void stepMarisaA(World& world, Player& player, int t);
    void stepMarisaB(World& world, Player& player, int t);
    void setRegion(int i, const Vec3f& center, const Vec3f& size, int damage);
    void setRect(int i, const Vec3f& center, const Vec3f& size);
    void startVm(World& world, AnmVm& vm, int script);

    std::array<Region, kRegions> regions_{};
    std::array<Rect, kCancelRects> cancel_{};
    std::array<Vec3f, kParts> partPos_{}, partPrevPos_{}, partVel_{};
    std::array<int, kParts> partState_{};
    std::array<float, kParts> partSpeed_{};
    std::array<std::array<AnmVm, 4>, kParts> vms_{};
    int type_ = 0;
    int timer_ = 0;
    int duration_ = 999;
    float moveMultiplier_ = 1.0f;
    int nameTimer_ = -1;   // frames since the name was shown, -1 = none
    int nameLeaving_ = -1;  // frames since the name started leaving, -1 = not leaving
};

}  // namespace th06
