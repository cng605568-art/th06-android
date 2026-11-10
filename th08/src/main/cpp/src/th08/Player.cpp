#include "th08/Player.hpp"

#include <algorithm>
#include <cmath>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bullets.hpp"
#include "th08/Effects.hpp"
#include "th08/Enemies.hpp"
#include "th08/Gui.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {

constexpr Vec3f kNoTarget{-999.0f, -999.0f, 0.0f};

bool ticked(const Timer& t) { return g_timing.speed > 0.99f || t.current != t.previous; }

// Inclusive axis-aligned overlap of centre +- half extents.
bool boxesTouch(const Vec3f& c1, float hw1, float hh1, const Vec3f& c2, float hw2, float hh2) {
    return !(c1.x + hw1 < c2.x - hw2 || c1.x - hw1 > c2.x + hw2 || c1.y + hh1 < c2.y - hh2 || c1.y - hh1 > c2.y + hh2);
}

// A polar vector with one rounding: the sine and cosine are not narrowed before the product
// with the length (spec_player B7 §1.2.2).
Vec3f polarOnce(float angle, float length) {
    return {static_cast<float>(std::cos(static_cast<double>(angle)) * static_cast<double>(length)),
            static_cast<float>(std::sin(static_cast<double>(angle)) * static_cast<double>(length)), 0.0f};
}

std::uint32_t withAlpha(std::uint32_t argb, std::int32_t a) {
    return (argb & 0x00FFFFFFu) | (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24);
}

// randSigned: u32 / 2^31 - 1 (one u32 draw, two steps).
float randSigned(ZunRng& rng) { return static_cast<float>(rng.u32()) / 2147483648.0f - 1.0f; }

// Remilia's familiars (spec_bombs 7.1): offsets, initial orbit angles, orbit rates.
constexpr Vec3f kRemiliaOffset[4] = {{-30, -16, 0}, {-10, -32, 0}, {10, -32, 0}, {30, -16, 0}};
constexpr float kRemiliaOrbit[4] = {0.0f, kZunPi, 0.0f, kZunPi};
constexpr float kRemiliaRate[4] = {kZunPi / 120.0f, -kZunPi / 90.0f, kZunPi / 90.0f, -kZunPi / 120.0f};

}  // namespace

// ---- loading and stage start ------------------------------------------------------------

bool Player::load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, int character, bool firstStage, std::string* error) {
    character_ = std::clamp(character, 0, 11);
    for (int focused = 0; focused < 2; ++focused) {
        const std::string name = shotFileName(character_, focused != 0);
        const auto bytes = vfs.read(name);
        if (!bytes) {
            if (error) *error = name + " is not in the archive";
            return false;
        }
        std::string why;
        if (!(focused ? shots_.focused : shots_.unfocused).parse(*bytes, &why)) {
            if (error) *error = name + ": " + why;
            return false;
        }
    }
    if (firstStage || !anmLoaded_) {
        if (!anm.load(vfs, textures, playerAnmName(character_), kSlotPlayer, error)) return false;
        anmLoaded_ = true;
    }
    return true;
}

void Player::releaseAnm(AnmLibrary& anm) {
    if (anmLoaded_) anm.release(kSlotPlayer);
    anmLoaded_ = false;
}

void Player::setSpriteScript(const World& w, int script) {
    if (w.anm && w.rng) vm_.start(*w.anm, *w.rng, kSlotPlayer, script);
}

void Player::stageStart(const World& w) {
    // The whole player is zero-filled at registration; the shot files survive.
    const ShotType keep = shots_;
    const int character = character_;
    const bool anm = anmLoaded_;
    *this = Player{};
    shots_ = keep;
    character_ = character;
    anmLoaded_ = anm;
    pos_ = {192.0f, 384.0f, 0.49f};
    state_ = State::Spawning;
    stateTimer_.assign((w.mode & ModeSpellPractice) ? 10 : 120);
    countdown_ = header().deathbombCap;
    focus_ = 2;
    fireCycle_.assign(-1);
    moveMulX_ = moveMulY_ = 1.0f;
    vm_ = AnmVm{};
    setSpriteScript(w, isYoukaiSolo(character_) ? 5 : 0);
    history_.fill(pos_);
    // Boxes from the unfocused header (they are recomputed by the movement step).
    const float hb = header().hitbox / 2.0f, gb = header().grazeBox / 2.0f, ib = header().itemBox / 2.0f;
    hitMin_ = {pos_.x - hb, pos_.y - hb, 0}, hitMax_ = {pos_.x + hb, pos_.y + hb, 0};
    grazeMin_ = {pos_.x - gb, pos_.y - gb, 0}, grazeMax_ = {pos_.x + gb, pos_.y + gb, 0};
    itemMin_ = {pos_.x - ib, pos_.y - ib, 0}, itemMax_ = {pos_.x + ib, pos_.y + ib, 0};
    if (isSolo(character_)) buildFamiliars(w, false);
    // The gauge limits and thresholds (spec 11.1) and the time-orb damage step (spec 8.7).
    GameState& g = *w.state;
    struct Limits {
        std::int16_t min, max, h80, y80, h20, y20;
    };
    Limits l{-10000, 10000, -8000, 8000, -2000, 2000};
    if (character_ == 3) l = {-5000, 10000, -3000, 8000, -2000, 2000};
    else if (character_ == 10) l = {-5000, 5000, -3000, 3000, -2000, 2000};
    else if (isHumanSolo(character_)) l = {-10000, 2000, -8000, 8000, -2000, 2001};
    else if (isYoukaiSolo(character_)) l = {-2000, 10000, -8000, 8000, -2001, 2000};
    g.gaugeMin = l.min, g.gaugeMax = l.max, g.human80 = l.h80, g.youkai80 = l.y80, g.human20 = l.h20, g.youkai20 = l.y20;
    orbStep_ = isHumanSolo(character_) ? 27 : 40;
}

// ---- familiars --------------------------------------------------------------------------

void Player::createFamiliar(int k, FamiliarKind kind) {
    Familiar& f = familiars_[static_cast<std::size_t>(k)];
    f = Familiar{};
    f.kind = kind;
    if (kind != FamiliarKind::None) {
        f.state = 1;
        f.timer.reset();
        f.index = k;
    }
}

void Player::dismissFamiliar(int k) {
    Familiar& f = familiars_[static_cast<std::size_t>(k)];
    if (f.state != 0 && f.state != 3) {
        f.state = 3;
        f.timer.reset();
    }
}

// The four records from the tables (spec_bombs 1.2): a team's youkai familiars on a switch to
// focused, a solo's own at stage start.
void Player::buildFamiliars(const World& w, bool youkaiTable) {
    (void)w, (void)youkaiTable;
    std::array<FamiliarKind, 4> kinds{};
    switch (character_) {
        case 0: case 5: kinds[0] = FamiliarKind::Yukari; break;
        case 1: case 7: kinds[0] = FamiliarKind::AliceDoll; break;
        case 2: kinds.fill(FamiliarKind::RemiliaTeam); break;
        case 9: kinds.fill(FamiliarKind::RemiliaSolo); break;
        case 3: case 11: kinds[0] = kinds[1] = FamiliarKind::Yuyuko; break;
        case 10: kinds[2] = FamiliarKind::PhantomSolo; break;
        default: break;
    }
    for (int k = 0; k < kFamiliars; ++k) createFamiliar(k, kinds[static_cast<std::size_t>(k)]);
}

void Player::trailEffect(const World& w, const Familiar& f, std::uint32_t color) {
    if (w.effects) w.effects->spawn(w, 47, f.pos, 1, color);  // its script draws 8 RNG steps
}

void Player::facingUpdate(Familiar& f) {
    float target;
    switch (direction_) {
        case Direction::Up: target = kZunPi / 2.0f; break;
        case Direction::Down: target = -kZunPi / 2.0f; break;
        case Direction::Left: target = 0.0f; break;
        case Direction::Right: target = kZunPi; break;
        case Direction::UpLeft: target = kZunPi / 4.0f; break;
        case Direction::UpRight: target = 3.0f * kZunPi / 4.0f; break;
        case Direction::DownLeft: target = -kZunPi / 4.0f; break;
        case Direction::DownRight: target = -3.0f * kZunPi / 4.0f; break;
        default: return;
    }
    float d = std::fabs(f.facing - target);
    if (d > kZunPi) {
        target += f.facing > target ? kZunTwoPi : -kZunTwoPi;
        d = std::fabs(f.facing - target);
    }
    if (d > kZunPi / 2.0f) f.facing = target;
    else f.facing = addNormalizeAngle(f.facing, (target - f.facing) * 0.07f);
}

// Yukari's spring (0x44E770 / 0x44E8D0): towards `target`, velocity eased by 0.2, small
// horizontal speeds snapped to 0.
void Player::yukariSpring(const World& w, Familiar& f, const Vec3f& target) {
    (void)w;
    const Vec3f a = (target - f.pos) * (1.0f / 16.0f);
    f.velocity = f.velocity + (a - f.velocity) * 0.2f;
    f.pos = f.pos + f.velocity;
    if (std::fabs(f.velocity.x) < 0.05f) f.velocity.x = 0.0f;
}

void Player::updateFamiliar(const World& w, Familiar& f, const Vec3f& oldest) {
    AnmLibrary& lib = *w.anm;
    ZunRng& rng = *w.rng;
    const bool focusedNow = focus_ == 1;
    // Dismissing (all kinds): fade out, then gone after the 18th update.
    if (f.state == 3) {
        if (f.kind == FamiliarKind::AliceDoll) f.pos = pos_ - Vec3f{0.0f, 32.0f, 0.0f};
        if (f.timer.current == 0) f.vm.interrupt(5);
        if (f.timer.current > 16) {
            f.state = 0;
            f.kind = FamiliarKind::None;
        }
        return;
    }
    switch (f.kind) {
        case FamiliarKind::Yukari: {
            const Vec3f home{pos_.x, std::max(pos_.y - 96.0f, 32.0f), pos_.z};
            if (f.state == 1) {
                f.vm.start(lib, rng, kSlotPlayer, 18);
                f.pos = home;
                f.state = 2;
                targetEnemy_ = -1;
                return;
            }
            auto spring = [&]() {
                yukariSpring(w, f, home);
                if (fireCycle_.current >= 0 && targetEnemy_ >= 0 && f.timer.current >= 10) {
                    f.vm.interrupt(3);
                    f.mode = 3;
                } else {
                    targetEnemy_ = -1;  // Yukari's familiar is what clears the pointer [QUIRK]
                }
            };
            auto flip = [&](bool left) { f.vm.scale.x = left ? std::fabs(f.vm.scale.x) : -std::fabs(f.vm.scale.x); };
            switch (f.mode) {
                case 0:
                    spring();
                    if (f.velocity.x < 0.0f) {
                        f.vm.interrupt(2), f.mode = 1, flip(true);
                    } else if (f.velocity.x > 0.0f) {
                        f.vm.interrupt(2), f.mode = 2, flip(false);
                    }
                    break;
                case 1:
                    spring();
                    if (f.velocity.x == 0.0f) {
                        f.vm.interrupt(1), f.mode = 0, flip(true);
                    } else if (f.velocity.x > 0.0f) {
                        f.vm.interrupt(2), f.mode = 2, flip(false);
                    }
                    break;
                case 2:
                    spring();
                    if (f.velocity.x == 0.0f) {
                        f.vm.interrupt(1), f.mode = 0, flip(true);
                    } else if (f.velocity.x < 0.0f) {
                        f.vm.interrupt(2), f.mode = 1, flip(true);
                    }
                    break;
                case 3:
                    if (targetEnemy_ >= 0 && w.enemies) {
                        const Vec3f a = w.enemies->enemy(targetEnemy_).abs;
                        yukariSpring(w, f, {a.x, std::max(a.y + 32.0f, 32.0f), a.z});
                    }
                    if ((fireCycle_.current < 0 && !shootHeld_) || targetEnemy_ < 0) {
                        targetEnemy_ = -1;
                        f.vm.interrupt(1);
                        f.mode = 0;
                    }
                    break;
                default: break;
            }
            break;
        }
        case FamiliarKind::AliceDoll:
            if (f.state == 1) {
                f.vm.start(lib, rng, kSlotPlayer, 29);
                f.state = 2;
            }
            if (!bombActive_) f.pos = pos_ - Vec3f{0.0f, 32.0f, 0.0f};
            break;
        case FamiliarKind::RemiliaTeam:
        case FamiliarKind::RemiliaSolo: {
            const auto k = static_cast<std::size_t>(f.index & 3);
            if (f.state == 1) {
                f.vm.start(lib, rng, kSlotPlayer, 24);
                f.state = 2;
                f.anchor = pos_ + kRemiliaOffset[k];
                f.orbit = kRemiliaOrbit[k];
                if (f.kind == FamiliarKind::RemiliaTeam) {
                    f.pos = f.anchor + polarOnce(f.orbit, 8.0f);
                    break;
                }
            }
            if (f.timer.current > 12) f.orbit = addNormalizeAngle(f.orbit, kRemiliaRate[k]);
            if (f.kind == FamiliarKind::RemiliaTeam) {
                f.pos = f.anchor + polarOnce(f.orbit, 8.0f);  // the anchor never moves [QUIRK]
                break;
            }
            f.vm.color = 0xFFFF8080u;
            const Vec3f offset = polarOnce(f.orbit, 8.0f);
            if (!focusedNow) {
                f.vm.color = 0xFF80FFFFu;
                f.anchor = ((pos_ + kRemiliaOffset[k]) - f.anchor) * 0.2f + f.anchor;
            }
            f.pos = offset + f.anchor;
            f.pos.z = 0.0f;
            trailEffect(w, f, 0x80602050u);
            break;
        }
        case FamiliarKind::PhantomTeam:
        case FamiliarKind::PhantomSolo: {
            if (f.state == 1) {
                f.vm.start(lib, rng, kSlotPlayer, 21);
                f.state = 2;
                f.anchor = oldest;
                f.orbit = 0.0f;
                f.facing = -kZunPi / 2.0f;
            }
            f.orbit = addNormalizeAngle(f.orbit, kZunPi / 60.0f);
            const Vec3f offset = polarOnce(f.orbit, 8.0f);
            f.anchor = (oldest - f.anchor) * 0.05f + f.anchor;
            f.pos = offset + f.anchor;
            f.pos.z = 0.0f;
            if (f.kind == FamiliarKind::PhantomTeam) {
                trailEffect(w, f, 0x80405080u);
                facingUpdate(f);
                break;
            }
            f.vm.color = 0xFFFF8080u;
            if (focusedNow) {
                trailEffect(w, f, 0xFFF05080u);
            } else {
                f.vm.color = 0xFFFFFFFFu;
                facingUpdate(f);
                if (direction_ == Direction::None) break;  // no trail while standing still [QUIRK]
                trailEffect(w, f, 0x80405080u);
            }
            break;
        }
        case FamiliarKind::Yuyuko: {
            const bool left = f.index == 0;
            if (f.state == 1) {
                f.vm.start(lib, rng, kSlotPlayer, 21);
                f.state = 2;
                f.anchor = pos_;
                f.orbit = left ? 0.0f : -kZunPi;
            }
            const Vec3f target = pos_ + Vec3f{left ? -32.0f : 32.0f, 0.0f, 0.0f};
            f.orbit = addNormalizeAngle(f.orbit, left ? kZunPi / 60.0f : -kZunPi / 60.0f);
            const Vec3f offset = polarOnce(f.orbit, 6.0f);
            f.anchor = (target - f.anchor) * 0.09f + f.anchor;
            f.pos = offset + f.anchor;
            f.pos.z = 0.0f;
            trailEffect(w, f, 0x80602050u);
            break;
        }
        default: break;
    }
}

// ---- the pass ---------------------------------------------------------------------------

void Player::update(const World& w, std::uint16_t input) {
    if (!w.anm || !w.rng || !w.state) return;
    shootHeld_ = (input & BtnShoot) != 0;
    // 0. Time stop: the focus effect hidden, nothing else runs.
    if (w.timeStop) {
        if (focusEffect_) focusEffect_->vm.set(AnmVm::Frozen, true);
        return;
    }
    if (focusEffect_) focusEffect_->vm.set(AnmVm::Frozen, false);
    // 1. Regions; 2. the bomb.
    regionUpkeep();
    bombStep(w, input);
    // 3. Death; 4. spawn-in (also in the frame of the respawn move).
    bool spawnIn = state_ == State::Spawning;
    if (state_ == State::Dead && deathStep(w)) spawnIn = true;
    if (spawnIn && !continueRequested_) spawnInStep(w);
    // 5. The state step.
    stateStep(w);
    // 6. Movement, focus, familiars, the shot trigger.
    if (!dead()) movementStep(w, input);
    // 7. The player sprite.
    vm_.tick(*w.rng);
    // 8-9. Shots and the fire cycle.
    shotUpdate(w);
    fireCycle(w, input);
    // 10. End of pass: the generic homing target and its lock are reset (the enemy pointer is not).
    target_ = kNoTarget;
    targetLock_ = false;
    // 11. Gauge statistics: frames spent at either 80 %, worth 100 points each.
    if (!w.dialogue) {
        GameState& g = *w.state;
        ++g.stageFrames;
        ++g.gameFrames;
        if (g.h80()) {
            ++g.stageHuman80;
            ++g.gameHuman80;
            g.score += 10;
        } else if (g.y80()) {
            ++g.stageYoukai80;
            ++g.gameYoukai80;
            g.score += 10;
        }
    }
    prevInput_ = input;
}

void Player::spawnInStep(const World& w) {
    bulletClear_ = 60;
    const float t = static_cast<float>(stateTimer_.current);
    const float s = 1.0f - t / 30.0f;
    vm_.scale = {1.0f - s, 2.0f * s + 1.0f};
    vm_.set(AnmVm::ScaleDirty, true);
    vm_.setBlend(1);
    vm_.color = withAlpha(0xFFFFFFFFu, stateTimer_.current * 255 / 30);
    moveMulX_ = moveMulY_ = 1.0f;
    countdown_ = 0;
    if (stateTimer_.current >= 30) {
        state_ = State::Invulnerable;
        vm_.scale = {1.0f, 1.0f};
        vm_.color = 0xFFFFFFFFu;
        vm_.setBlend(0);
        if (!(w.mode & ModeSpellPractice)) stateTimer_.assign(240);  // spell practice keeps 30
        countdown_ = header().deathbombCap;
    }
}

void Player::stateStep(const World& w) {
    (void)w;
    if (bulletClear_ != 0) {
        --bulletClear_;
        cancelRect(pos_, 768.0f, 896.0f, -1, 0);  // one frame, no item
    }
    if (state_ == State::Invulnerable) {
        lastSpellArmed_ = false;
        if (bombEffect_) bombEffect_->pos = pos_;
        stateTimer_.decrement(1);
        if (stateTimer_.current <= 0) {
            if (bombEffect_) bombEffect_->inUse = false;
            bombEffect_ = nullptr;
            state_ = State::Alive;
            stateTimer_.assign(0);
            vm_.color = 0xFFFFFFFFu;
        } else {
            vm_.color = (stateTimer_.current % 8) < 2 ? 0xFFF02020u : 0xFFFFFFFFu;
        }
        return;
    }
    stateTimer_.tick();
}

void Player::focusBookkeeping(const World& w) {
    const bool focus = bombActive_ ? (bombIndex_ & 1) != 0 : focusInput_;
    const bool team = !isSolo(character_);
    if (focus) {
        if (focus_ != 1) {
            if (team) {
                buildFamiliars(w, true);
                setSpriteScript(w, 5);
                prevVx_ = 0.0f;
                if (switchCounter_ >= 4 && w.effects) w.effects->spawn(w, 29, pos_, 1, 0x80FF8080u);
            }
            if (!focusEffect_ && w.effects) focusEffect_ = &w.effects->spawnSpecial(w, 22, pos_, 2, 1, 0xFFFFFFFFu);
            switchCounter_ = 0;
            gaugeRamp_.assign(0);
        } else {
            ++switchCounter_;
        }
    } else {
        if (focus_ != 0) {
            if (team) {
                if (character_ == 3) {
                    dismissFamiliar(0);
                    dismissFamiliar(1);
                    createFamiliar(2, FamiliarKind::PhantomTeam);  // Youmu's phantom
                    history_.fill(pos_);
                } else {
                    for (int k = 0; k < kFamiliars; ++k) dismissFamiliar(k);
                }
                setSpriteScript(w, 0);
                prevVx_ = 0.0f;
                if (switchCounter_ >= 4 && w.effects) w.effects->spawn(w, 28, pos_, 1, 0x808080FFu);
            }
            if (focusEffect_) focusEffect_->inUse = false;
            focusEffect_ = nullptr;
            switchCounter_ = 0;
            gaugeRamp_.assign(0);
        } else {
            ++switchCounter_;
        }
    }
    if (switchCounter_ >= 7) youkaiFront_ = focus;
    focus_ = focus ? 1 : 0;
    if (isYoukaiSolo(character_)) youkaiFront_ = true;
    else if (isHumanSolo(character_)) youkaiFront_ = false;
}

void Player::movementStep(const World& w, std::uint16_t input) {
    focusInput_ = (input & BtnFocus) != 0;
    // The direction: the first matching test (Down beats Up, Left beats Right) [QUIRK].
    const bool up = input & BtnUp, down = input & BtnDown, left = input & BtnLeft, right = input & BtnRight;
    if (up && left) direction_ = Direction::UpLeft;
    else if (down && left) direction_ = Direction::DownLeft;
    else if (up && right) direction_ = Direction::UpRight;
    else if (down && right) direction_ = Direction::DownRight;
    else if (down) direction_ = Direction::Down;
    else if (up) direction_ = Direction::Up;
    else if (left) direction_ = Direction::Left;
    else if (right) direction_ = Direction::Right;
    else direction_ = Direction::None;
    focusBookkeeping(w);
    // Speeds: the focused file's when focused (spec 3.4).
    const bool focused = focus_ == 1;
    const float orth = focused ? shots_.focused.focusSpeed : shots_.unfocused.speed;
    const float diag = focused ? shots_.focused.focusDiagonalSpeed : shots_.unfocused.diagonalSpeed;
    float vx = 0.0f, vy = 0.0f;
    switch (direction_) {
        case Direction::Up: vy = -orth; break;
        case Direction::Down: vy = orth; break;
        case Direction::Left: vx = -orth; break;
        case Direction::Right: vx = orth; break;
        case Direction::UpLeft: vx = -diag, vy = -diag; break;
        case Direction::UpRight: vx = diag, vy = -diag; break;
        case Direction::DownLeft: vx = -diag, vy = diag; break;
        case Direction::DownRight: vx = diag, vy = diag; break;
        default: break;
    }
    vx *= moveMulX_;
    vy *= moveMulY_;
    // The lean animation follows the horizontal speed (scripts 1-4, or 6-9 for the youkai).
    const bool youkaiSprite = isSolo(character_) ? isYoukaiSolo(character_) : focused;
    const int base = youkaiSprite ? 5 : 0;
    if (vx < 0.0f && prevVx_ >= 0.0f) setSpriteScript(w, base + 1);
    else if (vx == 0.0f && prevVx_ < 0.0f) setSpriteScript(w, base + 2);
    else if (vx > 0.0f && prevVx_ <= 0.0f) setSpriteScript(w, base + 3);
    else if (vx == 0.0f && prevVx_ > 0.0f) setSpriteScript(w, base + 4);
    prevVx_ = vx;
    vx_ = vx;
    vy_ = vy;
    pos_.x += vx * g_timing.speed;
    pos_.y += vy * g_timing.speed;
    if (pos_.x < 8.0f) pos_.x = 8.0f;
    else if (pos_.x > 376.0f) pos_.x = 376.0f;
    if (pos_.y < 16.0f) pos_.y = 16.0f;
    else if (pos_.y > 432.0f) pos_.y = 432.0f;
    const float hb = header().hitbox / 2.0f, gb = header().grazeBox / 2.0f, ib = header().itemBox / 2.0f;
    hitMin_ = {pos_.x - hb, pos_.y - hb, 0}, hitMax_ = {pos_.x + hb, pos_.y + hb, 0};
    grazeMin_ = {pos_.x - gb, pos_.y - gb, 0}, grazeMax_ = {pos_.x + gb, pos_.y + gb, 0};
    itemMin_ = {pos_.x - ib, pos_.y - ib, 0}, itemMax_ = {pos_.x + ib, pos_.y + ib, 0};
    // Familiars: after this frame's position, before the history shift.
    const Vec3f oldest = history_[kHistory - 1];
    for (Familiar& f : familiars_) {
        if (f.kind == FamiliarKind::None) continue;
        updateFamiliar(w, f, oldest);
        f.vm.tick(*w.rng);
        f.timer.tick();
    }
    if (vx != 0.0f || vy != 0.0f) {
        for (int i = kHistory - 1; i > 0; --i) history_[static_cast<std::size_t>(i)] = history_[static_cast<std::size_t>(i - 1)];
        history_[0] = pos_;
    }
    gaugeFromShooting(w);
    // The first shot trigger (0x44BDBE).
    if ((input & BtnShoot) && fireCycle_.current < 0 && !w.dialogue) fireCycle_.assign(0);
}

// The gauge from shooting and its decay (spec 11.3).
void Player::gaugeFromShooting(const World& w) {
    if (w.dialogue || switchCounter_ < 30 || bombActive_) return;
    GameState& g = *w.state;
    if (fireCycle_.current >= 0) {
        if (gaugeIdle_.current > 0) {
            gaugeIdle_.decrement(1);
            return;
        }
        const float t = gaugeRamp_.asFloat();
        int n = t > 300.0f ? 21 : static_cast<int>(t / 15.0f);  // 21 from t = 301 [QUIRK]
        if (focus_ != 1) n = -n;
        g.addGauge(static_cast<int>(static_cast<float>(n) * g_timing.speed), false, bombActive_);
        gaugeRamp_.tick();
        return;
    }
    if (gaugeIdle_.current >= 4) gaugeRamp_.assign(0);
    if (gaugeIdle_.current >= 30) {
        if (std::abs(static_cast<int>(g.gauge)) <= 9) {
            g.gauge = 0;  // set directly: no clamp, the display copy untouched
            return;
        }
        int delta;
        if (g.y80()) delta = -5;
        else if (g.y20()) delta = -3;
        else if (g.gauge > 0) delta = -2;
        else if (!g.h20()) delta = 2;
        else if (!g.h80()) delta = 3;
        else delta = 5;
        g.addGauge(static_cast<int>(static_cast<float>(delta) * g_timing.speed), false, bombActive_);
    } else {
        gaugeIdle_.tick();
    }
}

// ---- shots ------------------------------------------------------------------------------

Vec3f Player::originOf(int origin) const {
    if (origin >= 1 && origin <= kFamiliars) return familiars_[static_cast<std::size_t>(origin - 1)].pos;
    return pos_;
}

int Player::liveShots() const {
    int n = 0;
    for (const Shot& s : shotSlots_) n += s.state != 0 ? 1 : 0;
    return n;
}

void Player::shotInit(const World& w, Shot& s, const ShotEntry& e) {
    const Vec3f origin = originOf(e.origin);
    s.pos = {origin.x + e.offsetX, origin.y + e.offsetY, 0.495f};
    s.boxW = e.boxW;
    s.boxH = e.boxH;
    s.angle = e.angle;
    s.speed = e.speed;
    s.velocity = polarOnce(e.angle, e.speed);
    s.age.reset();
    s.age.assign(0);
    s.focus = focus_;
    s.shotClass = e.shotClass;
    s.damage = e.damage;
    s.sprite = e.sprite;
    s.side = e.side;
    s.updateActive = true;
    if (e.sound >= 0) w.playSound(e.sound);
    s.vm = AnmVm{};
    s.vm.start(*w.anm, *w.rng, kSlotPlayer, e.sprite + 10);
    s.youkaiPower = w.state->y80() && e.side > 0;  // (tints the sprite red)
}

bool Player::fire(const World& w, Shot& s, int slot, const ShotEntry& e, int cycle) {
    auto periodic = [&]() { return e.interval > 0 && cycle % e.interval == e.phase; };
    switch (e.fireKind) {
        case 1:
        case 5: {
            if (!periodic()) return false;
            shotInit(w, s, e);
            const bool aim = e.fireKind == 1 ? targetEnemy_ >= 0 && w.enemies : target_.x > -100.0f;
            if (aim) {
                const Vec3f t = e.fireKind == 1 ? w.enemies->enemy(targetEnemy_).pos : target_;
                s.angle = addNormalizeAngle(std::atan2(t.y - s.pos.y, t.x - s.pos.x), e.angle + kZunPi / 2.0f);
                s.velocity = polarOnce(s.angle, 1.5f * e.speed);
            }
            return true;
        }
        case 2:
        case 3:
            if (bombActive_ || !periodic()) return false;
            shotInit(w, s, e);
            return true;
        case 4: {  // the doll laser (not periodic)
            const int k = std::clamp<int>(e.phase, 0, 2);
            if (bombActive_ || (w.mode & ModeBit13)) return false;
            LaserSlot& ls = lasers_[static_cast<std::size_t>(k)];
            if (ls.shot >= 0) {
                if (ls.entry != &e) {
                    shotSlots_[static_cast<std::size_t>(ls.shot)].vm.interrupt(1);
                    ls.shot = -1;
                }
                return false;
            }
            ls.timer = 999;
            ls.shot = slot;
            s.laserSlot = k;
            s.origin = e.origin;
            s.offsetX = e.offsetX;
            s.offsetY = e.offsetY;
            s.segments = e.interval;
            shotInit(w, s, e);
            for (Vec3f& t : s.trail) t.x = -999.0f;
            s.pos.x = -999.0f;
            ls.entry = &e;
            return true;
        }
        case 6:
            if (!periodic()) return false;
            shotInit(w, s, e);
            s.angle = addNormalizeAngle(-kZunPi / 2.0f, e.angle + kZunPi / 2.0f);
            s.velocity = polarOnce(s.angle, e.speed);
            return true;
        case 7:
            if (!periodic()) return false;
            shotInit(w, s, e);
            s.angle = randSigned(*w.rng) * kZunPi / 48.0f - kZunPi / 2.0f;
            s.velocity = polarOnce(s.angle, e.speed);
            return true;
        case 8:
            if (bombActive_ || !periodic()) return false;
            shotInit(w, s, e);
            s.angle = addNormalizeAngle(familiars_[2].facing, e.angle);
            s.velocity = polarOnce(s.angle, e.speed);
            return true;
        default:
            if (!periodic()) return false;
            shotInit(w, s, e);
            return true;
    }
}

void Player::spawnShots(const World& w, int cycle) {
    const ShotFile& file = focus_ == 1 ? shots_.focused : shots_.unfocused;
    if (file.levels.empty()) return;
    int level = file.levelFor(w.state->powerI());
    // Remilia's in-bomb levels (spec 4.3) [QUIRK].
    if (bombActive_ && (character_ == 9 || (character_ == 2 && (bombIndex_ & 1))) && bombTimer_.current >= 60) {
        level = bombIndex_ < 2 ? 6 : 7;
    }
    const std::vector<ShotEntry>& list = file.entries(level);
    if (list.empty()) return;
    std::size_t e = 0;
    for (int slot = 0; slot < kShots; ++slot) {
        Shot& s = shotSlots_[static_cast<std::size_t>(slot)];
        if (s.state != 0) continue;
        while (true) {
            const ShotEntry& entry = list[e];
            const bool fired = fire(w, s, slot, entry, cycle);
            if (fired) {
                s.state = 1;
                s.entry = &entry;
                s.updateKind = entry.updateKind;
                s.drawKind = entry.drawKind;
                s.hitKind = entry.hitKind;
                s.vm.set(AnmVm::ZWriteDisable, true);
            }
            if (++e >= list.size()) return;
            if (fired) break;
        }
    }
}

void Player::fireCycle(const World& w, std::uint16_t input) {
    if (w.stageFrame < 20) return;
    if (fireCycle_.current < 0) return;
    // (The Dissolve spell stops it here: not ported yet.)
    if (ticked(fireCycle_) && !(bombActive_ && (character_ == 1 || character_ == 6 || character_ == 7))) {
        spawnShots(w, fireCycle_.current);
    }
    fireCycle_.tick();
    if (fireCycle_.current >= 20) fireCycle_.assign(-1);
    if ((input & BtnShoot) && fireCycle_.current < 0 && !w.dialogue) fireCycle_.assign(0);
    if (dead()) fireCycle_.assign(-1);
}

bool Player::runShotUpdate(const World& w, Shot& s, int slot) {
    if (!s.updateActive) return true;
    switch (s.updateKind) {
        case 1: {  // homing
            if (s.state == 1) {
                if (target_.x > -100.0f && s.age.current < 40 && ticked(s.age)) {
                    const float dx = target_.x - s.pos.x, dy = target_.y - s.pos.y;
                    float l = std::sqrt(dx * dx + dy * dy) / (s.speed / 4.0f);
                    if (l < 1.0f) l = 1.0f;
                    const float vx = dx / l + s.velocity.x, vy = dy / l + s.velocity.y;
                    const float m = std::sqrt(vx * vx + vy * vy);
                    s.speed = m > 10.0f ? 10.0f : m;
                    if (s.speed < 1.0f) s.speed = 1.0f;
                    s.velocity.x = vx * s.speed / m;
                    s.velocity.y = vy * s.speed / m;
                } else if (s.speed < 10.0f) {
                    s.speed += 0.33333334f;
                    const float len = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.y * s.velocity.y);
                    if (len > 0.0f) {
                        s.velocity.x = s.velocity.x * s.speed / len;
                        s.velocity.y = s.velocity.y * s.speed / len;
                    }
                }
            }
            s.angle = std::atan2(s.velocity.y, s.velocity.x);
            return true;
        }
        case 3:  // rising missile: one float draw per flying missile
            if (s.state == 1) s.velocity.y -= w.rng->f32InRange(0.1f) + 0.27f;
            return true;
        case 5: {  // the doll laser (spec_bombs 11.2)
            LaserSlot& ls = lasers_[static_cast<std::size_t>(std::clamp(s.laserSlot, 0, 2))];
            if (ls.shot != slot || w.dialogue || fireCycle_.current < 0 || state_ == State::Dead || bombActive_ || (w.mode & ModeBit13)) {
                s.vm.interrupt(1);
                if (ls.shot == slot) ls.shot = -1;
                s.updateActive = false;  // never called again; the rest of this frame still runs [QUIRK]
            }
            if (familiars_[0].state == 0) {
                if (ls.shot == slot) ls.shot = -1;
                return false;
            }
            // (The trail's damage regions come with the regions.)
            for (int j = 31; j >= 1; --j) {
                s.trail[static_cast<std::size_t>(j)] = s.trail[static_cast<std::size_t>(j - 1)];
                s.trail[static_cast<std::size_t>(j)].y -= 1.0f;
            }
            s.trail[0] = s.pos;
            s.pos = familiars_[0].pos;
            s.pos.z = 0.44f;
            s.boxH = 448.0f;
            s.pos.y -= 208.0f;
            return true;
        }
        default: return true;
    }
}

void Player::shotUpdate(const World& w) {
    if (w.mode & ModeSpellFreeze) return;
    for (int i = 0; i < kShots; ++i) {
        Shot& s = shotSlots_[static_cast<std::size_t>(i)];
        if (s.state == 0) continue;
        if (!runShotUpdate(w, s, i)) {
            s.state = 0;
            continue;
        }
        s.pos.x += s.velocity.x * g_timing.speed;
        s.pos.y += s.velocity.y * g_timing.speed;
        if (!s.entry || !s.entry->beam()) {
            const float sw = s.vm.spriteWidth, sh = s.vm.spriteHeight;
            if (s.pos.x + sw / 2.0f < 0.0f || s.pos.x - sw / 2.0f > kPlayfieldW || s.pos.y + sh / 2.0f < 0.0f ||
                s.pos.y - sh / 2.0f > kPlayfieldH) {
                s.state = 0;  // still runs its script and ticks this frame [QUIRK, harmless]
            }
        }
        if (s.vm.tick(*w.rng)) {
            s.state = 0;
            continue;
        }
        s.age.tick();
    }
}

int Player::damageQuery(const World& w, const Vec3f& centre, const Vec3f& size, std::int32_t& accumulator, bool& bomb) {
    if (!ticked(stateTimer_)) return 0;
    bomb = false;
    GameState& g = *w.state;
    int sum = 0;
    const float hw = size.x / 2.0f, hh = size.y / 2.0f;
    // 3. Shots.
    for (Shot& s : shotSlots_) {
        if (!(s.state == 1 || (s.state != 0 && s.shotClass == 3))) continue;
        if (!boxesTouch(s.pos, s.boxW / 2.0f, s.boxH / 2.0f, centre, hw, hh)) continue;
        if ((s.shotClass == 4 || s.shotClass == 5) && (s.age.current & 1)) continue;
        if (s.hitKind == 1) {  // explosive (spec_bombs 11.3)
            if (s.state == 2) {
                if (s.age.current & 1) continue;
                if (w.spellCardActive && (s.age.current % 4) != 0) continue;
                s.damage = static_cast<std::int16_t>(std::max(1, s.damage / 3));
                s.velocity = s.velocity * 0.88f;
            } else {
                const float blast = w.rng->f32InRange(kZunPi / 2.0f) - 3.0f * kZunPi / 4.0f;
                float box = -1.0f;
                switch (s.vm.scriptIndex - 12) {
                    case 0: box = 48.0f; break;
                    case 2: box = 64.0f; break;
                    case 4: box = 80.0f; break;
                    case 6: box = 96.0f; break;
                    case 8: box = 128.0f; break;
                    default: break;
                }
                if (box > 0.0f) {
                    s.boxW = s.boxH = box;
                    s.velocity = polarOnce(blast, 6.0f);
                }
            }
            if (s.age.current % 6 == 0 && w.effects) w.effects->spawn(w, 5, centre, 1, 0xFFFFFFFFu);
        } else if (s.hitKind == 2) {  // the doll laser's sparks (shared counter)
            ++sparkCounter_;
            if (sparkCounter_ % 8 == 0 && w.effects) w.effects->spawn(w, 5, {s.pos.x, centre.y, 0.0f}, 1, 0xFFFFFFFFu);
        }
        sum += bombActive_ ? std::max(1, s.damage / 5) : s.damage;
        // Time orbs from the damage, with the accumulator as it stood before this query.
        while (accumulator >= orbStep_) {
            if (g.h80() && s.side < 0 && w.bullets) w.bullets->spawnItem(w, s.pos, BulletManager::TimeOrb, BulletManager::Falling);
            accumulator -= orbStep_;
        }
        if (s.shotClass == 4 || s.shotClass == 5 || s.shotClass == 6) continue;  // piercing
        if (s.state == 1) {
            s.vm.start(*w.anm, *w.rng, kSlotPlayer, s.sprite + 11);
            if (w.effects) w.effects->spawn(w, 5, s.pos, 1, 0xFFFFFFFFu);
            s.pos.z = 0.1f;
            s.state = 2;
            if (s.shotClass != 3) s.velocity = s.velocity * (1.0f / 8.0f);
        }
    }
    // 4.
    accumulator += std::min(sum, 50);
    // 5. Damage regions, on the frames their interval allows.
    for (int i = 0; i < 192; ++i) {
        Region& r = regions_[static_cast<std::size_t>(i)];
        if (!r.active || r.interval == 0 || (r.lifetime % r.interval) != 0) continue;
        bool hit;
        if (r.radius != 0.0f) {
            const float dx = centre.x - r.centre.x, dy = centre.y - r.centre.y;
            hit = dx * dx + dy * dy <= r.radius * r.radius;
        } else if (r.angle != 0.0f) {
            const float sn = std::sin(-r.angle), cs = std::cos(-r.angle);
            const float dx = centre.x - r.centre.x, dy = centre.y - r.centre.y;
            const float x = dx * cs - dy * sn, y = dx * sn + dy * cs;
            hit = std::fabs(x) <= r.w / 2.0f + hw && std::fabs(y) <= r.h / 2.0f + hh;
        } else {
            hit = boxesTouch(r.centre, r.w / 2.0f, r.h / 2.0f, centre, hw, hh);
        }
        if (!hit) continue;
        int d = r.damage;
        r.dealt += d;
        if (r.cap > 0 && r.dealt >= r.cap) {
            d -= r.dealt - r.cap;
            r.damage = 0;
        }
        sum += d;
        if (!r.noSpark) {
            ++sparkCounter_;
            if (sparkCounter_ % 4 == 0 && w.effects) w.effects->spawn(w, 3, centre, 1, 0xFFFFFFFFu);
        }
        if (bombActive_) bomb = true;
    }
    // 6. The youkai bonus.
    if (g.y80() && sum != 0) sum = sum * 106 / 100;
    return sum;
}

// ---- regions and the tests against the player ---------------------------------------------

void Player::regionUpkeep() {
    for (Region& r : regions_) {
        if (r.lifetime < 0) continue;
        --r.lifetime;
        r.radius += r.growth;
        r.w += r.wGrowth;
        r.h += r.hGrowth;
        if (r.lifetime <= 0) r.active = false;
    }
}

// The first inactive slot among the group's first 191, else the group's last (spec 10.2).
int Player::freeRegion(int first) const {
    for (int i = first; i < first + 191; ++i) {
        if (!regions_[static_cast<std::size_t>(i)].active) return i;
    }
    return first + 191;
}

int Player::cancelRect(const Vec3f& centre, float w, float h, int itemType, std::int32_t lifetime) {
    const int i = freeRegion(192);
    Region& r = regions_[static_cast<std::size_t>(i)];
    r = Region{};
    r.centre = centre, r.w = w, r.h = h, r.itemType = itemType, r.lifetime = lifetime, r.active = true;
    return i;
}

int Player::cancelCircle(const Vec3f& centre, float radius, float growth, std::int32_t lifetime, int itemType) {
    const int i = freeRegion(192);
    Region& r = regions_[static_cast<std::size_t>(i)];
    r = Region{};
    r.centre = centre, r.radius = radius, r.growth = growth, r.lifetime = lifetime, r.itemType = itemType, r.active = true;
    return i;
}

int Player::damageRect(const Vec3f& centre, float w, float h, std::int32_t damage, std::int32_t lifetime) {
    const int i = freeRegion(0);
    Region& r = regions_[static_cast<std::size_t>(i)];
    r = Region{};
    r.centre = centre, r.w = w, r.h = h, r.damage = damage, r.lifetime = lifetime, r.active = true;
    return i;
}

int Player::damageCircle(const Vec3f& centre, float radius, float growth, std::int32_t damage, std::int32_t lifetime) {
    const int i = freeRegion(0);
    Region& r = regions_[static_cast<std::size_t>(i)];
    r = Region{};
    r.centre = centre, r.radius = radius, r.growth = growth, r.damage = damage, r.lifetime = lifetime, r.active = true;
    return i;
}

bool Player::cancelTest(const Vec3f& c) {
    for (int i = 192; i < kRegions; ++i) {
        Region& r = regions_[static_cast<std::size_t>(i)];
        if (!r.active) continue;
        bool hit;
        if (r.radius != 0.0f) {
            const float dx = c.x - r.centre.x, dy = c.y - r.centre.y;
            hit = dx * dx + dy * dy < r.radius * r.radius;
        } else if (r.angle != 0.0f) {
            const float s = std::sin(-r.angle), co = std::cos(-r.angle);
            const float dx = c.x - r.centre.x, dy = c.y - r.centre.y;
            const float x = dx * co - dy * s, y = dx * s + dy * co;
            hit = std::fabs(x) <= r.w / 2.0f && std::fabs(y) <= r.h / 2.0f;
        } else {
            hit = c.x >= r.centre.x - r.w / 2.0f && c.x <= r.centre.x + r.w / 2.0f && c.y >= r.centre.y - r.h / 2.0f &&
                  c.y <= r.centre.y + r.h / 2.0f;
        }
        if (hit) {
            cancelItemType_ = r.itemType;
            ++r.dealt;
            return true;
        }
    }
    return false;
}

Player::HitResult Player::bulletHit(const World& w, const Vec3f& centre, const Vec3f& size) {
    cancelItemType_ = 6;
    if (cancelTest(centre)) return HitResult::Cancelled;
    const float hw = size.x / 2.0f, hh = size.y / 2.0f;
    if (centre.x + hw < hitMin_.x || centre.x - hw > hitMax_.x || centre.y + hh < hitMin_.y || centre.y - hh > hitMax_.y) {
        return HitResult::None;
    }
    if (state_ != State::Alive) return HitResult::Hit;  // absorbed
    for (int k = 0; k < 8; ++k) (void)w.rng->u16();  // (the 16-step reroll: 5 u32 and 3 floats)
    for (int k = 0; k < 8; ++k) (void)w.rng->u16();
    die(w);
    return HitResult::Hit;
}

Player::HitResult Player::bulletGraze(const World& w, const Vec3f& centre, const Vec3f& size, bool laser) {
    cancelItemType_ = 6;
    if (cancelTest(centre)) return HitResult::Cancelled;
    const float hw = size.x / 2.0f + 20.0f, hh = size.y / 2.0f + 20.0f;
    if (dead()) return HitResult::None;
    if (centre.x + hw < grazeMin_.x || centre.x - hw > grazeMax_.x || centre.y + hh < grazeMin_.y || centre.y - hh > grazeMax_.y) {
        return HitResult::None;
    }
    scoreGraze(w, centre, laser);
    return HitResult::Grazed;
}

int Player::laserTest(const World& w, const Vec3f& origin, float angle, const Vec3f& c, const Vec3f& size, bool grazeAllowed) {
    // The player point rotated into the laser's frame about its origin.
    const float s = std::sin(-angle), co = std::cos(-angle);
    const float dx = pos_.x - origin.x, dy = pos_.y - origin.y;
    const Vec3f p{dx * co - dy * s + origin.x, dx * s + dy * co + origin.y, 0.0f};
    const float hb = header().hitbox / 2.0f;
    const float hw = size.x / 2.0f, hh = size.y / 2.0f;
    if (!(p.x + hb < c.x - hw || p.x - hb > c.x + hw || p.y + hb < c.y - hh || p.y - hb > c.y + hh)) {
        if (state_ != State::Alive) return 0;
        for (int k = 0; k < 16; ++k) (void)w.rng->u16();
        die(w);
        return 1;
    }
    if (!grazeAllowed) return 0;
    const float gw = hw + 48.0f, gh = hh + 48.0f;
    if (!(p.x + hb < c.x - gw || p.x - hb > c.x + gw || p.y + hb < c.y - gh || p.y - hb > c.y + gh) && !dead()) {
        scoreGraze(w, pos_, true);
        return 2;
    }
    return 0;
}

Player::HitResult Player::enemyContact(const World& w, const Vec3f& centre, const Vec3f& box) {
    const float hw = box.x / 1.5f / 2.0f, hh = box.y / 1.5f / 2.0f;
    if (centre.x + hw < hitMin_.x || centre.x - hw > hitMax_.x || centre.y + hh < hitMin_.y || centre.y - hh > hitMax_.y) {
        return HitResult::None;
    }
    if (state_ == State::Alive) {
        for (int k = 0; k < 16; ++k) (void)w.rng->u16();
        die(w);
    }
    return HitResult::Hit;
}

void Player::scoreGraze(const World& w, const Vec3f& at, bool laser) {
    GameState& g = *w.state;
    if (!bombActive_) {
        const int k = g.h80() ? 3 : g.h20() ? 2 : 1;
        if (g.stageGraze < 99999) g.stageGraze += k;
        if (g.graze < 999999) g.graze += k;
    }
    if (w.effects) w.effects->spawn(w, 8, (pos_ + at) * 0.5f, 1, 0xFFFFFFFFu);
    g.increaseSubrank(6);
    w.playSound(30);
    g.score += g.y20() ? 400 : 200;
    if (youkaiFront_) g.addGauge(100, false, bombActive_);
    // Time orbs from grazing near a boss at youkai 80 %.
    if ((!isHumanSolo(character_) || character_ == 10) && w.enemies && w.enemies->anyBoss() && g.y80() && w.bullets) {
        w.bullets->spawnItem(w, at, 10, BulletManager::Falling);
        if (!laser && w.spellCardActive) {
            w.bullets->spawnItem(w, at, 10, BulletManager::Falling);
            if (!isYoukaiSolo(character_)) w.bullets->spawnItem(w, at, 10, BulletManager::Falling);
        }
    }
}

bool Player::pickupBoxTouches(const Vec3f& item) const {
    const float half = header().itemBox / 2.0f;
    return !(item.x + half < itemMin_.x || item.x - half > itemMax_.x || item.y + half < itemMin_.y || item.y - half > itemMax_.y);
}

void Player::makeInvulnerable(std::int32_t frames) {
    if (state_ != State::Alive) return;
    state_ = State::Invulnerable;
    stateTimer_.assign(frames);
}

// ---- death (spec 8) -----------------------------------------------------------------------

void Player::die(const World& w) {
    GameState& g = *w.state;
    w.timeStop = false;
    antiTamper(*w.rng);
    if (w.effects) w.effects->spawn(w, 6, pos_, 16, 0xFFFFFFFFu);
    state_ = State::Dead;
    stateTimer_.assign(0);
    w.playSound(4);
    committed_ = false;
    if (w.mode & ModeDissolve) {  // a boss's Last Spell: the Dissolve spell starts next frame
        countdown_ = 2;
        dissolveTrigger_ = true;
    } else {
        g.gauge = 0;  // set directly: the display copy is not touched [QUIRK]
        // The deathbomb countdown: bombs x 6, +7 with the time quota met, at most 15 (twice in
        // a card, at most 30); x 9 / 5 for Reimu's characters.
        const int bombs = g.bombsI();
        if (bombs >= 1) {
            countdown_ = bombs * 6;
            if (g.timePoints >= g.timeQuota) countdown_ += 7;
            countdown_ = std::min(countdown_, 15);
            if (w.spellCardActive) countdown_ = std::min(countdown_ * 2, 30);
            if (character_ == 0 || character_ == 4 || character_ == 5) countdown_ = countdown_ * 9 / 5;
            vm_.set(AnmVm::UseAltColor, true);
            vm_.altColor = 0xFFFFFFFFu;
            if (w.effects) {
                deathEffect_ = &w.effects->spawnSpecial(w, 59, pos_, 11, 1, 0xFFF0404Fu);
                deathEffect_->ignoreFreeze = true;
            }
            if (w.spellCardActive) w.mode |= ModeSpellFreeze;
        } else {
            countdown_ = 2;
        }
    }
    if (w.bullets) w.bullets->stopHoming();
}

void Player::commitDeath(const World& w) {
    GameState& g = *w.state;
    if (deathEffect_) deathEffect_->inUse = false;
    deathEffect_ = nullptr;
    if (w.effects) {
        w.effects->spawnSpecial(w, 12, pos_, 3, 1, 0xFF4040FFu);
        w.effects->spawn(w, 6, pos_, 16, 0xFFFFFFFFu);
    }
    w.playSound(15);
    w.mode &= ~static_cast<std::uint32_t>(ModeSpellFreeze);
    vm_.set(AnmVm::UseAltColor, false);
    lastSpellArmed_ = false;
    if (w.enemies) w.enemies->playerMissed();
    g.misses += 1.0f;
    antiTamper(*w.rng);
    w.addTimePoints(-(g.timePoints > 5000 ? 500 : g.timePoints / 10));
    if (g.lives >= 1.0f) {
        g.power = g.power <= 16.0f ? 0.0f : g.power - 16.0f;
        antiTamper(*w.rng);
        if (w.bullets) {
            w.bullets->spawnItem(w, pos_, BulletManager::BigPower, BulletManager::Scatter);
            for (int k = 0; k < 5; ++k) w.bullets->spawnItem(w, pos_, BulletManager::SmallPower, BulletManager::Scatter);
            if (g.bombsI() >= 1 && (character_ == 2 || character_ == 8 || character_ == 9)) {
                w.bullets->spawnItem(w, pos_, BulletManager::Bomb, BulletManager::Scatter);
            }
            w.bullets->stopHoming();
        }
    } else {
        g.power = 0.0f;
        antiTamper(*w.rng);
        if (w.bullets) {
            for (int k = 0; k < 5; ++k) w.bullets->spawnItem(w, pos_, BulletManager::FullPower, BulletManager::Scatter);
        }
    }
    g.decreaseSubrank(1600);
}

bool Player::deathStep(const World& w) {
    GameState& g = *w.state;
    if (countdown_ != 0) {
        w.addTimePoints(-15);
        --countdown_;
        lastSpellArmed_ = true;
        if (countdown_ == 0) {
            commitDeath(w);
            committed_ = true;
        }
        return false;
    }
    // Fade out, then the respawn move.
    const float t = static_cast<float>(stateTimer_.current);
    vm_.scale = {1.0f - t / 30.0f, 1.0f + 3.0f * t / 30.0f};
    vm_.set(AnmVm::ScaleDirty, true);
    vm_.color = withAlpha(0xFFFFFFFFu, static_cast<std::int32_t>(255.0f - 255.0f * t / 30.0f));
    vm_.setBlend(1);
    prevVx_ = 0.0f;
    if (stateTimer_.current < 30) return false;
    state_ = State::Spawning;
    pos_ = {192.0f, 384.0f, 0.2f};
    stateTimer_.assign(0);
    vm_.scale = {3.0f, 3.0f};
    const bool youkaiSprite = isSolo(character_) ? isYoukaiSolo(character_) : focus_ == 1;
    setSpriteScript(w, youkaiSprite ? 5 : 0);
    if (g.lives < 1.0f) {
        continueRequested_ = true;  // (the continue menu is not ported yet)
        return false;
    }
    g.lives -= 1.0f;
    antiTamper(*w.rng);
    g.bombs = static_cast<float>(static_cast<int>(header().bombsPerLife));
    return true;
}

// ---- drawing ----------------------------------------------------------------------------

void Player::drawShots(SpriteSink& sink, const AnmDrawState& state) {
    drawShotSprites(sink, state);
    if (bomb_.active) drawBomb(sink, state);
}

void Player::drawShotSprites(SpriteSink& sink, const AnmDrawState& state) {
    for (Shot& s : shotSlots_) {
        if (s.state == 0) continue;
        if (s.state == 1 && s.vm.autoRotate != 0) s.vm.rotation.z = s.angle + kZunPi / 2.0f;
        s.vm.position = {s.pos.x + kPlayfieldX, s.pos.y + kPlayfieldY, s.pos.z};
        drawVm(sink, s.vm, state);
    }
}

void Player::drawPlayer(SpriteSink& sink, const AnmDrawState& state) {
    vm_.position = {pos_.x + kPlayfieldX, pos_.y + kPlayfieldY, pos_.z};
    drawVm(sink, vm_, state);
    for (Familiar& f : familiars_) {
        if (f.kind == FamiliarKind::None) continue;
        f.vm.position = {f.pos.x + kPlayfieldX, f.pos.y + kPlayfieldY, 0.49f};
        drawVm(sink, f.vm, state);
    }
}

}  // namespace touhou::th08
