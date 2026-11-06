#include "th06/ItemManager.hpp"

#include <algorithm>
#include <cmath>

#include "th06/BulletManager.hpp"
#include "th06/Gui.hpp"
#include "th06/Player.hpp"
#include "th06/Resources.hpp"

namespace touhou::th06 {
namespace {

constexpr float kSpawnRiseSpeed = -2.2f;
constexpr float kGravity = 0.03f;
constexpr float kMaxFallSpeed = 3.0f;
constexpr float kMagnetSpeed = 8.0f;
constexpr float kRemoveBelowY = kGameRegionHeight + 16.0f;
constexpr float kItemHalfSize = 8.0f;
constexpr float kGrabHalfSize = 12.0f;
constexpr int kScatterFrames = 60;

constexpr int kSoundItem = 21;
constexpr int kSoundPowerUp = 31;
constexpr int kSoundExtend = 28;

// Score per small/big power item at full power, by chain index.
constexpr std::uint32_t kFullPowerScores[31] = {
    10,   20,   30,   40,   50,   60,   70,   80,    90,    100,   200,   300,   400,   500,   600,  700,
    800,  900,  1000, 2000, 3000, 4000, 5000, 6000,  7000,  8000,  9000,  10000, 11000, 12000, 51200,
};

struct PointValue {
    std::uint32_t top, base, slope;
};
constexpr PointValue kPointValues[5] = {
    {100000, 60000, 100}, {100000, 60000, 100}, {150000, 100000, 180}, {200000, 150000, 270}, {300000, 200000, 400},
};

}  // namespace

ItemManager::ItemManager() = default;

void ItemManager::reset() {
    items_->fill(Item{});
    next_ = 0;
}

std::uint32_t ItemManager::pointItemValue(int difficulty, float y) {
    const PointValue& v = kPointValues[std::clamp(difficulty, 0, 4)];
    const auto row = static_cast<std::int32_t>(y);  // truncates toward zero
    if (row < static_cast<std::int32_t>(kCollectLineY)) return v.top;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(v.base) -
                                      static_cast<std::int64_t>(row - 128) * static_cast<std::int64_t>(v.slope));
}

std::uint32_t ItemManager::fullPowerScore(int chain) { return kFullPowerScores[std::clamp(chain, 0, 30)]; }

bool ItemManager::spawn(World& world, const Vec3f& pos, int type, ItemState state) {
    if (type < 0 || type >= kItemTypeCount) return false;
    auto& pool = *items_;
    int slot = -1;
    for (int probe = 0; probe < kMaxItems; ++probe) {
        const int k = (next_ + probe) % kMaxItems;
        if (!pool[static_cast<std::size_t>(k)].active) {
            slot = k;
            break;
        }
    }
    if (slot < 0) return false;
    next_ = (slot + 1) % kMaxItems;

    Item& it = pool[static_cast<std::size_t>(slot)];
    it = Item{};
    it.active = true;
    it.type = static_cast<std::int8_t>(type);
    it.state = state;
    it.pos = pos;
    it.prevPos = pos;
    it.velocity = {0.0f, kSpawnRiseSpeed, 0.0f};
    it.timer = 0;
    if (state == ItemState::DeathScatter) {
        it.target.x = world.rng->f32InRange(288.0f) + 48.0f;
        it.target.y = world.rng->f32InRange(192.0f) - 64.0f;
        it.target.z = 0.0f;
        it.velocity = pos;  // start of the scatter lerp
    }
    world.anm->setAndExecuteScript(it.vm, kAnmScriptFirstItem + type);
    it.vm.color = 0xffffffffu;
    it.showingIndicator = false;
    return true;
}

void ItemManager::magnetAll() {
    for (Item& it : *items_) {
        if (it.active) it.state = ItemState::Magnet;
    }
}

int ItemManager::activeCount() const {
    int n = 0;
    for (const Item& it : *items_) n += it.active;
    return n;
}

void ItemManager::holdFrame() {
    for (Item& it : *items_) it.prevPos = it.pos;
}

void ItemManager::update(World& world) {
    GameState& gs = *world.state;
    const Player& player = *world.player;
    const Vec3f grabTL = player.positionCenter - Vec3f{kGrabHalfSize, kGrabHalfSize, 0.0f};
    const Vec3f grabBR = player.positionCenter + Vec3f{kGrabHalfSize, kGrabHalfSize, 0.0f};
    const bool canCollect = player.state == Player::Alive || player.state == Player::Invulnerable;
    bool collectedAny = false;

    for (Item& it : *items_) {
        if (!it.active) continue;
        it.prevPos = it.pos;

        bool moves = true;
        if (it.state == ItemState::DeathScatter) {
            if (it.timer < kScatterFrames) {
                const float s = static_cast<float>(it.timer) / static_cast<float>(kScatterFrames);
                it.pos = it.target * s + it.velocity * (1.0f - s);
                moves = false;
            } else if (it.timer == kScatterFrames) {
                it.velocity = {};
            }
        } else if (it.state == ItemState::Magnet ||
                   (gs.power >= GameState::kMaxPower && player.positionCenter.y < kCollectLineY)) {
            const float dx = player.positionCenter.x - it.pos.x;
            const float dy = player.positionCenter.y - it.pos.y;
            const float a = (dx == 0.0f && dy == 0.0f) ? kZunHalfPi : std::atan2(dy, dx);
            it.velocity.x = std::cos(a) * kMagnetSpeed;
            it.velocity.y = std::sin(a) * kMagnetSpeed;
            it.state = ItemState::Magnet;
        } else {
            it.velocity.x = 0.0f;
            it.velocity.z = 0.0f;
            if (it.velocity.y < kSpawnRiseSpeed) it.velocity.y = kSpawnRiseSpeed;
        }

        if (moves) {
            it.pos += it.velocity;
            if (it.pos.y >= kRemoveBelowY) {
                it.active = false;
                gs.decreaseSubrank(3);
                continue;
            }
            if (it.velocity.y < kMaxFallSpeed) it.velocity.y += kGravity;
            else it.velocity.y = kMaxFallSpeed;
        }

        if (canCollect) {
            const bool separated = it.pos.x + kItemHalfSize < grabTL.x || it.pos.x - kItemHalfSize > grabBR.x ||
                                   it.pos.y + kItemHalfSize < grabTL.y || it.pos.y - kItemHalfSize > grabBR.y;
            if (!separated) {
                collect(world, it);
                it.active = false;
                collectedAny = true;
                ++collectedTotal_;
                continue;
            }
        }
        ++it.timer;
        world.anm->executeScript(it.vm);
    }
    if (collectedAny) world.playSound(kSoundItem);
}

void ItemManager::fullPowerReached(World& world) {
    world.bullets->removeAllBullets(world, true);  // every bullet and laser becomes a star
    if (world.gui) world.gui->showFullPower();
}

void ItemManager::collect(World& world, Item& it) {
    GameState& gs = *world.state;
    switch (it.type) {
        case ItemPowerSmall:
        case ItemPowerBig: {
            const bool big = it.type == ItemPowerBig;
            if (gs.power >= GameState::kMaxPower) {
                gs.fullPowerChain = std::min(gs.fullPowerChain + (big ? 8 : 1), 30);
                const std::uint32_t v = fullPowerScore(gs.fullPowerChain);
                gs.score += v;
                if (world.gui) world.gui->popup(it.pos, static_cast<std::int32_t>(v), v >= 12800 ? 0xffffff00u : 0xffffffffu);
            } else {
                const int before = gs.powerLevel();
                if (!big) gs.fullPowerChain = 0;
                gs.power += big ? 8 : 1;
                if (gs.power >= GameState::kMaxPower) {
                    gs.power = GameState::kMaxPower;
                    fullPowerReached(world);
                }
                gs.score += 10;
                const bool levelUp = gs.powerLevel() != before;
                if (levelUp) world.playSound(kSoundPowerUp);
                if (world.gui) world.gui->popup(it.pos, levelUp ? -1 : 10, levelUp ? 0xff80c0ffu : 0xffffffffu);
            }
            if (!big) gs.increaseSubrank(1);
            break;
        }
        case ItemPoint: {
            const std::uint32_t v = pointItemValue(gs.difficulty, it.pos.y);
            gs.score += v;
            ++gs.stagePointItems;
            ++gs.totalPointItems;
            gs.increaseSubrank(it.pos.y < kCollectLineY ? 30 : 3);
            if (world.gui) {
                const bool top = v == pointItemValue(gs.difficulty, 0.0f);
                world.gui->popup(it.pos, static_cast<std::int32_t>(v), top ? 0xffffff00u : 0xffffffffu);
            }
            break;
        }
        case ItemBomb:
            if (gs.bombs < GameState::kMaxBombs) ++gs.bombs;
            gs.increaseSubrank(5);
            break;
        case ItemFullPower:
            if (gs.power < GameState::kMaxPower) {
                fullPowerReached(world);
                world.playSound(kSoundPowerUp);
                if (world.gui) world.gui->popup(it.pos, -1, 0xff80c0ffu);
            }
            gs.power = GameState::kMaxPower;
            gs.score += 1000;
            if (world.gui) world.gui->popup(it.pos, 1000, 0xffffffffu);
            break;
        case ItemLife:
            if (gs.livesRemaining < GameState::kMaxLives) ++gs.livesRemaining;
            gs.increaseSubrank(200);
            world.playSound(kSoundExtend);
            break;
        case ItemStar: {
            const std::uint32_t v = gs.bombActive ? 100u : (gs.grazes / 3) * 10 + 500;
            gs.score += v;
            if (world.gui) world.gui->popup(it.pos, static_cast<std::int32_t>(v), 0xffffffffu, true);
            break;
        }
        default:
            break;
    }
}

void ItemManager::draw(World& world, SpriteSink& sink, float alpha) {
    for (Item& it : *items_) {
        if (!it.active) continue;
        const Vec3f p = lerp(it.prevPos, it.pos, alpha);
        float y = p.y;
        if (p.y < -8.0f) {
            y = 8.0f;  // pinned to the top edge of the playfield
            if (!it.showingIndicator) {
                world.anm->setActiveSprite(it.vm, kAnmSpriteFirstIndicator + it.type);
                it.showingIndicator = true;
            }
            int a = 255 - static_cast<int>((8.0f - p.y) * 255.0f / 128.0f);
            if (a < 64) a = 64;
            it.vm.color = (it.vm.color & 0x00ffffffu) | (static_cast<std::uint32_t>(a) << 24);
        } else if (it.showingIndicator) {
            world.anm->setActiveSprite(it.vm, kAnmSpriteFirstItem + it.type);
            it.showingIndicator = false;
            it.vm.color = 0xffffffffu;
        }
        it.vm.pos = {world.state->playfieldX + p.x, world.state->playfieldY + y, 0.01f};
        world.anm->drawScreen(sink, it.vm);
    }
}

}  // namespace touhou::th06
