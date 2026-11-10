#pragma once

// Imperishable Night's player (claude/th08-specs/spec_player_bullets_items.md §1-§8 and
// spec_bombs_familiars.md): movement with TH08's direction priorities and the focused file's
// speeds, the human/youkai focus switch (the team's sprite, familiars and the 7-frame
// "youkai in front" lag), the eight characters' familiars, the shots of the .sht files (128
// slots, the 20-frame fire cycle, fire kinds 0-8, homing, rising missiles, the doll laser),
// the stage-start spawn-in and invulnerability, and the damage the shots deal. Death, bombs,
// Last Spells, the damage and cancel regions and the gauge come next. Calc priority 9, draw
// priorities 9-10.

#include <array>
#include <cstdint>
#include <string>

#include "core/ZunMath.hpp"
#include "th08/Anm.hpp"
#include "th08/ShotData.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th08 {

struct Effect;
struct World;

class Player {
  public:
    static constexpr int kSlotPlayer = 5;  // player0T.anm
    static constexpr int kShots = 128;
    static constexpr int kFamiliars = 4;
    static constexpr int kHistory = 16;

    enum class State : std::uint8_t { Alive, Spawning, Dead, Invulnerable };
    // The movement direction code (Player+0xE2A98), chosen by the first matching test.
    enum class Direction : std::uint8_t { None, Up, Down, Left, Right, UpLeft, UpRight, DownLeft, DownRight };
    enum class FamiliarKind : std::uint8_t { None, Yukari, AliceDoll, RemiliaTeam, RemiliaSolo, PhantomTeam, PhantomSolo, Yuyuko };
    enum class HitResult : std::uint8_t { None, Cancelled, Hit, Grazed };
    static constexpr int kRegions = 384;  // 0-191 damage, 192-383 cancel (spec 10.1)

    struct Region {
        Vec3f centre;
        float radius = 0, growth = 0;
        float w = 0, h = 0, wGrowth = 0, hGrowth = 0;
        float angle = 0;
        std::int32_t lifetime = 0;
        std::int32_t itemType = 6;
        std::int32_t damage = 0, dealt = 0, cap = 0, interval = 1;
        bool active = false, noSpark = false;
    };

    struct Familiar {
        AnmVm vm;
        Vec3f pos, anchor, velocity;
        std::uint8_t state = 0;  // 0 none, 1 just created, 2 normal, 3 dismissing
        std::int32_t mode = 0;
        int index = 0;
        float orbit = 0, facing = 0;
        Timer timer;
        FamiliarKind kind = FamiliarKind::None;
    };
    struct Shot {
        std::uint8_t state = 0;  // 0 free, 1 flying, 2 hit animation
        const ShotEntry* entry = nullptr;
        AnmVm vm;
        Vec3f pos, velocity;
        float boxW = 0, boxH = 0;
        float angle = 0, speed = 0;
        Timer age;
        std::int16_t damage = 0;
        std::int16_t shotClass = 0, sprite = 0, side = 0;
        std::uint8_t focus = 0;
        bool youkaiPower = false;
        std::uint32_t updateKind = 0, drawKind = 0, hitKind = 0;
        bool updateActive = true;  // the doll laser clears its own update callback
        int laserSlot = -1, origin = 0, segments = 0;
        float offsetX = 0, offsetY = 0;
        std::array<Vec3f, 32> trail{};
    };
    struct LaserSlot {
        int shot = -1;  // the owning shot slot
        const ShotEntry* entry = nullptr;
        std::int32_t timer = 0;
    };

    // Loads the shot files (every stage) and the character's ANM (the first stage of a game).
    bool load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, int character, bool firstStage, std::string* error);
    void releaseAnm(AnmLibrary& anm);
    // The player at a stage start (spec 8.7): everything cleared, spawning for 120 frames
    // (10 in spell practice), the solos' familiars built.
    void stageStart(const World& w);

    // The player pass (spec 1.2) with the game input word.
    void update(const World& w, std::uint16_t input);
    void drawShots(SpriteSink& sink, const AnmDrawState& state);   // draw priority 9
    void drawPlayer(SpriteSink& sink, const AnmDrawState& state);  // draw priority 10

    // The damage the shots deal to an enemy box this frame (spec 6.5, shots part); `bomb` is
    // set when a bomb region hit.
    int damageQuery(const World& w, const Vec3f& centre, const Vec3f& size, std::int32_t& accumulator, bool& bomb);
    // Homing targets, written by the enemy pass (spec_ecl 8.5): the generic target and its
    // lock (reset at the end of the player pass) and the target enemy's slot (-1: none; not
    // reset there - Yukari's familiar clears it).
    Vec3f& homingTarget() { return target_; }
    bool& targetLock() { return targetLock_; }
    int& targetEnemySlot() { return targetEnemy_; }

    // The tests the bullets, lasers and enemies make against the player (spec 3.7).
    HitResult bulletHit(const World& w, const Vec3f& centre, const Vec3f& size);
    HitResult bulletGraze(const World& w, const Vec3f& centre, const Vec3f& size, bool laser);
    int laserTest(const World& w, const Vec3f& origin, float angle, const Vec3f& boxCentre, const Vec3f& boxSize, bool grazeAllowed);
    HitResult enemyContact(const World& w, const Vec3f& centre, const Vec3f& box);
    // The cancel-region test on a bullet centre (sets the cancel item type on a hit).
    bool cancelTest(const Vec3f& centre);
    int cancelItemType() const { return cancelItemType_; }
    void scoreGraze(const World& w, const Vec3f& at, bool laser);
    bool canPickUp() const { return state_ == State::Alive || state_ == State::Invulnerable; }
    bool pickupBoxTouches(const Vec3f& item) const;
    // Regions (spec 10.2).
    void cancelRect(const Vec3f& centre, float w, float h, int itemType, std::int32_t lifetime);
    void cancelCircle(const Vec3f& centre, float r, float growth, std::int32_t lifetime, int itemType);
    void makeInvulnerable(std::int32_t frames);
    bool continueRequested() const { return continueRequested_; }

    const Vec3f& position() const { return pos_; }
    Vec3f& scriptPosition() { return pos_; }  // ECL can move the player
    State state() const { return state_; }
    bool dead() const { return state_ == State::Dead || state_ == State::Spawning; }
    bool focused() const { return focus_ == 1; }
    bool youkaiInFront() const { return youkaiFront_; }
    Direction direction() const { return direction_; }
    int character() const { return character_; }
    bool bombActive() const { return bombActive_; }
    const ShotType& shots() const { return shots_; }
    const ShotFile& header() const { return shots_.unfocused; }
    const Familiar& familiar(int k) const { return familiars_[static_cast<std::size_t>(k)]; }
    const Shot& shot(int i) const { return shotSlots_[static_cast<std::size_t>(i)]; }
    int liveShots() const;
    bool firing() const { return fireCycle_.current >= 0; }
    std::int32_t stateTimer() const { return stateTimer_.current; }
    std::int32_t deathbombCountdown() const { return countdown_; }
    const AnmVm& sprite() const { return vm_; }

  private:
    void regionUpkeep();
    void die(const World& w);
    bool deathStep(const World& w);  // true: moved to the respawn point
    void commitDeath(const World& w);
    int freeRegion(int first) const;
    void spawnInStep(const World& w);
    void stateStep(const World& w);
    void movementStep(const World& w, std::uint16_t input);
    void focusBookkeeping(const World& w);
    void setSpriteScript(const World& w, int script);
    void buildFamiliars(const World& w, bool youkaiTable);
    void createFamiliar(int k, FamiliarKind kind);
    void dismissFamiliar(int k);
    void updateFamiliar(const World& w, Familiar& f, const Vec3f& oldest);
    void facingUpdate(Familiar& f);
    void yukariSpring(const World& w, Familiar& f, const Vec3f& target);
    void trailEffect(const World& w, const Familiar& f, std::uint32_t color);
    void fireCycle(const World& w, std::uint16_t input);
    void spawnShots(const World& w, int cycle);
    bool fire(const World& w, Shot& s, int slot, const ShotEntry& e, int cycle);
    void shotInit(const World& w, Shot& s, const ShotEntry& e);
    bool runShotUpdate(const World& w, Shot& s, int slot);  // false: remove
    void shotUpdate(const World& w);
    Vec3f originOf(int origin) const;

    ShotType shots_;
    int character_ = 0;
    bool anmLoaded_ = false;

    State state_ = State::Spawning;
    Timer stateTimer_;
    std::int32_t countdown_ = 0;     // the deathbomb countdown (Player+0xE2A68)
    std::int32_t bulletClear_ = 0;   // Player+0xE2A70

    Vec3f pos_{192, 384, 0.49f};
    Vec3f hitMin_, hitMax_, grazeMin_, grazeMax_, itemMin_, itemMax_;
    float vx_ = 0, vy_ = 0;          // this frame's speed (after the bomb multipliers)
    float prevVx_ = 0;               // the lean animation's previous speed
    float moveMulX_ = 1, moveMulY_ = 1;
    std::uint8_t focus_ = 2;         // byte+3: 0 unfocused, 1 focused, 2 before the first input step
    bool youkaiFront_ = false;       // byte+5
    bool focusInput_ = false, shootHeld_ = false;  // this pass's Focus and Shot buttons
    std::int32_t switchCounter_ = 0; // Player+0x8: frames since the last focus switch
    Direction direction_ = Direction::None;
    AnmVm vm_;
    Effect* focusEffect_ = nullptr;
    std::array<Familiar, kFamiliars> familiars_{};
    std::array<Vec3f, kHistory> history_{};

    std::array<Shot, kShots> shotSlots_{};
    std::array<LaserSlot, 3> lasers_{};
    Timer fireCycle_;  // current < 0: idle

    Vec3f target_{-999, -999, 0};
    bool targetLock_ = false;
    int targetEnemy_ = -1;
    std::uint8_t sparkCounter_ = 0;  // Player+0xE2A94 (shared with the region sparks)
    std::array<Region, kRegions> regions_{};
    int cancelItemType_ = 6;
    bool lastSpellArmed_ = false;     // byte+4
    bool continueRequested_ = false;  // no spare life at the respawn move
    bool committed_ = false;

    // The bomb (spec 9): not ported yet; the shots and familiars read these.
    bool bombActive_ = false;
    int bombIndex_ = 0;
    Timer bombTimer_;
};

}  // namespace touhou::th08
