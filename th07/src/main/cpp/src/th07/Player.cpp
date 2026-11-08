#include "th07/Player.hpp"

#include <algorithm>
#include <cmath>

#include "core/Log.hpp"
#include "io/Vfs.hpp"
#include "th07/Effects.hpp"
#include "th07/World.hpp"

namespace touhou::th07 {

namespace {

constexpr int kPlayerScripts = 0x400;
constexpr Vec3f kNoTarget{-999.0f, -999.0f, 0.0f};

bool ticked(const Timer& t) { return g_timing.speed > 0.99f || t.current != t.previous; }

// Inclusive axis-aligned overlap of centre +- half extents.
bool boxesTouch(const Vec3f& c1, float hw1, float hh1, const Vec3f& c2, float hw2, float hh2) {
    return !(c1.x + hw1 < c2.x - hw2 || c1.x - hw1 > c2.x + hw2 || c1.y + hh1 < c2.y - hh2 || c1.y - hh1 > c2.y + hh2);
}

std::uint32_t withAlpha(std::uint32_t argb, std::int32_t a) {
    return (argb & 0x00FFFFFFu) | (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24);
}

}  // namespace

bool Player::load(const Vfs& vfs, TextureLibrary& textures, AnmLibrary& anm, int shotIndex, bool firstStage, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    shotIndex_ = shotIndex;
    for (bool focusedFile : {false, true}) {
        const std::string name = shotFileName(shotIndex, focusedFile);
        const auto bytes = vfs.read(name);
        if (!bytes) return fail(name + " is not in the archive");
        std::string why;
        if (!(focusedFile ? shots_.focused : shots_.unfocused).parse(*bytes, &why)) return fail(name + ": " + why);
    }
    if (firstStage || !anmLoaded_) {
        std::string why;
        if (!anm.load(vfs, textures, playerAnmName(shotIndex), 10, kPlayerScripts, &why)) return fail(why);
        anmLoaded_ = true;
    }
    return true;
}

void Player::releaseAnm(AnmLibrary& anm) {
    if (anmLoaded_) anm.release(10);
    anmLoaded_ = false;
}

void Player::startScript(const World& w, AnmVm& vm, int local) { vm.start(*w.anm, *w.rng, kPlayerScripts + local); }

void Player::stageStart(const World& w) {
    const int shotIndex = shotIndex_;
    const bool anmLoaded = anmLoaded_;
    ShotType shots = std::move(shots_);
    // All player state cleared.
    vm_ = AnmVm{};
    optionVms_ = {};
    shotSlots_ = {};
    lasers_ = {};
    regions_ = {};
    cancels_ = {};
    proj_ = {};
    bombVms_ = {};
    shotIndex_ = shotIndex;
    anmLoaded_ = anmLoaded;
    shots_ = std::move(shots);
    border_ = BorderFlag::None;
    borderLockout_ = 0;
    bulletClear_ = 0;
    continueMenu_ = false;
    bombActive_ = bombing_ = bombFocus_ = false;
    w.state->bombActive = false;
    bombTimer_.reset();
    bombDuration_ = 0;
    drain_ = 0;
    focusEffect_ = bombRing_ = borderEffect_ = nullptr;
    sparkCounter_ = 0;
    target_ = sakuyaTarget_ = kNoTarget;
    targetLocked_ = false;

    pos_ = {192.0f, 384.0f, 0.49f};
    state_ = State::Spawning;
    stateTimer_.assign(120);
    respawn_ = header().deathbombWindow;
    firing_ = false;
    fireCycle_.assign(-1);
    optionAngle_ = -kZunPi / 2.0f;
    moveMulX_ = moveMulY_ = 1.0f;
    vxRaw_ = vyRaw_ = prevVxRaw_ = vx_ = vy_ = 0.0f;
    focused_ = false;
    options_ = Options::Unfocused;
    optionCounter_ = 0;
    const float hb = header().hitbox / 2.0f, gb = header().grazeBox / 2.0f;
    hitMin_ = {pos_.x - hb, pos_.y - hb, 0};
    hitMax_ = {pos_.x + hb, pos_.y + hb, 0};
    grazeMin_ = {pos_.x - gb, pos_.y - gb, 0};
    grazeMax_ = {pos_.x + gb, pos_.y + gb, 0};
    pickMin_ = {pos_.x - 12.0f, pos_.y - 12.0f, 0};
    pickMax_ = {pos_.x + 12.0f, pos_.y + 12.0f, 0};
    optionPos_[0] = {pos_.x - 24.0f, pos_.y, 0};
    optionPos_[1] = {pos_.x + 24.0f, pos_.y, 0};
    if (anmLoaded_) {
        startScript(w, vm_, 0);
        startScript(w, optionVms_[0], 128);
        startScript(w, optionVms_[1], 129);
    }
    if (w.state->cherryPlus >= GameState::kCherryPlusFull) requestBorder(w);
}

// ---- the frame ---------------------------------------------------------------------------

void Player::update(const World& w, std::uint16_t input) {
    if (w.state->timeStop) return;  // never set in 1.00b
    regionUpkeep();
    bombStep(w, input);
    bool spawnHandled = false;
    if (state_ == State::Dead) {
        const bool moved = deathStep(w);
        if (state_ == State::Spawning) {
            spawnHandled = true;
            if (moved) spawnInStep(w);
        }
    }
    if (state_ == State::Spawning && !spawnHandled) spawnInStep(w);
    stateTimerStep(w);
    if (state_ != State::Dead && state_ != State::Spawning) inputStep(w, input);
    vm_.tick(*w.anm, *w.rng);
    optionVms_[0].tick(*w.anm, *w.rng);
    optionVms_[1].tick(*w.anm, *w.rng);
    shotUpdate(w);
    fireCycle(w);
    target_ = sakuyaTarget_ = kNoTarget;
    targetLocked_ = false;
}

void Player::regionUpkeep() {
    for (DamageRegion& r : regions_) r.w = 0.0f;
    for (CancelRegion& c : cancels_) {
        if (c.w == 0.0f && c.radius == 0.0f) continue;
        if (c.lifetime <= 0) {
            c.radius = 0.0f;
            c.w = 0.0f;
        } else {
            --c.lifetime;
            c.radius += c.growth;
        }
    }
}

void Player::bombStep(const World& w, std::uint16_t input) {
    GameState& g = *w.state;
    const bool held = (input & BtnBomb) != 0;
    if (border_ != BorderFlag::None && !bombActive_ && held) {
        breakBorder(w);
        bombing_ = false;
        if (w.items) w.items->collectAll();
        return;
    }
    if (border_ == BorderFlag::Ready) requestBorder(w);
    if (borderLockout_ != 0) --borderLockout_;
    if (bombActive_) {
        if (ticked(bombTimer_)) g.cherry = g.cherry >= drain_ ? g.cherry - drain_ : 0;
        bombFrame(w);
    } else if (!g.dialogue && respawn_ != 0 && g.bombsI() >= 1 && borderLockout_ == 0 && held) {
        startBomb(w);
    } else {
        bombing_ = false;
    }
}

bool Player::deathStep(const World& w) {
    GameState& g = *w.state;
    if (respawn_ != 0) {
        if (border_ == BorderFlag::Active) {
            breakBorder(w);
            return false;
        }
        Timer t;
        t.assign(respawn_);
        t.decrement(1);
        respawn_ = std::max(0, t.current);
        if (respawn_ == 0) commitDeath(w);
        return false;
    }
    const float t = static_cast<float>(stateTimer_.current);
    vm_.scale = {1.0f - t / 30.0f, 1.0f + 3.0f * t / 30.0f};
    vm_.set(AnmVm::ScaleDirty, true);
    vm_.color = withAlpha(0xFFFFFFFFu, static_cast<std::int32_t>(255.0f - 255.0f * t / 30.0f));
    vm_.set(AnmVm::Additive, true);
    prevVxRaw_ = 0.0f;
    vxRaw_ = vyRaw_ = 0.0f;
    if (stateTimer_.current < 30) return false;
    state_ = State::Spawning;
    pos_ = {192.0f, 384.0f, 0.2f};
    stateTimer_.assign(0);
    vm_.scale = {3.0f, 3.0f};
    startScript(w, vm_, 0);
    if (g.livesI() < 1) {
        continueMenu_ = true;
        return false;
    }
    w.addLives(-1.0f);
    w.setBombs(static_cast<float>(static_cast<int>(header().initialBombs)));
    return true;
}

void Player::spawnInStep(const World& w) {
    (void)w;
    bulletClear_ = 60;
    const float t = static_cast<float>(stateTimer_.current);
    const float s = 1.0f - t / 30.0f;
    vm_.scale = {1.0f - s, 2.0f * s + 1.0f};
    vm_.set(AnmVm::ScaleDirty, true);
    vm_.set(AnmVm::Additive, true);
    vm_.color = withAlpha(0xFFFFFFFFu, stateTimer_.current * 255 / 30);
    moveMulX_ = moveMulY_ = 1.0f;
    respawn_ = 0;
    if (stateTimer_.current >= 30) {
        state_ = State::Invulnerable;
        vm_.scale = {1.0f, 1.0f};
        vm_.color = 0xFFFFFFFFu;
        vm_.set(AnmVm::Additive, false);
        stateTimer_.assign(240);
        respawn_ = header().deathbombWindow;
    }
}

void Player::stateTimerStep(const World& w) {
    GameState& g = *w.state;
    if (bulletClear_ != 0) {
        --bulletClear_;
        if (w.bullets) w.bullets->clearWithoutItems();
    }
    switch (state_) {
        case State::Invulnerable:
            if (bombRing_ && bombRing_->inUse) bombRing_->pos = pos_;
            stateTimer_.decrement(1);
            if (stateTimer_.current <= 0) {
                if (bombRing_) bombRing_->inUse = false;
                bombRing_ = nullptr;
                state_ = State::Alive;
                stateTimer_.assign(0);
                vm_.color = 0xFFFFFFFFu;
            } else {
                vm_.color = (stateTimer_.current % 8) < 2 ? 0xFF404040u : 0xFFFFFFFFu;
            }
            break;
        case State::Border:
            g.cherryPlus = std::max(0, stateTimer_.current * GameState::kCherryPlusFull / borderLength_);
            stateTimer_.decrement(1);
            if (stateTimer_.current <= 0) {
                endBorderNaturally(w);
            } else {
                vm_.color = (stateTimer_.current % 4) < 2 ? 0xFFFF0000u : 0xFFFFFFFFu;
            }
            break;
        default: stateTimer_.tick(); break;
    }
}

void Player::inputStep(const World& w, std::uint16_t input) {
    GameState& g = *w.state;
    focused_ = (input & BtnFocus) != 0;
    const ShotFile& h = header();
    const float orth = focused_ ? h.focusSpeed : h.speed;
    const float diag = focused_ ? h.focusDiagonalSpeed : h.diagonalSpeed;
    float vx = 0.0f, vy = 0.0f;
    if (input & BtnUp) {
        if (input & BtnRight) vx = diag, vy = -diag;
        else if (input & BtnLeft) vx = -diag, vy = -diag;
        else vy = -orth;
    } else if (input & BtnDown) {
        if (input & BtnRight) vx = diag, vy = diag;
        else if (input & BtnLeft) vx = -diag, vy = diag;
        else vy = orth;
    } else if (input & BtnRight) {
        vx = orth;
    } else if (input & BtnLeft) {
        vx = -orth;
    }
    vxRaw_ = vx;
    vyRaw_ = vy;
    vx_ = vx * moveMulX_ * g_timing.speed;
    vy_ = vy * moveMulY_ * g_timing.speed;
    pos_.x += vx_;
    pos_.y += vy_;
    if (pos_.x < 8.0f) pos_.x = 8.0f;
    else if (pos_.x > 376.0f) pos_.x = 376.0f;
    if (pos_.y < 16.0f) pos_.y = 16.0f;
    else if (pos_.y > 432.0f) pos_.y = 432.0f;
    const float hb = h.hitbox / 2.0f, gb = h.grazeBox / 2.0f;
    hitMin_ = {pos_.x - hb, pos_.y - hb, pos_.z - 5.0f};
    hitMax_ = {pos_.x + hb, pos_.y + hb, pos_.z + 5.0f};
    grazeMin_ = {pos_.x - gb, pos_.y - gb, pos_.z - 5.0f};
    grazeMax_ = {pos_.x + gb, pos_.y + gb, pos_.z + 5.0f};
    pickMin_ = {pos_.x - 12.0f, pos_.y - 12.0f, pos_.z - 5.0f};
    pickMax_ = {pos_.x + 12.0f, pos_.y + 12.0f, pos_.z + 5.0f};

    // The lean animation follows the raw horizontal speed.
    if (vx < 0.0f && prevVxRaw_ >= 0.0f) startScript(w, vm_, 1);
    else if (vx == 0.0f && prevVxRaw_ < 0.0f) startScript(w, vm_, 2);
    else if (vx > 0.0f && prevVxRaw_ <= 0.0f) startScript(w, vm_, 3);
    else if (vx == 0.0f && prevVxRaw_ > 0.0f) startScript(w, vm_, 4);
    prevVxRaw_ = vx;

    updateOptions(w);
    // The option angle (Sakuya B's options and fire kind 5).
    if ((input & BtnShoot) && !g.dialogue && !(input & BtnFocus)) {
        if (vx_ != 0.0f) {
            optionAngle_ += vx_ * kZunPi / 200.0f;
            optionAngle_ = std::clamp(optionAngle_, -7.0f * kZunPi / 10.0f, -3.0f * kZunPi / 10.0f);
        } else if (std::fabs(optionAngle_ + kZunPi / 2.0f) > kZunPi / 100.0f) {
            const float step = kZunPi / 50.0f * g_timing.speed;
            optionAngle_ += optionAngle_ < -kZunPi / 2.0f ? step : -step;
        } else {
            optionAngle_ = -kZunPi / 2.0f;
        }
    }
    if ((input & BtnShoot) && !g.dialogue && !firing_) {
        firing_ = true;
        fireCycle_.assign(0);
    }
}

void Player::spawnFocusEffect(const World& w) {
    if (w.effects) focusEffect_ = &w.effects->spawnSpecial(w, 24, pos_, 2, 0xFFFFFFFFu);
}

void Player::interruptFocusEffect() {
    if (focusEffect_ && focusEffect_->inUse) focusEffect_->vm.interrupt(1);
}

void Player::updateOptions(const World& w) {
    const Vec3f p = pos_;
    if (shotIndex_ != 5) {
        float X = 24.0f, Y = 0.0f;
        auto focusing = [&]() {
            ++optionCounter_;
            const float t = static_cast<float>(optionCounter_) / 8.0f;
            Y = -32.0f * t;
            X = 24.0f - 16.0f * t * t;
            if (optionCounter_ >= 8) options_ = Options::Focused;
        };
        auto unfocusing = [&]() {
            ++optionCounter_;
            const float t = static_cast<float>(optionCounter_) / 8.0f;
            Y = -32.0f + 32.0f * t;
            X = 24.0f - 16.0f * (1.0f - t * t);
            if (optionCounter_ >= 8) options_ = Options::Unfocused;
        };
        switch (options_) {
            case Options::Unfocused:
                X = 24.0f, Y = 0.0f;
                optionCounter_ = 0;
                if (!focused_) break;
                options_ = Options::Focusing;
                spawnFocusEffect(w);
                [[fallthrough]];
            case Options::Focusing:
                focusing();
                if (!focused_) {
                    options_ = Options::Unfocusing;
                    optionCounter_ = 8 - optionCounter_;
                    interruptFocusEffect();
                    unfocusing();
                }
                break;
            case Options::Focused:
                X = 8.0f, Y = -32.0f;
                optionCounter_ = 0;
                if (focused_) break;
                options_ = Options::Unfocusing;
                interruptFocusEffect();
                unfocusing();
                break;
            case Options::Unfocusing:
                unfocusing();
                if (focused_) {
                    options_ = Options::Focusing;
                    optionCounter_ = 8 - optionCounter_;
                    spawnFocusEffect(w);
                    focusing();
                }
                break;
        }
        optionPos_[0] = {p.x - X, p.y + Y, 0.0f};
        optionPos_[1] = {p.x + X, p.y + Y, 0.0f};
        return;
    }
    // Sakuya B: options rotating with the option angle.
    const float a = optionAngle_;
    const Vec3f B{24.0f * std::cos(a + kZunPi / 2.0f), 24.0f * std::sin(a + kZunPi / 2.0f), 0.0f};
    const Vec3f F1{24.0f * std::cos(a + kZunPi / 14.0f), 24.0f * std::sin(a + kZunPi / 14.0f), 0.0f};
    const Vec3f F0{24.0f * std::cos(a - kZunPi / 14.0f), 24.0f * std::sin(a - kZunPi / 14.0f), 0.0f};
    auto place = [&](float t) {
        optionPos_[1] = p + (B + (F1 - B) * t);
        optionPos_[0] = p + (B * -1.0f + (F0 + B) * t);
    };
    // The reversal checks come before the counter step here.
    enum class Rule { None, Focusing, Unfocusing };
    Rule rule = Rule::None;
    switch (options_) {
        case Options::Unfocused:
            if (focused_) {
                options_ = Options::Focusing;
                spawnFocusEffect(w);
                rule = Rule::Focusing;
            } else {
                optionPos_[0] = p - B;
                optionPos_[1] = p + B;
            }
            break;
        case Options::Focusing: rule = Rule::Focusing; break;
        case Options::Focused:
            optionCounter_ = 0;
            if (!focused_) {
                options_ = Options::Unfocusing;
                interruptFocusEffect();
                rule = Rule::Unfocusing;
            } else {
                optionPos_[1] = p + F1;
                optionPos_[0] = p + F0;
            }
            break;
        case Options::Unfocusing: rule = Rule::Unfocusing; break;
    }
    if (rule == Rule::Focusing && !focused_) {
        options_ = Options::Unfocusing;
        optionCounter_ = 8 - optionCounter_;
        interruptFocusEffect();
        rule = Rule::Unfocusing;
    } else if (rule == Rule::Unfocusing && focused_) {
        options_ = Options::Focusing;
        optionCounter_ = 8 - optionCounter_;
        spawnFocusEffect(w);
        rule = Rule::Focusing;
    }
    if (rule == Rule::Focusing) {
        ++optionCounter_;
        place(static_cast<float>(optionCounter_) / 8.0f);
        if (optionCounter_ >= 8) options_ = Options::Focused;
    } else if (rule == Rule::Unfocusing) {
        ++optionCounter_;
        place(1.0f - static_cast<float>(optionCounter_) / 8.0f);
        if (optionCounter_ >= 8) options_ = Options::Unfocused;
    }
}

// ---- shots -------------------------------------------------------------------------------

void Player::commonShotInit(const World& w, Shot& s, const ShotEntry& e) {
    const Vec3f origin = e.origin == 1 ? optionPos_[0] : e.origin == 2 ? optionPos_[1] : pos_;
    s.pos = {origin.x + e.offsetX, origin.y + e.offsetY, 0.495f};
    s.boxW = e.boxW;
    s.boxH = e.boxH;
    s.angle = e.angle;
    s.speed = e.speed;
    s.velocity = {std::cos(e.angle) * e.speed, std::sin(e.angle) * e.speed, 0.0f};
    s.age.reset();
    s.age.assign(0);
    s.behaviour = e.behaviour;
    s.damage = e.damage;
    w.playSound(e.sound);
    s.vm.start(*w.anm, *w.rng, e.script);
}

bool Player::fire(const World& w, Shot& s, int slot, const ShotEntry& e, int cycle) {
    auto periodic = [&]() { return e.interval > 0 && cycle % e.interval == e.phase; };
    switch (e.fireKind) {
        case 2:
        case 3: {
            const int k = std::clamp<int>(e.phase, 0, 2);
            LaserSlot& ls = lasers_[static_cast<std::size_t>(k)];
            if (ls.shot >= 0) {
                if (ls.entry != &e) {
                    shotSlots_[static_cast<std::size_t>(ls.shot)].vm.interrupt(1);
                    ls.shot = -1;
                }
                return false;
            }
            if (options_ != (e.fireKind == 2 ? Options::Unfocused : Options::Focused)) return false;
            ls.countdown = e.fireKind == 2 ? e.interval : 999;
            ls.shot = slot;
            s.laserSlot = k;
            s.option = e.origin == 2 ? 1 : 0;
            s.offsetX = e.offsetX;
            s.offsetY = e.offsetY;
            commonShotInit(w, s, e);
            ls.entry = &e;
            if (e.fireKind == 3) {
                s.trailLength = std::min<int>(e.interval, 16);
                for (Vec3f& t : s.trail) t.x = -999.0f;
                s.pos.x = -999.0f;
            }
            return true;
        }
        case 4:
            if (!periodic()) return false;
            commonShotInit(w, s, e);
            if (sakuyaTarget_.x > -100.0f) {
                s.angle = addNormalizeAngle(std::atan2(sakuyaTarget_.y - s.pos.y, sakuyaTarget_.x - s.pos.x), e.angle + kZunPi / 2.0f);
                s.velocity = {std::cos(s.angle) * e.speed * 1.5f, std::sin(s.angle) * e.speed * 1.5f, 0.0f};
            }
            return true;
        case 5:
            if (!periodic()) return false;
            commonShotInit(w, s, e);
            s.angle = addNormalizeAngle(optionAngle_, e.angle + kZunPi / 2.0f);
            s.velocity = {std::cos(s.angle) * e.speed, std::sin(s.angle) * e.speed, 0.0f};
            return true;
        default:
            if (!periodic()) return false;
            commonShotInit(w, s, e);
            return true;
    }
}

void Player::spawnShots(const World& w, int cycle) {
    const ShotFile& file = focused_ ? shots_.focused : shots_.unfocused;
    if (file.levels.empty()) return;
    const std::vector<ShotEntry>& list = file.entriesFor(w.state->powerI());
    if (list.empty()) return;  // the original would evaluate the terminator once
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

void Player::fireCycle(const World& w) {
    if (!firing_) return;
    if (ticked(fireCycle_) && !(bombActive_ && shotIndex_ == 3)) spawnShots(w, fireCycle_.current);
    fireCycle_.tick();
    if (fireCycle_.current >= 30 || dead()) {
        firing_ = false;
        fireCycle_.assign(-1);
    }
}

bool Player::runShotUpdate(const World& w, Shot& s, int slot) {
    const GameState& g = *w.state;
    switch (s.updateKind) {
        case 1:
        case 2: {
            if (s.state != 1) return true;
            const float cap = s.updateKind == 1 ? 10.0f : 18.0f;
            const float inc = s.updateKind == 1 ? 1.0f / 3.0f : 0.6f;
            if (target_.x > -100.0f && s.age.current < 40 && ticked(s.age)) {
                const float dx = target_.x - s.pos.x, dy = target_.y - s.pos.y;
                float l = std::sqrt(dx * dx + dy * dy) / (s.speed / 4.0f);
                if (l < 1.0f) l = 1.0f;
                const float vx = dx / l + s.velocity.x, vy = dy / l + s.velocity.y;
                const float m = std::sqrt(vx * vx + vy * vy);
                s.speed = std::min(m, cap);
                if (s.speed < 1.0f) s.speed = 1.0f;
                s.velocity.x = vx * s.speed / m;
                s.velocity.y = vy * s.speed / m;
            } else if (s.speed < cap) {
                s.speed += inc;
                const float len = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.y * s.velocity.y);
                if (len > 0.0f) {
                    s.velocity.x = s.velocity.x * s.speed / len;
                    s.velocity.y = s.velocity.y * s.speed / len;
                }
            }
            return true;
        }
        case 3:
            if (s.state == 1) s.velocity.y -= w.rng->f32InRange(0.1f) + 0.27f;
            return true;
        case 4:
        case 5: {
            LaserSlot& ls = lasers_[static_cast<std::size_t>(std::clamp(s.laserSlot, 0, 2))];
            if (ls.shot != slot && s.vm.has(AnmVm::Stopped)) s.vm.interrupt(1);
            if ((g.dialogue || bombActive_) && ls.countdown > 20) ls.countdown = 20;
            if (ls.countdown <= 0) {
                ls.countdown = 0;
                ls.shot = -1;
                return false;
            }
            if (ls.countdown <= 70 && s.vm.has(AnmVm::Stopped)) s.vm.interrupt(1);
            if (s.updateKind == 4) {
                const Vec3f& o = optionPos_[static_cast<std::size_t>(s.option & 1)];
                s.pos = {o.x + s.offsetX, o.y, 0.44f};
                if (state_ == State::Dead) return false;
                const float y = s.pos.y;
                s.vm.scale.y = y / 14.0f;
                s.vm.set(AnmVm::ScaleDirty, true);
                s.boxH = y;
                s.pos.y = y / 2.0f;
                return true;
            }
            for (int i = 0; i < s.trailLength; ++i) {
                const Vec3f& t = s.trail[static_cast<std::size_t>(i)];
                if (t.x >= -900.0f) setRegion(96 + i, t, s.boxW, s.boxH, 1);
            }
            for (std::size_t i = s.trail.size() - 1; i > 0; --i) s.trail[i] = s.trail[i - 1];
            s.trail[0] = s.pos;
            if (state_ == State::Dead) return false;
            const float y = pos_.y;
            s.pos = {pos_.x + s.offsetX, y, 0.44f};
            s.vm.scale.y = (y + 64.0f) / 14.0f;
            s.vm.set(AnmVm::ScaleDirty, true);
            s.boxH = y + 64.0f;
            s.pos.y = y / 2.0f - 32.0f;
            return true;
        }
        default: return true;
    }
}

void Player::shotUpdate(const World& w) {
    auto removeLinked = [&](int k, bool fade) {
        LaserSlot& ls = lasers_[static_cast<std::size_t>(k)];
        if (ls.shot < 0) return;
        Shot& s = shotSlots_[static_cast<std::size_t>(ls.shot)];
        if (fade) s.vm.interrupt(1);
        else s.state = 0;
        ls.shot = -1;
    };
    if (options_ != Options::Focused) removeLinked(2, false);
    if (options_ != Options::Unfocused) {
        removeLinked(0, true);
        removeLinked(1, true);
    }
    if (state_ == State::Dead) {
        for (int k = 0; k < 3; ++k) removeLinked(k, false);
    }
    for (LaserSlot& ls : lasers_) {
        if (ls.shot < 0) continue;
        if (ls.countdown > 0 && ls.countdown < 999) --ls.countdown;
        if (!firing_ && ls.countdown > 50) ls.countdown = 50;
        if (ls.countdown == 0) ls.shot = -1;
    }
    for (int i = 0; i < kShots; ++i) {
        Shot& s = shotSlots_[static_cast<std::size_t>(i)];
        if (s.state == 0) continue;
        if (!runShotUpdate(w, s, i)) {
            s.state = 0;
            continue;
        }
        s.pos.x += s.velocity.x * g_timing.speed;
        s.pos.y += s.velocity.y * g_timing.speed;
        if (s.behaviour != 4 && s.behaviour != 5 && s.vm.sprite) {
            const float sw = s.vm.sprite->pixelWidth, sh = s.vm.sprite->pixelHeight;
            if (s.pos.x + sw / 2.0f < 0.0f || s.pos.x - sw / 2.0f > kPlayfieldW || s.pos.y + sh / 2.0f < 0.0f ||
                s.pos.y - sh / 2.0f > kPlayfieldH) {
                s.state = 0;
                continue;
            }
        }
        if (s.vm.tick(*w.anm, *w.rng)) {
            s.state = 0;
            continue;
        }
        s.age.tick();
    }
}

// ---- collisions ---------------------------------------------------------------------------

int Player::freeCancelSlot() const {
    for (int i = 0; i < kCancelRegions - 1; ++i) {
        const CancelRegion& c = cancels_[static_cast<std::size_t>(i)];
        if (c.w == 0.0f && c.radius == 0.0f) return i;
    }
    return kCancelRegions - 1;
}

void Player::cancelCircle(const Vec3f& centre, float radius, float growth, std::int32_t lifetime, int itemType) {
    CancelRegion& c = cancels_[static_cast<std::size_t>(freeCancelSlot())];
    c.centre = centre;
    c.radius = radius;
    c.growth = growth;
    c.lifetime = lifetime;
    c.itemType = itemType;
}

int Player::cancelRect(const Vec3f& centre, float w, float h, int itemType) {
    const int i = freeCancelSlot();
    CancelRegion& c = cancels_[static_cast<std::size_t>(i)];
    c.centre = centre;
    c.w = w;
    c.h = h;
    c.lifetime = 0;
    c.itemType = itemType;
    return i;
}

void Player::setRegion(int i, const Vec3f& centre, float w, float h, std::int32_t damage) {
    DamageRegion& r = regions_[static_cast<std::size_t>(i)];
    r.centre = centre;
    r.w = w;
    r.h = h;
    r.damage = damage;
}

bool Player::cancelTest(const Vec3f& centre, const Vec3f& size) {
    for (const CancelRegion& c : cancels_) {
        if (c.w != 0.0f) {
            if (boxesTouch(centre, size.x / 2.0f, size.y / 2.0f, c.centre, c.w / 2.0f, c.h / 2.0f)) {
                cancelItemType_ = c.itemType;
                return true;
            }
        } else if (c.radius != 0.0f) {
            const float dx = centre.x - c.centre.x, dy = centre.y - c.centre.y;
            if (dx * dx + dy * dy < c.radius * c.radius) {
                cancelItemType_ = c.itemType;
                return true;
            }
        }
    }
    return false;
}

Player::HitResult Player::bulletHit(const World& w, const Vec3f& centre, const Vec3f& size) {
    cancelItemType_ = ItemField::PointBullet;
    if (cancelTest(centre, size)) return HitResult::Cancelled;
    const float hx = size.x / 2.0f, hy = size.y / 2.0f;
    if (centre.x + hx < hitMin_.x || centre.x - hx > hitMax_.x || centre.y + hy < hitMin_.y || centre.y - hy > hitMax_.y) {
        return HitResult::None;
    }
    w.state->eventFlags |= 0x02;
    if (state_ == State::Border) {
        breakBorder(w);
        return HitResult::Hit;
    }
    if (state_ != State::Alive) return HitResult::Hit;
    w.reroll();
    die(w);
    return HitResult::Hit;
}

Player::HitResult Player::bulletGraze(const World& w, const Vec3f& centre, const Vec3f& size) {
    cancelItemType_ = ItemField::PointBullet;
    if (cancelTest(centre, size)) return HitResult::Cancelled;
    if (state_ == State::Dead || state_ == State::Spawning) return HitResult::None;
    const float hx = size.x / 2.0f + 20.0f, hy = size.y / 2.0f + 20.0f;
    if (centre.x + hx < grazeMin_.x || centre.x - hx > grazeMax_.x || centre.y + hy < grazeMin_.y || centre.y - hy > grazeMax_.y) {
        return HitResult::None;
    }
    scoreGraze(w, centre);
    return HitResult::Grazed;
}

Player::HitResult Player::laserTest(const World& w, const Vec3f& origin, float angle, const Vec3f& boxCentre, const Vec3f& boxSize,
                                    bool grazeAllowed) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float dx = pos_.x - origin.x, dy = pos_.y - origin.y;
    const Vec3f p{origin.x + c * dx + s * dy, origin.y + c * dy - s * dx, 0.0f};
    const float hb = header().hitbox / 2.0f;
    if (boxesTouch(p, hb, hb, boxCentre, boxSize.x / 2.0f, boxSize.y / 2.0f)) {
        w.state->eventFlags |= 0x02;
        if (state_ == State::Border) {
            breakBorder(w);
        } else if (state_ == State::Alive) {
            w.reroll();
            die(w);
        }
        return HitResult::Hit;
    }
    if (grazeAllowed && boxesTouch(p, hb, hb, boxCentre, boxSize.x / 2.0f + 48.0f, boxSize.y / 2.0f + 48.0f) &&
        state_ != State::Dead && state_ != State::Spawning) {
        scoreGraze(w, pos_);
        return HitResult::Grazed;
    }
    return HitResult::None;
}

void Player::scoreGraze(const World& w, const Vec3f& at) {
    GameState& g = *w.state;
    if (!bombActive_) {
        if (g.stageGraze < 9999) ++g.stageGraze;
        if (g.totalGraze < 999999) ++g.totalGraze;
    }
    const Vec3f spark = (pos_ + at) * 0.5f;
    if (w.effects) {
        if (border_ == BorderFlag::Active && !focused_) w.effects->spawn(w, 8, spark, 3, 0xFFFF8080u);
        else w.effects->spawn(w, 8, spark, 1, 0xFFFFFFFFu);
    }
    g.increaseSubrank(6);
    w.playSound(30);
    g.grazeBonus += 2500 + (g.cherry / 1500) * 20;
    g.addScore(2000);
    if (border_ == BorderFlag::Active) {
        const std::int32_t a = focused_ ? 30 : 80;
        w.cherryRaiseMax(a);
        w.cherryAdd(a);
    }
}

bool Player::pickupBoxTouches(const Vec3f& item, float half) const {
    return !(item.x + half < pickMin_.x || item.x - half > pickMax_.x || item.y + half < pickMin_.y || item.y - half > pickMax_.y);
}

int Player::damageQuery(const World& w, const Vec3f& centre, const Vec3f& size, bool& bomb) {
    if (!ticked(stateTimer_)) return 0;
    bomb = false;
    const float hw = size.x / 2.0f, hh = size.y / 2.0f;
    int total = 0;
    for (Shot& s : shotSlots_) {
        if (!(s.state == 1 || (s.state == 2 && s.behaviour == 3))) continue;
        if (!boxesTouch(s.pos, s.boxW / 2.0f, s.boxH / 2.0f, centre, hw, hh)) continue;
        if ((s.behaviour == 4 || s.behaviour == 5) && (s.age.current & 1)) continue;
        // The hit kind.
        bool veto = false;
        if (s.hitKind == 1) {
            if (s.state == 2) {
                if (s.age.current & 1) veto = true;
                else {
                    s.damage = static_cast<std::int16_t>(std::max(1, s.damage / 3));
                    s.velocity = s.velocity * 0.88f;
                }
            } else {
                const float a = w.rng->f32InRange(kZunPi / 2.0f) - 3.0f * kZunPi / 4.0f;
                float box = 0.0f, speed = 0.0f;
                switch (s.vm.scriptIndex - kPlayerScripts) {
                    case 65: box = 32, speed = 4; break;
                    case 66: box = 42, speed = 4; break;
                    case 67: box = 48, speed = 4; break;
                    case 68: box = 56, speed = 4; break;
                    case 69: box = 48, speed = 6; break;
                    case 70: box = 64, speed = 6; break;
                    case 71: box = 80, speed = 6; break;
                    case 72: box = 96, speed = 6; break;
                    default: break;
                }
                if (box != 0.0f) {
                    s.boxW = s.boxH = box;
                    s.velocity = {std::cos(a) * speed, std::sin(a) * speed, 0.0f};
                }
            }
            if (!veto && (s.age.current % 6) == 0 && w.effects) w.effects->spawn(w, 5, centre, 1, 0xFFFFFFFFu);
        } else if (s.hitKind == 2) {
            if ((++sparkCounter_ % 8) == 0 && w.effects) w.effects->spawn(w, 5, {s.pos.x, centre.y, 0.0f}, 1, 0xFFFFFFFFu);
        }
        if (veto) continue;
        total += bombActive_ ? std::max(1, s.damage / 3) : s.damage;
        if (s.behaviour != 4 && s.behaviour != 5) {
            if (s.state == 1) {
                if (s.entry) s.vm.start(*w.anm, *w.rng, s.entry->script + 32);
                if (w.effects) w.effects->spawn(w, 5, s.pos, 1, 0xFFFFFFFFu);
                s.pos.z = 0.1f;
            }
            s.state = 2;
            if (s.behaviour != 3) s.velocity = s.velocity / 8.0f;
        }
    }
    for (int i = 0; i < kDamageRegions; ++i) {
        DamageRegion& r = regions_[static_cast<std::size_t>(i)];
        if (r.w <= 0.0f) continue;
        if (!boxesTouch(r.centre, r.w / 2.0f, r.h / 2.0f, centre, hw, hh)) continue;
        total += r.damage;
        r.dealt += r.damage;
        if ((++sparkCounter_ % 4) == 0 && w.effects) w.effects->spawn(w, i < 96 ? 3 : 5, centre, 1, 0xFFFFFFFFu);
        if (bombActive_) bomb = true;
    }
    return total;
}

void Player::offerTarget(const Vec3f& enemy, bool boss) {
    const auto inCone = [&]() {
        const float a = std::atan2(enemy.y - pos_.y, enemy.x - pos_.x);
        return a >= -2.0f * kZunPi / 3.0f && a <= -kZunPi / 3.0f;
    };
    if (boss) {
        if (!targetLocked_ || std::fabs(enemy.x - pos_.x) < std::fabs(target_.x - pos_.x)) target_ = enemy;
        if (shotIndex_ / 2 == 2) {
            if (inCone() && (!targetLocked_ || std::fabs(enemy.x - pos_.x) < std::fabs(sakuyaTarget_.x - pos_.x))) {
                sakuyaTarget_ = enemy;
                targetLocked_ = true;
            }
        } else {
            targetLocked_ = true;
        }
    }
    if (!targetLocked_) {
        if (target_.y < enemy.y) target_ = enemy;
        if (shotIndex_ / 2 == 2 && sakuyaTarget_.y < -900.0f && inCone()) sakuyaTarget_ = enemy;
    }
}

// ---- death, border -----------------------------------------------------------------------

void Player::die(const World& w) {
    w.regenerate();
    if (w.effects) {
        w.effects->spawnSpecial(w, 12, pos_, 3, 0xFF4040FFu);
        w.effects->spawn(w, 6, pos_, 16, 0xFFFFFFFFu);
    }
    state_ = State::Dead;
    stateTimer_.assign(0);
    w.playSound(4);
}

void Player::commitDeath(const World& w) {
    GameState& g = *w.state;
    g.eventFlags |= 0x04;
    g.fullPowerChain = 0;
    g.captureValue = 0;
    g.capturing = false;
    w.addMisses(1.0f);
    if (g.livesI() >= 1) {
        if (g.powerI() <= 16) w.setPower(0.0f);
        else w.addPower(-16.0f);
        if (w.items) {
            w.items->spawn(pos_, ItemField::BigPower, ItemField::Scatter);
            for (int i = 0; i < 5; ++i) w.items->spawn(pos_, ItemField::SmallPower, ItemField::Scatter);
        }
        std::int32_t p = static_cast<std::int32_t>(static_cast<float>(g.cherry) * header().cherryPenalty);
        p = std::min(p, shotIndex_ / 2 == 2 ? 60000 : 100000);
        p -= p % 10;
        g.cherry -= p;
        if (w.items) w.items->stopHoming();
    } else {
        w.setPower(0.0f);
        if (w.items) {
            for (int i = 0; i < 5; ++i) w.items->spawn(pos_, ItemField::FullPower, ItemField::Scatter);
        }
    }
    g.decreaseSubrank(1600);
}

void Player::requestBorder(const World& w) {
    if (bombActive_ || w.state->dialogue) {
        border_ = BorderFlag::Ready;
        return;
    }
    switch (state_) {
        case State::Spawning:
        case State::Invulnerable: border_ = BorderFlag::Ready; break;
        case State::Dead:
            if (respawn_ != 0) breakBorder(w);
            else border_ = BorderFlag::Ready;
            break;
        case State::Alive:
        case State::Border: activateBorder(w); break;
    }
}

void Player::activateBorder(const World& w) {
    stateTimer_.assign(540);
    borderLength_ = 540;
    border_ = BorderFlag::Active;
    state_ = State::Border;
    if (borderEffect_) borderEffect_->inUse = false;
    if (bombRing_) bombRing_->inUse = false;
    bombRing_ = nullptr;
    if (w.effects) borderEffect_ = &w.effects->spawnSpecial(w, 28, pos_, 4, 0xFFFFFFFFu);
    if (w.popups) w.popups->status(PopupSink::SupernaturalBorder, 0);
    w.playSound(32);
    w.playSound(36);
    w.state->eventFlags |= 0x08;
}

void Player::endBorderNaturally(const World& w) {
    GameState& g = *w.state;
    w.cherryRaiseMax(10000);
    w.cherryRaise(10000);
    g.score += static_cast<std::uint32_t>(g.cherry);  // 10 x cherry display points
    if (w.popups) w.popups->status(PopupSink::BorderBonus, static_cast<std::uint32_t>(g.cherry) * 10u);
    g.cherryPlus = 0;
    w.playSound(33);
    state_ = State::Invulnerable;
    stateTimer_.assign(40);
    borderLockout_ = 40;
    border_ = BorderFlag::None;
    if (borderEffect_) borderEffect_->inUse = false;
    borderEffect_ = nullptr;
}

void Player::breakBorder(const World& w) {
    GameState& g = *w.state;
    if (w.effects) borderEffect_ = &w.effects->spawnSpecial(w, 28, pos_, 4, 0xFFFFFFFFu);  // the burst
    g.captureValue = 0;
    g.capturing = false;
    border_ = BorderFlag::None;
    state_ = State::Invulnerable;
    stateTimer_.assign(40);
    borderLockout_ = 40;
    g.cherryPlus = 0;
    cancelCircle(pos_, 32.0f, 16.0f, 50, ItemField::SmallCherry);
    if (w.effects) {
        for (int k = 0; k < 32; ++k) {
            Effect& e = w.effects->spawn(w, 29, pos_, 1, 0xFFFFFFFFu);
            const float a = -kZunPi + static_cast<float>(k) * kZunPi / 16.0f;
            e.direction = {std::cos(a), std::sin(a), 0.0f};
        }
    }
    w.playSound(7);
    w.playSound(33);
    g.eventFlags |= 0x10;
}

// ---- drawing ------------------------------------------------------------------------------

void Player::drawLow(SpriteSink& sink, const AnmDrawState& state) {
    for (Shot& s : shotSlots_) {
        if (s.state != 1) continue;
        if (s.drawKind == 1) {
            for (int i = 0; i < s.trailLength; ++i) {
                const Vec3f& t = s.trail[static_cast<std::size_t>(i)];
                if (t.x < -900.0f) break;
                AnmVm copy = s.vm;
                copy.position = {t.x + kPlayfieldX, t.y + kPlayfieldY, t.z};
                copy.color = withAlpha(copy.color, 255 - 255 * i / std::max(1, s.trailLength));
                drawVm(sink, copy, state);
            }
        }
        if (s.vm.autoRotate != 0) s.vm.rotation.z = s.angle + kZunPi / 2.0f;
        s.vm.position = {s.pos.x + kPlayfieldX, s.pos.y + kPlayfieldY, s.pos.z};
        drawVm(sink, s.vm, state);
    }
}

void Player::drawHigh(SpriteSink& sink, const AnmDrawState& state) {
    if (!continueMenu_) {
        vm_.position = {pos_.x + kPlayfieldX, pos_.y + kPlayfieldY, pos_.z};
        drawVm(sink, vm_, state);
        if (state_ != State::Dead && state_ != State::Spawning) {
            for (int i = 0; i < 2; ++i) {
                optionVms_[static_cast<std::size_t>(i)].position = {optionPos_[static_cast<std::size_t>(i)].x + kPlayfieldX,
                                                                    optionPos_[static_cast<std::size_t>(i)].y + kPlayfieldY, 0.0f};
                drawVm(sink, optionVms_[static_cast<std::size_t>(i)], state);
            }
        }
    }
    for (Shot& s : shotSlots_) {
        if (s.state != 2) continue;
        s.vm.position = {s.pos.x + kPlayfieldX, s.pos.y + kPlayfieldY, s.pos.z};
        drawVm(sink, s.vm, state);
    }
    if (!bombActive_) return;
    // Owners add the VM's own offset to the position (ANM spec).
    for (int i = 0; i < projDrawCount_; ++i) {
        Projectile& p = proj_[static_cast<std::size_t>(i)];
        if (!p.active) continue;
        for (AnmVm& vm : p.vms) {
            if (!vm.sprite) continue;
            vm.position = {p.pos.x + vm.offset.x + kPlayfieldX, p.pos.y + vm.offset.y + kPlayfieldY, p.pos.z};
            drawVm(sink, vm, state);
        }
    }
    for (std::size_t i = 0; i < bombVms_.size(); ++i) {
        AnmVm& vm = bombVms_[i];
        if (!vm.sprite) continue;
        vm.position = {bombAnchor_[i].x + vm.offset.x + kPlayfieldX, bombAnchor_[i].y + vm.offset.y + kPlayfieldY, bombAnchor_[i].z};
        drawVm(sink, vm, state);
    }
}

}  // namespace touhou::th07
