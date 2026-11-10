#pragma once

// Imperishable Night's bullet manager (claude/th08-specs/spec_player_bullets_items.md §15-17,
// in TH07's shape): 1536 enemy bullets (patterns with nine aim modes, six-command lists and
// their modifiers, grazing, cancelling into items), 256 lasers, and the 2096 items kept in
// spawn order (time orbs, scatter, homing, pickup and every item's effect). Calc priority 14
// (items, then bullets, then lasers); draw priority 13.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/ZunMath.hpp"
#include "th08/Anm.hpp"
#include "th08/World.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th08 {

// One bullet command (24 bytes in the shooter record).
struct BulletCommand {
    float a = 0, b = 0;
    std::int32_t duration = 0, loops = 0;
    std::uint32_t type = 0;
    std::int32_t immediate = 0;
};

// A pattern request (the "shooter" record, spec_ecl 10.1).
struct Shooter {
    std::int16_t type = 0, color = 0;
    Vec3f pos;
    float angle1 = 0, angle2 = 0, speed1 = 0, speed2 = 0;
    std::array<BulletCommand, 6> commands{};
    std::int16_t count1 = 1, count2 = 1;
    std::uint16_t aimMode = 0;
    std::uint32_t flags = 0;
    int sound = 7, bulletSound = 25;
};

struct Laser {
    AnmVm beam, orb;
    Vec3f origin;
    float angle = 0, start = 0, end = 0, startLength = 0, width = 0, targetWidth = 0, speed = 0;
    std::int32_t startTime = 0, duration = 0, endTime = 0, hitboxStart = 0, hitboxEnd = 0;
    bool inUse = false;
    Timer timer;
    std::uint16_t flags = 0;
    std::int16_t color = 0;
    std::uint8_t state = 0;  // 0 warning, 1 active, 2 shrink
    std::uint8_t hideWarning = 0;
};

class BulletManager {
  public:
    static constexpr int kBullets = 1536;
    static constexpr int kLasers = 256;
    static constexpr int kItems = 2096;
    static constexpr int kSlotEtama = 6;

    // Item types (spec 17.2).
    enum ItemType : int {
        SmallPower = 0, Point = 1, BigPower = 2, Bomb = 3, FullPower = 4, Extend = 5, PointBullet = 6, TimeOrb = 7,
        SmallFullPower = 8,
    };
    enum ItemState : int { Falling = 0, Homing = 1, Scatter = 2, OrbRising = 3, GrazeOrb = 5 };

    struct Bullet {
        AnmVm vm, spawnVm, cancelVm;
        float hitSize = 0;
        std::uint8_t bucket = 0;
        Vec3f pos, velocity;
        float speed = 0, angle = 0;
        Timer timerA, timerB;
        std::int32_t spawnDelay = 0;
        std::uint32_t active = 0, allowed = 0;
        std::int16_t color = 0, type = 0;
        std::uint8_t state = 0;  // 0 free, 1 normal, 2-4 spawning, 5 cancelling
        std::uint16_t offscreen = 0;
        std::uint8_t grazed = 0;
        bool pendingCancel = false, noCollision = false;
        std::int32_t tag = 0;  // bullet+0xDC4: the ECL border routines' cooldown (ex 4, 7, 21)
        int sound = -1;
        int cursor = 0;
        std::array<BulletCommand, 6> commands{};
        Timer burstTimer;
        Vec3f accel;
        Timer accelTimer;
        std::int32_t accelDuration = 0;
        float turnRate = 0, turnSpeed = 0;
        Timer turnTimer;
        std::int32_t turnDuration = 0;
        float aimAngle = 0, aimSpeed = 0;
        Timer aimTimer;
        std::int32_t aimInterval = 0, aimMax = 0, aimCount = 0;
        float bounceSpeed = 0;
        std::int32_t bounceMax = 0, bounceCount = 0;
    };
    struct Item {
        AnmVm vm;
        Vec3f pos, velocity, target;
        Timer timer;
        std::int8_t type = 0;
        bool inUse = false, onScreen = false;
        std::uint8_t state = 0;
        bool fullValue = false;
        int next = -1, prev = -1;  // the spawn-order list
    };

    BulletManager();
    ~BulletManager();

    // Stage start: the pools zeroed and the type templates rebuilt (etama.anm must be loaded).
    void stageStart(const World& w);
    void update(const World& w);
    void draw(SpriteSink& sink, const AnmDrawState& state);

    // ECL.
    void fire(const World& w, const Shooter& s);
    Laser* spawnLaser(const World& w, const Shooter& s, float startOffset, float endOffset, float startLength, float width,
                      std::int32_t startTime, std::int32_t duration, std::int32_t endTime, std::int32_t hitboxStart,
                      std::int32_t hitboxEnd, std::uint16_t flags, bool aimed);
    void stopLaser(Laser& l);
    // Clear-all with code q (spec 15.9): 1 = items homing, 4 = no items.
    void clearAll(const World& w, int q);
    std::uint32_t bonusClear(const World& w, std::uint32_t maxBonus);
    int liveBullets() const { return liveCount_; }
    Bullet& bullet(int i) { return bullets_[static_cast<std::size_t>(i)]; }
    Laser& laser(int i) { return lasers_[static_cast<std::size_t>(i)]; }
    int laserIndex(const Laser* l) const {
        return l >= lasers_.data() && l < lasers_.data() + kLasers ? static_cast<int>(l - lasers_.data()) : -1;
    }

    // Items (spec 17.3).
    Item& spawnItem(const World& w, const Vec3f& pos, int type, int state);
    void collectAll();
    void stopHoming();
    int liveItems() const;

  private:
    struct Template {
        AnmVm vm, fast, normal, slow, cancel;
        float hitSize = 0;
        std::uint8_t bucket = 0;
    };
    bool initBullet(const World& w, const Shooter& s, const Template& t, int j, int k, float aim);
    void runScheduler(const World& w, Bullet& b);
    void applyModifiers(const World& w, Bullet& b);
    bool inBounds(const Bullet& b) const;
    void cancelItems(const World& w, Bullet& b, int itemType);
    void updateItems(const World& w);
    void collect(const World& w, Item& it);
    void convertPowerItems(const World& w, const Item* except);
    void freeItem(int i);
    void updateLasers(const World& w);

    std::array<Template, 21> templates_{};
    std::vector<Bullet> bullets_;
    std::vector<Laser> lasers_;
    Laser dummyLaser_;
    std::vector<Item> items_;  // + the sink
    int itemHead_ = -1, itemTail_ = -1;
    int bulletCursor_ = 0, itemCursor_ = 0;
    int liveCount_ = 0;
    std::int32_t clearCountdown_ = 0;
    int cancelItemType_ = PointBullet;
    std::array<std::vector<std::int16_t>, 6> buckets_{};
    int itemSound_ = -1;
};

}  // namespace touhou::th08
