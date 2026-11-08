#pragma once

// Perfect Cherry Blossom's bullet manager: 1024 enemy bullets (patterns, the five-command
// lists and their modifiers, grazing, cancelling), 64 lasers, and the 1100 items it also
// updates (spawning, homing, pickup and every item's effect). Logic priority 12, draw
// order 10 (lasers, then items, then bullets).

#include <array>
#include <cstdint>
#include <memory>

#include "core/ZunMath.hpp"
#include "th07/Anm.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
}  // namespace touhou

namespace touhou::th07 {

// One bullet command (24 bytes in the ECL data).
struct BulletCommand {
    float a = 0, b = 0;
    std::int32_t duration = 0, loops = 0;
    std::uint32_t type = 0;
    std::int32_t immediate = 0;
};

// A pattern request (the "shooter" record).
struct Shooter {
    std::int16_t type = 0, color = 0;
    Vec3f pos;
    float angle1 = 0, angle2 = 0, speed1 = 0, speed2 = 0;
    std::int16_t count1 = 1, count2 = 1;
    int aimMode = 0;
    std::uint32_t flags = 0;
    int sound = 0;
    int bulletSound = -1;
    std::array<BulletCommand, 6> commands{};
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

class BulletManager final : public BulletField, public ItemField {
  public:
    static constexpr int kBullets = 1024;
    static constexpr int kLasers = 64;
    static constexpr int kItems = 1100;

    struct Bullet {
        AnmVm vm, spawnVm, cancelVm;
        float hitSize = 0;
        std::uint8_t bucket = 0, height = 0;
        Vec3f pos, velocity;
        float speed = 0, angle = 0;
        Timer timerA, timerB;
        std::int32_t spawnDelay = 0;
        std::uint32_t active = 0, allowed = 0;
        std::int16_t color = 0;
        std::uint8_t state = 0;  // 0 free, 1 normal, 2-4 spawning, 5 cancelling
        std::uint16_t offscreen = 0;
        std::uint8_t grazed = 0;
        std::int16_t tag = 0;
        int sound = -1;
        int cursor = 0;
        std::array<BulletCommand, 5> commands{};
        // Modifier records.
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
        std::uint8_t state = 0;  // 0 normal, 1 auto-collect, 2 scatter
        bool bonus = false;      // full value (collected while the border homed it in)
    };

    // Stage start: the pool zeroed and the type templates rebuilt (etama.anm must be loaded).
    void stageStart(const World& w);
    void update(const World& w);
    void draw(SpriteSink& sink, const AnmDrawState& state);

    // ECL.
    void fire(const World& w, const Shooter& s);
    Laser* spawnLaser(const World& w, const Shooter& s, float startOffset, float endOffset, float startLength, float width,
                      std::int32_t startTime, std::int32_t duration, std::int32_t endTime, std::int32_t hitboxStart,
                      std::int32_t hitboxEnd, std::uint16_t flags, bool aimed);
    void stopLaser(Laser& l);
    void clearAll(const World& w, int q);
    std::uint32_t bonusClear(const World& w, std::uint32_t maxBonus, bool laserItems);
    void radiusClear(const World& w, const Vec3f& centre, float radius);
    void setCancelItemType(int type) { cancelItemType_ = type; }
    int lastLiveCount() const { return liveCount_; }
    Bullet& bullet(int i) { return bullets_[static_cast<std::size_t>(i)]; }
    Laser& laser(int i) { return lasers_[static_cast<std::size_t>(i)]; }

    // BulletField (the player's view).
    void clearWithoutItems() override { clearAll(*world_, 0); }
    void turnIntoItems() override { clearAll(*world_, 1); }
    void freeze() override;
    int liveBullets() const override { return liveCount_; }
    // ItemField.
    void spawn(const Vec3f& pos, int type, int state) override;
    void collectAll() override;
    void removeItems();  // every item gone (dialogue)
    void stopHoming() override;

  private:
    struct Template {
        AnmVm vm, fast, normal, slow, cancel;
        float hitSize = 0;
        std::uint8_t bucket = 0, height = 0;
    };
    bool initBullet(const World& w, const Shooter& s, const Template& t, int j, int k, float aim);
    void runScheduler(const World& w, Bullet& b);
    void applyModifiers(const World& w, Bullet& b);
    bool inBounds(const Bullet& b) const;
    void cancelInto(const World& w, Bullet& b, int itemType);
    void updateItems(const World& w);
    void collect(const World& w, Item& it, int slot);
    void convertPowerItems(const World& w, const Item* except);
    void updateLasers(const World& w);
    void laserItems(const World& w, const Laser& l, int type, int state, bool atOrigin);
    void laserCollision(const World& w, Laser& l, const Vec3f& centre, const Vec3f& size, bool graze);

    const World* world_ = nullptr;
    std::array<Template, 11> templates_{};
    std::unique_ptr<std::array<Bullet, kBullets>> bullets_storage_ = std::make_unique<std::array<Bullet, kBullets>>();
    std::array<Bullet, kBullets>& bullets_ = *bullets_storage_;
    std::array<Laser, kLasers> lasers_{};
    Laser dummyLaser_;
    std::unique_ptr<std::array<Item, kItems + 1>> items_storage_ = std::make_unique<std::array<Item, kItems + 1>>();
    std::array<Item, kItems + 1>& items_ = *items_storage_;
    int bulletCursor_ = 0, itemCursor_ = 0;
    int liveCount_ = 0;
    std::int32_t clearCountdown_ = 0;
    int cancelItemType_ = PointBullet;
    std::array<std::array<std::int16_t, kBullets>, 6> buckets_{};
    std::array<int, 6> bucketCount_{};
    std::array<std::int16_t, kItems + 1> itemDraw_{};
    int itemDrawCount_ = 0;
};

}  // namespace touhou::th07
