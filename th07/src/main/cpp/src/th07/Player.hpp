#pragma once

// Perfect Cherry Blossom's player: movement, options, shots (from the .sht files), death and
// respawn with the deathbomb window, the twelve bombs, the Supernatural Border, and the
// collision tests the enemies and bullets make against the player. Runs in the logic pass
// at priority 8 and draws at orders 6 and 8.

#include <array>
#include <cstdint>
#include <string>

#include "core/ZunMath.hpp"
#include "th07/Anm.hpp"
#include "th07/ShotData.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
}  // namespace touhou

namespace touhou::th07 {

struct Effect;
struct World;

// The game's button word (shared with replays).
enum Button : std::uint16_t {
    BtnShoot = 0x0001, BtnBomb = 0x0002, BtnFocus = 0x0004, BtnMenu = 0x0008,
    BtnUp = 0x0010, BtnDown = 0x0020, BtnLeft = 0x0040, BtnRight = 0x0080,
    BtnSkip = 0x0100, BtnQ = 0x0200, BtnS = 0x0400, BtnHome = 0x0800, BtnEnter = 0x1000, BtnD = 0x2000, BtnR = 0x4000,
};

class Player {
  public:
    enum class State : std::uint8_t { Alive, Spawning, Dead, Invulnerable, Border };
    enum class BorderFlag : std::uint8_t { None, Active, Ready };
    enum class Options : std::uint8_t { Unfocused, Focusing, Focused, Unfocusing };
    enum class HitResult : std::uint8_t { None, Cancelled, Hit, Grazed };

    static constexpr int kShots = 96;
    static constexpr int kDamageRegions = 112;
    static constexpr int kCancelRegions = 96;
    static constexpr int kProjectiles = 96;

    struct Shot {
        std::uint8_t state = 0;  // 0 free, 1 live, 2 hit animation
        const ShotEntry* entry = nullptr;
        AnmVm vm;
        Vec3f pos, velocity;
        float boxW = 0, boxH = 0;
        float angle = 0, speed = 0;
        Timer age;
        std::int16_t damage = 0;
        std::int8_t behaviour = 0;
        std::uint32_t updateKind = 0, drawKind = 0, hitKind = 0;
        int laserSlot = -1, option = 0;
        float offsetX = 0, offsetY = 0;
        int trailLength = 0;
        std::array<Vec3f, 16> trail{};
    };
    struct DamageRegion {
        Vec3f centre;
        float w = 0, h = 0;
        std::int32_t damage = 0, dealt = 0;
    };
    struct CancelRegion {
        Vec3f centre;
        float w = 0, h = 0;
        float radius = 0, growth = 0;
        std::int32_t lifetime = 0;
        int itemType = 6;
    };
    struct LaserSlot {
        int shot = -1;  // the linked shot slot
        const ShotEntry* entry = nullptr;
        std::int32_t countdown = 0;
    };
    struct Projectile {  // bomb orbs, stars, knives, attractors
        bool active = false;
        std::uint8_t state = 0;
        Vec3f pos, velocity, accel;
        float angle = 0, speed = 0, accelScalar = 0, turn = 0;
        std::int32_t dealt = 0, counter = 0;
        Timer timer;
        std::array<AnmVm, 4> vms{};
        std::array<Vec3f, 32> trail{};
    };

    // Loads the shot files (every stage) and the character ANM (first stage of a run only).
    bool load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, int shotIndex, bool firstStage, std::string* error);
    void releaseAnm(AnmLibrary& anm);
    // The player rebuilt at a stage start (all state cleared; spawning, then invulnerable).
    void stageStart(const World& w);

    void update(const World& w, std::uint16_t input);
    void drawLow(SpriteSink& sink, const AnmDrawState& state);   // order 6: live shots
    void drawHigh(SpriteSink& sink, const AnmDrawState& state);  // order 8: player, options, hits, bombs

    // Tests the bullets, lasers and enemies make (spec: player 3.4).
    HitResult bulletHit(const World& w, const Vec3f& centre, const Vec3f& size);
    HitResult bulletGraze(const World& w, const Vec3f& centre, const Vec3f& size);
    // The laser's box in its own frame (origin, angle); grazing when allowed this frame.
    HitResult laserTest(const World& w, const Vec3f& origin, float angle, const Vec3f& boxCentre, const Vec3f& boxSize, bool grazeAllowed);
    int cancelItemType() const { return cancelItemType_; }
    // Graze scoring at a position (shared by bullets and lasers).
    void scoreGraze(const World& w, const Vec3f& at);
    // The damage the player deals to a box this frame (spec 5.6); `bomb` is the bomb flag.
    int damageQuery(const World& w, const Vec3f& centre, const Vec3f& size, bool& bomb);
    // Homing targets (set by the enemy pass, reset at the end of the player pass).
    void offerTarget(const Vec3f& enemy, bool boss);
    bool canPickUp() const { return state_ == State::Alive || state_ == State::Invulnerable || state_ == State::Border; }
    bool pickupBoxTouches(const Vec3f& item, float halfSize) const;

    void requestBorder(const World& w);  // cherry plus filled
    void breakBorder(const World& w);

    const Vec3f& position() const { return pos_; }
    // ECL variables 10021-10023 are writable: scripts can move the player.
    Vec3f& scriptPosition() { return pos_; }
    State state() const { return state_; }
    BorderFlag borderFlag() const { return border_; }
    bool focused() const { return focused_; }
    bool bombing() const { return bombing_; }  // the "bombing" marker (point-bullet value)
    bool dead() const { return state_ == State::Dead || state_ == State::Spawning; }
    bool continueMenuRequested() const { return continueMenu_; }
    void clearContinueRequest() { continueMenu_ = false; }
    const ShotType& shots() const { return shots_; }
    const ShotFile& header() const { return shots_.header(); }
    std::int32_t respawnCountdown() const { return respawn_; }
    const Vec3f& genericTarget() const { return target_; }

  private:
    // The frame's steps (spec 1.2).
    void regionUpkeep();
    void bombStep(const World& w, std::uint16_t input);
    bool deathStep(const World& w);  // true: moved to the respawn point
    void spawnInStep(const World& w);
    void stateTimerStep(const World& w);
    void inputStep(const World& w, std::uint16_t input);
    void updateOptions(const World& w);
    void shotUpdate(const World& w);
    void fireCycle(const World& w);
    void spawnShots(const World& w, int cycle);
    bool fire(const World& w, Shot& s, int slot, const ShotEntry& e, int cycle);
    void commonShotInit(const World& w, Shot& s, const ShotEntry& e);
    bool runShotUpdate(const World& w, Shot& s, int slot);  // false: remove
    void die(const World& w);
    void commitDeath(const World& w);
    void activateBorder(const World& w);
    void endBorderNaturally(const World& w);
    void startBomb(const World& w);
    void bombFrame(const World& w);
    void bombStartUp(const World& w, int invulnerability, int minCost, float scale);
    void endBomb(const World& w);
    void bombReimuA(const World& w, bool startUp);
    void bombReimuAFocus(const World& w, bool startUp);
    void bombReimuB(const World& w, bool startUp);
    void bombReimuBFocus(const World& w, bool startUp);
    void bombMarisaA(const World& w, bool startUp);
    void bombMarisaAFocus(const World& w, bool startUp);
    void bombMarisaB(const World& w, bool startUp);
    void bombMarisaBFocus(const World& w, bool startUp);
    void bombSakuyaA(const World& w, bool startUp);
    void bombSakuyaAFocus(const World& w, bool startUp);
    void bombSakuyaB(const World& w, bool startUp);
    void bombSakuyaBFocus(const World& w, bool startUp);
    void reimuOrbExplode(const World& w, int i, float radius, float growth, std::int32_t lifetime);
    void launchReimuOrb(const World& w, int i, float speed, float angle);
    void setRegion(int i, const Vec3f& centre, float w, float h, std::int32_t damage);
    void cancelCircle(const Vec3f& centre, float radius, float growth, std::int32_t lifetime, int itemType = 6);
    int cancelRect(const Vec3f& centre, float w, float h, int itemType = 6);
    int freeCancelSlot() const;
    bool cancelTest(const Vec3f& centre, const Vec3f& size);
    void startScript(const World& w, AnmVm& vm, int local);
    void spawnFocusEffect(const World& w);
    void interruptFocusEffect();

    ShotType shots_;
    int shotIndex_ = 0;
    bool anmLoaded_ = false;

    State state_ = State::Spawning;
    BorderFlag border_ = BorderFlag::None;
    Timer stateTimer_;
    std::int32_t respawn_ = 0;
    std::int32_t borderLength_ = 540;
    std::int32_t borderLockout_ = 0;
    std::int32_t bulletClear_ = 0;
    bool continueMenu_ = false;

    Vec3f pos_{192, 384, 0.49f};
    Vec3f hitMin_, hitMax_, grazeMin_, grazeMax_, pickMin_, pickMax_;
    float vxRaw_ = 0, vyRaw_ = 0, prevVxRaw_ = 0;
    float vx_ = 0, vy_ = 0;  // after multipliers
    float moveMulX_ = 1, moveMulY_ = 1;
    bool focused_ = false;
    AnmVm vm_;
    std::array<AnmVm, 2> optionVms_{};
    std::array<Vec3f, 2> optionPos_{};
    Options options_ = Options::Unfocused;
    std::int32_t optionCounter_ = 0;
    float optionAngle_ = -kZunPi / 2.0f;
    Effect* focusEffect_ = nullptr;
    Effect* bombRing_ = nullptr;
    Effect* borderEffect_ = nullptr;

    std::array<Shot, kShots> shotSlots_{};
    std::array<LaserSlot, 3> lasers_{};
    Timer fireCycle_;  // current -1: idle
    bool firing_ = false;
    std::array<DamageRegion, kDamageRegions> regions_{};
    std::array<CancelRegion, kCancelRegions> cancels_{};
    int cancelItemType_ = 6;
    std::int32_t sparkCounter_ = 0;

    Vec3f target_{-999, -999, 0}, sakuyaTarget_{-999, -999, 0};
    bool targetLocked_ = false;

    // The bomb.
    bool bombActive_ = false;
    bool bombing_ = false;
    bool bombFocus_ = false;
    Timer bombTimer_;
    std::int32_t bombDuration_ = 0;
    std::int32_t drain_ = 0;
    Vec3f bombStart_;
    std::array<Projectile, kProjectiles> proj_{};
    std::array<AnmVm, 4> bombVms_{};
    std::array<Vec3f, 4> bombAnchor_{};
    std::array<float, 3> beamAngle_{};
    int projDrawCount_ = 0;
    Effect* knifeEffect_ = nullptr;
};

}  // namespace touhou::th07
