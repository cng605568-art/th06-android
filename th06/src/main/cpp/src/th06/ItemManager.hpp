#pragma once

// Items: power, point, big power, bomb, full power, 1up and the star items cancelled bullets
// turn into. A 512-slot pool searched from a rotating cursor; items rise, fall under gravity,
// home in on the player when magnetised (or at full power above the collection line), and
// are collected by a 24x24 grab box. Sprites and animation come from etama3.anm scripts
// 0x215..0x21B; items above the top edge are drawn as an indicator pinned to the edge.

#include <array>

#include "core/HeapArray.hpp"
#include <cstdint>
#include <memory>

#include "core/ZunMath.hpp"
#include "th06/World.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/SpriteSink.hpp"

namespace touhou::th06 {

enum ItemType : std::int8_t {
    ItemPowerSmall = 0,
    ItemPoint = 1,
    ItemPowerBig = 2,
    ItemBomb = 3,
    ItemFullPower = 4,
    ItemLife = 5,
    ItemStar = 6,
    kItemTypeCount = 7,
};

// Enemy drop codes besides the item types.
inline constexpr std::int8_t kItemDropRandom = -1;

enum class ItemState : std::uint8_t { Falling, Magnet, DeathScatter };

struct Item {
    AnmVm vm;
    Vec3f pos;
    Vec3f prevPos;      // render interpolation (not part of the original)
    Vec3f velocity;     // DeathScatter: the start position while the scatter lerp runs
    Vec3f target;       // DeathScatter destination
    std::int32_t timer = 0;
    std::int8_t type = ItemPowerSmall;
    ItemState state = ItemState::Falling;
    bool active = false;
    bool showingIndicator = false;
};

class ItemManager {
  public:
    static constexpr int kMaxItems = 512;
    static constexpr int kAnmScriptFirstItem = 0x200 + 21;
    static constexpr int kAnmSpriteFirstItem = 0x200;
    static constexpr int kAnmSpriteFirstIndicator = 0x200 + 7;
    static constexpr float kCollectLineY = 128.0f;  // full power: items home in above this line

    ItemManager();

    void reset();  // stage load: empty pool, cursor 0
    // Returns false (and draws no random numbers) when all 512 slots are taken.
    bool spawn(World& world, const Vec3f& pos, int type, ItemState state);
    void update(World& world);  // runs at the start of the bullet manager's update
    void holdFrame();           // a frozen frame (time stop): nothing moves
    void magnetAll();           // every item, of any state, homes in (bombs)
    void draw(World& world, SpriteSink& sink, float alpha);

    int activeCount() const;
    const Item& at(int i) const { return (*items_)[static_cast<std::size_t>(i)]; }
    Item& at(int i) { return (*items_)[static_cast<std::size_t>(i)]; }
    std::uint64_t collectedTotal() const { return collectedTotal_; }

    // Value of a point item collected at height y (playfield pixels) on a difficulty.
    static std::uint32_t pointItemValue(int difficulty, float y);
    // Score for small/big power items once power is full, by chain index 0..30.
    static std::uint32_t fullPowerScore(int chain);

  private:
    void collect(World& world, Item& item);
    void fullPowerReached(World& world);

    HeapArray<Item, kMaxItems> items_;
    int next_ = 0;
    std::uint64_t collectedTotal_ = 0;
};

}  // namespace touhou::th06
