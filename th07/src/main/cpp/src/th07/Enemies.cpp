#include "th07/Enemies.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th07/Background.hpp"
#include "th07/Effects.hpp"
#include "th07/ExeTables.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

namespace {

constexpr std::uint32_t kWhite = 0xFFFFFFFFu;

void rankReset(Enemy& e) {
    e.rankSpeedLow = -0.5f;
    e.rankSpeedHigh = 0.5f;
    e.rankAmount1Low = e.rankAmount1High = e.rankAmount2Low = e.rankAmount2High = 0;
}

}  // namespace

EnemyManager::EnemyManager() { resetTemplate(); }
EnemyManager::~EnemyManager() = default;

// The template every spawn starts from (spec 4.2): zero, with these defaults.
void EnemyManager::resetTemplate() {
    Enemy& t = *template_;
    t = Enemy{};
    t.lasers.fill(-1);
    t.shooter = Shooter{};
    t.shooter.count1 = t.shooter.count2 = 0;
    t.shooter.sound = 7;
    t.shooter.bulletSound = 25;
    t.trail.fill(TrailNode{});
}

// ---- stage start ---------------------------------------------------------------------------

bool EnemyManager::stageStart(const World& w, const Vfs& vfs, TextureLibrary& textures, int stage, std::string* error) {
    if (!w.anm->load(vfs, textures, std::format("data/stg{}enm.anm", stage), kAnmSlot, kScriptBase, error)) return false;
    anmOwned_ = true;
    const std::string name = std::format("data/ecldata{}.ecl", stage);
    const auto bytes = vfs.read(name);
    if (!bytes) {
        if (error) *error = name + ": not in the archive";
        return false;
    }
    std::string why;
    if (!stageStartWith(w, *bytes, &why)) {
        if (error) *error = name + ": " + why;
        return false;
    }
    log::info("Stage {} ECL: {} subs, {} timelines, {} bytes", stage, ecl_.subCount(), ecl_.timelineCount(), ecl_.size());
    return true;
}

bool EnemyManager::stageStartWith(const World& w, std::span<const std::uint8_t> ecl, std::string* error) {
    w_ = &w;
    GameState& g = *w.state;
    loaded_ = false;
    for (Enemy& e : enemies_) {
        e = Enemy{};
        e.active = false;
    }
    resetTemplate();
    bosses_.fill(-1);
    timelines_ = {};
    timelineTimer_ = zeroTimer();
    spellTimer_ = zeroTimer();
    drawCount_.fill(0);
    liveCount_ = 0;
    deadStore_ = 0;
    bombFlag_ = tookDamage_ = false;
    if (!ecl_.load(ecl, error)) return false;
    difficultyBit_ = 1u << std::clamp(g.difficulty, 0, 7);
    stage_ = g.stage;
    // The random-drop counters: two u16 draws, the counter first.
    dropCounter_ = w.rng->u16InRange(3);
    dropIndex_ = w.rng->u16InRange(8);
    spell_ = SpellCard{};
    setSpellActive(0);
    g.boss = BossHud{};
    loaded_ = true;
    return true;
}

void EnemyManager::release(AnmLibrary& anm) {
    if (anmOwned_) anm.release(kAnmSlot);
    anmOwned_ = false;
    ecl_.clear();
    for (Enemy& e : enemies_) e.active = false;
    loaded_ = false;
}

// ---- helpers -------------------------------------------------------------------------------

bool EnemyManager::dialogueCurrent() const { return w_->gui && w_->gui->dialogueCurrent(); }

Enemy* EnemyManager::bossAt(std::int32_t slot) {
    if (slot < 0 || slot >= kBossSlots) {
        eclWarnOnce(std::format("boss slot {} (only 8 exist)", slot));
        return nullptr;
    }
    const std::int32_t i = bosses_[static_cast<std::size_t>(slot)];
    return i >= 0 ? &enemies_[static_cast<std::size_t>(i)] : nullptr;
}

void EnemyManager::setSpellActive(std::uint32_t v) {
    spell_.active = v;
    w().state->spellCardActive = v != 0;
}

void EnemyManager::clampToBounds(Enemy& e) {
    if (!e.bounds) return;
    if (e.pos.x < e.boundsLowX) e.pos.x = e.boundsLowX;
    else if (e.pos.x > e.boundsHighX) e.pos.x = e.boundsHighX;
    if (e.pos.y < e.boundsLowY) e.pos.y = e.boundsLowY;
    else if (e.pos.y > e.boundsHighY) e.pos.y = e.boundsHighY;
}

void EnemyManager::movementStep(Enemy& e) {
    const float m = g_timing.speed;
    e.delta = e.pos - e.prevPos;
    e.prevPos = e.pos;
    if (e.mirror) e.pos.x -= m * e.velocity.x;
    else e.pos.x += m * e.velocity.x;
    e.pos.y += m * e.velocity.y;
    e.pos.z += m * e.velocity.z;
}

bool EnemyManager::onScreen(const Vec3f& p, float wd, float h) const {
    return p.x + wd / 2.0f >= 0.0f && p.x - wd / 2.0f <= kPlayfieldW && p.y + h / 2.0f >= 0.0f && p.y - h / 2.0f <= kPlayfieldH;
}

// ---- spawning ------------------------------------------------------------------------------

Enemy& EnemyManager::spawn(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score, bool mirror) {
    Enemy* slot = &enemies_[kEnemies];
    for (int i = 0; i < kEnemies; ++i) {
        if (!enemies_[static_cast<std::size_t>(i)].active) {
            slot = &enemies_[static_cast<std::size_t>(i)];
            break;
        }
    }
    if (isSpare(*slot)) return *slot;  // nothing spawns
    Enemy& e = *slot;
    e = *template_;
    e.mirror = mirror;
    if (life >= 0) e.life = life;
    e.pos = pos;
    startSub(e, sub);
    if (!runVm(e)) {
        e.active = false;  // despawn is not called
        return e;
    }
    e.color = e.vm.color;
    e.itemDrop = static_cast<std::int8_t>(item);
    if (score >= 0) e.score = score;
    e.maxLife = e.life;  // a time-0 "set life" in the script wins over the record [QUIRK]
    return e;
}

Enemy& EnemyManager::spawnWithContext(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score,
                                      const EclArgs& args) {
    Enemy* slot = &enemies_[kEnemies];
    for (int i = 0; i < kEnemies; ++i) {
        if (!enemies_[static_cast<std::size_t>(i)].active) {
            slot = &enemies_[static_cast<std::size_t>(i)];
            break;
        }
    }
    if (isSpare(*slot)) return *slot;
    Enemy& e = *slot;
    const EclArgs copy = args;
    e = *template_;
    if (life >= 0) e.life = life;
    e.pos = pos;
    startSub(e, sub);
    e.ctx.args = copy;
    if (!runVm(e)) {  // nested inside the spawner's step
        e.active = false;
        return e;
    }
    e.color = e.vm.color;
    e.itemDrop = static_cast<std::int8_t>(item);
    if (life >= 0) e.life = life;  // again, after the first step
    if (score >= 0) e.score = score;
    e.maxLife = e.life;
    return e;
}

// ---- timelines (spec 7) --------------------------------------------------------------------

void EnemyManager::timelineSpawn(const std::uint8_t* r, int op) {
    const World& wd = w();
    if (wd.state->boss.present) return;
    const std::int16_t sub = eclI16(r + 2);
    Vec3f pos{eclF32(r + 8), eclF32(r + 12), eclF32(r + 16)};
    const bool plain = op == 0 || op == 2 || op == 4 || op == 6;
    const std::int32_t life = plain ? eclI32(r + 20) : -1;
    const std::int32_t item = plain ? eclI32(r + 24) : -1;
    const std::int32_t score = plain ? eclI32(r + 28) : -1;
    if (op >= 4) {
        if (pos.x <= -990.0f) pos.x = wd.rng->f32InRange(368.0f);
        if (pos.y <= -990.0f) pos.y = wd.rng->f32InRange(416.0f);
        if (pos.z <= -990.0f) pos.z = wd.rng->f32InRange(800.0f);
    }
    Enemy& e = spawn(sub, pos, life, item, score, op == 2 || op == 3);
    if (op >= 6) e.mirror = true;  // after the first step (the spare slot when full) [QUIRK]
}

void EnemyManager::runTimelines() {
    const World& wd = w();
    for (int i = 0; i < ecl_.timelineCount(); ++i) {
        Timeline& t = timelines_[static_cast<std::size_t>(i)];
        if (!t.started) {
            t.pos = ecl_.timelineOffset(i);
            t.started = true;
        }
        for (;;) {
            if (static_cast<std::uint64_t>(t.pos) + 8 > ecl_.size()) break;
            const std::uint8_t* r = ecl_.at(t.pos);
            const std::int16_t time = eclI16(r);
            if (time < 0) break;
            if (t.clock.current < time) break;
            bool consumed = true;
            if (t.clock.current == time) {
                const std::int16_t op = eclI16(r + 4);
                const std::int16_t arg0 = eclI16(r + 2);
                switch (op) {
                    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: timelineSpawn(r, op); break;
                    case 8:
                        if (wd.gui) wd.gui->startDialogue(arg0 + wd.state->character() * 10);
                        break;
                    case 9:
                        if (wd.gui && wd.gui->dialogueWait()) {
                            t.clock.decrement(1);
                            consumed = false;
                        }
                        break;
                    case 10:
                        if (Enemy* b = bossAt(eclI32(r + 8))) b->pendingInterrupt = eclI32(r + 12);
                        else eclWarnOnce("timeline interrupt for a missing boss (the original writes through a stale pointer)");
                        break;
                    case 11: wd.setPower(static_cast<float>(arg0)); break;  // two u32 draws (anti-tamper)
                    case 12:
                        if (Enemy* b = bossAt(arg0); b && b->active) {
                            t.clock.decrement(1);
                            consumed = false;
                        }
                        break;
                    default: break;
                }
            }
            if (!consumed) break;
            const std::int16_t size = eclI16(r + 6);
            if (size < 8) {
                eclWarnOnce(std::format("timeline {} record of size {} at {:#x}; the timeline stops", i, size, t.pos));
                t.pos = ecl_.size();
                break;
            }
            t.pos += static_cast<std::uint32_t>(size);
        }
        t.clock.tick();
    }
}

// ---- the frame (spec 8.1, 8.2) -------------------------------------------------------------

void EnemyManager::update(const World& w) {
    w_ = &w;
    if (!loaded_) return;
    GameState& g = *w.state;
    if (!dialogueCurrent()) {
        const std::int32_t limit = 2400 - 240 * g.livesI();
        if (hasTicked(timelineTimer_) && limit != 0 && timelineTimer_.current % limit == 0) g.increaseSubrank(100);
        g.completion += 1;
    }
    drawCount_.fill(0);
    runTimelines();
    liveCount_ = 0;
    for (int i = 0; i < kEnemies; ++i) {
        Enemy& e = enemies_[static_cast<std::size_t>(i)];
        if (!e.active) continue;
        ++liveCount_;
        updateEnemy(e, i);
    }
    // (Every 200 frames the original runs an anti-tamper check here; not ported.)
    timelineTimer_.tick();
}

void EnemyManager::updateEnemy(Enemy& e, int index) {
    const World& wd = w();
    GameState& g = *wd.state;
    Player* pl = wd.player;
    const bool frozen = e.freezeBomb && (g.bombActive || (pl && pl->state() != Player::State::Alive));
    if (frozen) {
        e.timer.decrement(1);  // the tick below cancels it
    } else {
        for (int rerun = 0;; ++rerun) {
            if (rerun > 64) {
                eclWarnOnce(std::format("enemy {}: callbacks keep firing in one frame", index));
                break;
            }
            // 3. The VM step.
            if (!runVm(e)) {
                e.active = false;
                despawn(e);
                return;
            }
            // 4. Movement.
            if (!e.moveDisabled) {
                clampToBounds(e);
                movementStep(e);
                clampToBounds(e);
                if (e.special && !e.circleFrozen) e.special->pos += (e.pos - e.special->pos) * 0.0625f;
            }
            // 5. Trail history.
            if (e.trailFlags != 0) {
                const int count = std::clamp<int>(e.trailCount, 0, 96);
                for (int j = count - 1; j >= 1; --j) e.trail[static_cast<std::size_t>(j)] = e.trail[static_cast<std::size_t>(j - 1)];
                e.trail[0] = {e.pos, e.velocity, e.angle};
            }
            // 6-8. Screen bounds (a missing sprite counts as size 0; the original would crash).
            if (!e.vm.sprite) e.noCollision = true;  // permanent [QUIRK]
            const float sw = e.vm.sprite ? e.vm.sprite->pixelWidth : 0.0f;
            const float sh = e.vm.sprite ? e.vm.sprite->pixelHeight : 0.0f;
            if (!e.noCollision && !e.inBounds && onScreen(e.pos, sw, sh)) e.inBounds = true;
            if (e.inBounds && !e.oobDisabled && !onScreen(e.pos, sw, sh)) {
                const int last = std::clamp<int>(e.trailCount, 0, 96) - 1;
                if (e.trailFlags == 0 || last < 0 || !onScreen(e.trail[static_cast<std::size_t>(last)].pos, sw, sh)) {
                    e.active = false;
                    despawn(e);
                    return;
                }
            }
            // 9-10. Callbacks run the VM again in the same frame [QUIRK].
            if (lifeCallback(e)) continue;
            if (e.timerThreshold >= 0 && timerCallback(e)) continue;
            break;
        }
        // 11. ANM.
        e.vm.color = e.color;
        e.vm.tick(*wd.anm, *wd.rng);
        e.color = e.vm.color;
        for (std::size_t k = 0; k < 2; ++k) {
            if (e.vm2On[k] && e.vms2[k].tick(*wd.anm, *wd.rng)) e.vm2On[k] = false;
        }
        // 12-13. Collision.
        bombFlag_ = false;
        tookDamage_ = false;
        if (!e.noCollision && !e.invisibleBomb) {
            if (e.canDie && e.contact) {
                contactTest(e, e.pos, e.hitbox);
                if (e.trailFlags != 0) {
                    for (int j = 1; j < e.trailInterval && j < 96; j += 6) {
                        Vec3f box = e.hitbox;
                        if (e.trailFlags & 2) box = box - divVec(box * static_cast<float>(j), static_cast<float>(e.trailInterval));
                        contactTest(e, e.trail[static_cast<std::size_t>(j)].pos, box);
                    }
                }
            }
            e.damageThisFrame = 0;
            if (e.canDie && e.hittable && pl) {
                shotDamage(e);
                pl->offerTarget(e.pos, e.boss);
            }
        }
        // 14. Death.
        if (e.life <= 0 && e.canDie) death(e, index);
    }
    // 15. The damage tint, from the loop-carried flag (a frozen enemy sees the previous
    // enemy's [QUIRK]).
    if (e.tint != 0) {
        --e.tint;
        e.vm.set(AnmVm::UseAltColor, false);
    } else if (tookDamage_) {
        wd.playSound(20);
        e.vm.altColor = (e.vm.color & 0xFF000000u) | 0x00FF80C0u;
        e.vm.set(AnmVm::UseAltColor, true);
        e.tint = 1;
    } else {
        e.vm.set(AnmVm::UseAltColor, false);
    }
    // 16. Boss HUD.
    if (e.boss) {
        if (!dialogueCurrent() && e.bossId == 0) g.boss.bar = static_cast<float>(e.life) / static_cast<float>(e.maxLife);
        if (e.bossId < 4) {
            g.boss.markerX[e.bossId] = e.noCollision ? -999.0f : e.pos.x + 32.0f;
            g.boss.markerTint[e.bossId] = e.vm.has(AnmVm::UseAltColor);
        }
    }
    // 17-18.
    updateCircles(e);
    e.timer.tick();
    if (e.invincible.current > 0) e.invincible.decrement(1);
    // 19. Draw lists (front insertion: descending slot order).
    if (!e.noCollision && e.active) {
        const std::size_t layer = std::min<std::size_t>(e.drawLayer, 3);
        drawLists_[layer][static_cast<std::size_t>(drawCount_[layer]++)] = static_cast<std::int16_t>(index);
    }
}

// ---- collision and damage (spec 8.5, 8.6) --------------------------------------------------

void EnemyManager::contactTest(Enemy& e, const Vec3f& centre, const Vec3f& box) {
    const World& wd = w();
    Player* pl = wd.player;
    if (!pl) return;
    if (e.projectile && hasTicked(e.timer) && e.timer.current % 6 == 0) pl->bulletGraze(wd, centre, divVec(box, 0.7f));
    if (pl->bulletHit(wd, centre, divVec(box, 1.5f)) == Player::HitResult::Hit && e.canDie && !e.boss && !e.projectile) {
        e.life -= 10;  // also while the player is invulnerable [QUIRK]
    }
}

void EnemyManager::shotDamage(Enemy& e) {
    const World& wd = w();
    Player& pl = *wd.player;
    int d = pl.damageQuery(wd, e.pos, e.hitbox, bombFlag_);
    if (e.box2.x > 0.0f) {
        const int d2 = pl.damageQuery(wd, e.pos, e.box2, bombFlag_);  // replaces the bomb flag
        if (!bombFlag_) d = x86ToInt(static_cast<float>(d) + static_cast<float>(d2) / 2.5f);
    }
    if (d > 0) applyDamage(e, d);
}

void EnemyManager::applyDamage(Enemy& e, int d) {
    const World& wd = w();
    GameState& g = *wd.state;
    const bool focused = wd.player->focused();
    tookDamage_ = true;
    if ((e.boss || !focused) && !g.bombActive) {
        const int f = g.stage >= 5 ? 10 : 2 * g.stage;
        int gain = e.boss && !focused ? d / (10 - f / 3) * 10 : d / (30 - f) * 10;
        if (gain > 70) gain = 70;
        const bool odd = e.timer.current % 2 != 0;
        if (gain == 0 && (!focused || odd)) gain = 10;
        if (g.shotIndex == 0) {  // Reimu A, only on this branch [QUIRK]
            if ((gain == 20 || gain == 30) && odd) gain -= 10;
            if ((g.stage == 5 || g.stage == 6) && !e.boss) d /= 2;
            if (g.stage == 4 && !e.boss) d -= d / 4 + d / 16;
        }
        if (gain != 0) wd.cherryAddWithGauge(gain);
    }
    if (d >= 70) d = 70;
    g.addScore(static_cast<std::uint32_t>(d / 5 * 10));
    if (!e.damageable) return;
    if (spell_.active != 0) {
        if (!bombFlag_) d = d > 7 ? d / 7 : (d != 0 ? 1 : 0);
        else if (g.bombUsedInCard) d = d > 2 ? x86ToInt(static_cast<float>(d) / 2.5f) : (d != 0 ? 1 : 0);
        else d = 0;
    }
    if (e.invincible.current > 0) d = e.boss ? d / 9 : 0;
    e.life -= d;
    e.damageThisFrame = d;
}

// ---- callbacks (spec 8.7, 8.8) -------------------------------------------------------------

void EnemyManager::resetForCallback(Enemy& e) {
    rankReset(e);
    e.depth = 0;
    e.shooter = template_->shooter;
    e.shootInterval = 0;
}

// Every active non-boss enemy (the caller too) gets life 0; those that cannot die run their
// death sub.
void EnemyManager::killAllSimple() {
    for (int i = 0; i < kEnemies; ++i) {
        Enemy& o = enemies_[static_cast<std::size_t>(i)];
        if (!o.active || o.boss) continue;
        o.life = 0;
        if (!o.canDie && o.deathSub >= 0) {
            startSub(o, o.deathSub);
            o.deathSub = -1;
        }
    }
}

bool EnemyManager::lifeCallback(Enemy& e) {
    for (std::size_t i = 0; i < 4; ++i) {
        const std::int32_t threshold = e.lifeThresholds[i];
        if (threshold < 0 || e.life >= threshold) continue;
        e.life = threshold;
        startSub(e, e.lifeSubs[i]);
        e.lifeThresholds[i] = -1;
        e.timerThreshold = -1;  // cancelled, not re-armed
        e.periodicSub = -1;
        resetForCallback(e);
        killAllSimple();
        return true;
    }
    return false;
}

bool EnemyManager::timerCallback(Enemy& e) {
    const World& wd = w();
    GameState& g = *wd.state;
    if (e.boss && e.bossId == 0) g.boss.spellSeconds = (e.timerThreshold - e.timer.current) / 60;
    if (e.timer.current < e.timerThreshold) return false;
    int best = -1;
    for (int i = 0; i < 4; ++i) {
        const std::int32_t t = e.lifeThresholds[static_cast<std::size_t>(i)];
        if (t > 0 && (best < 0 || t > e.lifeThresholds[static_cast<std::size_t>(best)])) best = i;
    }
    if (best >= 0) {
        e.life = e.lifeThresholds[static_cast<std::size_t>(best)];
        e.lifeThresholds[static_cast<std::size_t>(best)] = -1;
    }
    startSub(e, e.timerSub);
    e.timerThreshold = -1;
    e.timerSub = e.deathSub;
    e.timer.assign(0);
    if (!e.survival) {
        // A timed-out card, but also every other timer callback [QUIRK].
        g.captureValue = 0;
        g.capturing = false;
        if (spell_.active != 0) setSpellActive(spell_.active + 1);
        if (wd.bulletManager) wd.bulletManager->clearAll(wd, 10);
        std::uint32_t p = x86ToU32(static_cast<float>(g.cherry) * 0.25f);
        p -= p % 10;
        g.cherry -= static_cast<std::int32_t>(p);
    }
    killAllSimple();
    e.periodicSub = -1;
    resetForCallback(e);
    return true;
}

// ---- death (spec 8.9, 8.11, 8.13) ----------------------------------------------------------

void EnemyManager::releaseCircles(Enemy& e) {
    for (int k = 0; k < e.circleCount; ++k) {
        if (Effect* c = e.circles[static_cast<std::size_t>(k)]) c->fading = true;
        e.circles[static_cast<std::size_t>(k)] = nullptr;
    }
    e.circleCount = 0;
}

void EnemyManager::updateCircles(Enemy& e) {
    for (int k = 0; k < e.circleCount; ++k) {
        Effect* c = e.circles[static_cast<std::size_t>(k)];
        if (!c) continue;
        c->vm.set(AnmVm::Visible, !e.noCollision);
        c->emitter = e.pos;
        if (c->radius < e.circleRadius) c->radius += 0.3f;
        c->angularVelocity = addNormalizeAngle(c->angularVelocity, kZunPi / 100.0f);
    }
}

std::uint32_t EnemyManager::killAllScoring(std::uint32_t max, std::uint32_t total) {
    const World& wd = w();
    std::uint32_t popup = 2000;
    auto bonusAt = [&](const Vec3f& at) {
        if (wd.items) wd.items->spawn(at, ItemField::PointBullet, ItemField::AutoCollect);
        if (wd.popups) wd.popups->score(at, popup, popup >= max ? 0xFFFFFF00u : 0xFFFFFFFFu, false);
        total += popup;
        popup = std::min(popup + 30, max);
    };
    for (int i = 0; i < kEnemies; ++i) {
        Enemy& o = enemies_[static_cast<std::size_t>(i)];
        if (!o.active || o.boss) continue;
        o.life = 0;
        if (o.projectile) {
            bonusAt(o.pos);
            if (o.trailFlags != 0) {
                for (int j = 0; j < o.trailCount && j < 96; j += 6) bonusAt(o.trail[static_cast<std::size_t>(j)].pos);
            }
        }
        if (!o.canDie && o.deathSub >= 0) {
            startSub(o, o.deathSub);
            o.deathSub = -1;
        }
    }
    return total;
}

void EnemyManager::itemDropAndBonus(Enemy& e) {
    const World& wd = w();
    GameState& g = *wd.state;
    const int state = bombFlag_ ? ItemField::AutoCollect : ItemField::Normal;
    const int spark = e.death2 + 4;
    if (e.itemDrop >= 0) {
        if (wd.effects) wd.effects->spawn(wd, spark, e.pos, 3, kWhite);
        if (wd.items) wd.items->spawn(e.pos, e.itemDrop, state);
    } else if (e.itemDrop == -1) {
        if (dropCounter_ % 3 == 0) {
            if (wd.effects) wd.effects->spawn(wd, spark, e.pos, 6, kWhite);
            const int type = wd.tables ? wd.tables->dropCycle[dropIndex_ % ExeTables::kDrops] : static_cast<int>(ItemField::SmallPower);
            if (wd.items) wd.items->spawn(e.pos, type, state);
            dropIndex_ = static_cast<std::uint16_t>((dropIndex_ + 1) % ExeTables::kDrops);
        }
        ++dropCounter_;
    }
    if (e.boss && spell_.active == 0) {
        std::uint32_t s = wd.bulletManager ? wd.bulletManager->bonusClear(wd, 8000, true) : 0;
        s = killAllScoring(8000, s);
        if (s != 0) {
            g.addScore(s);
            if (wd.gui) wd.gui->bonus(s);
        }
    }
    e.life = 0;
    g.eventFlags |= 0x20;
}

void EnemyManager::death(Enemy& e, int index) {
    const World& wd = w();
    GameState& g = *wd.state;
    e.lifeThresholds.fill(-1);
    e.timerThreshold = -1;
    e.periodicSub = -1;
    auto bossCleanup = [&]() {
        if (!e.boss) return;
        g.boss.present = false;
        releaseCircles(e);
    };
    switch (e.deathType) {
        case 3:
            e.life = 1;
            e.damageable = false;
            e.deathType = 0;
            g.boss.present = false;
            g.eventFlags |= 0x20;
            if (e.death1 >= 0 && wd.effects) {
                for (int k = 0; k < 3; ++k) wd.effects->spawn(wd, e.death1, e.pos, 1, kWhite);
            }
            break;
        case 1:
            g.addScore(static_cast<std::uint32_t>(e.score));
            e.canDie = false;
            bossCleanup();
            itemDropAndBonus(e);
            break;
        case 0:
            g.addScore(static_cast<std::uint32_t>(e.score));
            e.active = false;
            bossCleanup();
            itemDropAndBonus(e);
            break;
        case 2: itemDropAndBonus(e); break;  // stays active and can die again next frame [QUIRK]
        default: break;
    }
    wd.playSound(2 + index % 2);
    if (e.death1 >= 0 && wd.effects) {
        wd.effects->spawn(wd, e.death1, e.pos, 1, kWhite);
        wd.effects->spawn(wd, e.death2 + 4, e.pos, 4, kWhite);
    }
    if (e.deathSub >= 0) {
        // Its first instructions run at the enemy's next VM step.
        rankReset(e);
        e.depth = 0;
        e.lifeThresholds.fill(-1);
        e.timerThreshold = -1;
        e.periodicSub = -1;
        e.shooter = template_->shooter;
        e.shootInterval = 0;
        startSub(e, e.deathSub);
        e.deathSub = -1;
    }
}

void EnemyManager::removeAll() {
    if (!w_) return;
    GameState& g = *w().state;
    for (int i = 0; i < kEnemies; ++i) {
        Enemy& e = enemies_[static_cast<std::size_t>(i)];
        if (!e.active) continue;
        if (e.circleCount) releaseCircles(e);
        if (e.boss) {
            if (e.bossId < 4) g.boss.present = false;
            if (e.bossId < kBossSlots) bosses_[e.bossId] = -1;
        }
        e.active = false;
    }
}

void EnemyManager::despawn(Enemy& e) {
    GameState& g = *w().state;
    if (e.deathType == 0) e.active = false;
    else e.canDie = false;
    if (e.boss && e.bossId < 4) g.boss.present = false;
    if (e.circleCount) releaseCircles(e);
    if (e.boss && e.bossId < kBossSlots) bosses_[e.bossId] = -1;
    g.eventFlags |= 0x20;
}

// ---- spell cards (spec 11) -----------------------------------------------------------------

void EnemyManager::spellCardStart(Enemy& e, const std::uint8_t* ins) {
    const World& wd = w();
    GameState& g = *wd.state;
    const std::int16_t portrait = eclI16(ins + 12);
    const std::uint16_t number = eclU16(ins + 14);
    std::string name;
    for (int i = 0; i < 48; ++i) {
        const auto c = static_cast<char>(ins[16 + i] ^ 0xAA);
        if (c == 0) break;
        name.push_back(c);
    }
    if (wd.gui) wd.gui->spellCardStart(number, portrait, name);
    if (wd.bulletManager) wd.bulletManager->clearAll(wd, 1);
    if (wd.background) wd.background->spellStart(*wd.anm, *wd.rng);
    setSpellActive(1);
    g.capturing = true;
    spell_.number = number;
    if (number >= ExeTables::kSpellCards) eclWarnOnce(std::format("spell card {} has no base value (141 cards)", number));
    spell_.base = wd.tables && number < ExeTables::kSpellCards ? wd.tables->spellBonus[number] : 0;
    g.captureValue = static_cast<std::int32_t>(spell_.base);
    g.grazeBonus = 0;
    const std::int32_t divisor = e.timerThreshold / 60 + 10;
    spell_.drain = divisor != 0 ? g.captureValue / divisor : 0;
    spellTimer_.assign(0);
    rankReset(e);
    if (wd.effects) {
        Effect& s = wd.effects->spawnSpecial(wd, 25, e.pos, 1, kWhite);
        s.vm.progress[AnmVm::Scale].reset();
        s.vm.duration[AnmVm::Scale].assign(e.timerThreshold);
        s.vm.ease[AnmVm::Scale] = 0;
        s.vm.scaleStart = s.vm.scale;
        s.vm.scaleEnd = {0.125f, 0.125f};
        s.pos = e.pos;
        e.special = &s;
    }
    e.circleFrozen = false;
    log::info("ECL: spell card {} (frame {}, base {}, time {}, drain {})", number, g.stageFrame, spell_.base, e.timerThreshold,
              spell_.drain);
    // The card's score.dat history entry (attempts), outside replays (spec 11.2 step 8).
    if (wd.onSpellDeclared) wd.onSpellDeclared(number, name);
}

void EnemyManager::spellCardEnd() {
    const World& wd = w();
    GameState& g = *wd.state;
    if (spell_.active != 0) {
        log::info("ECL: spell card {} ends (frame {}): {}", spell_.number, g.stageFrame,
                  spell_.active != 1 ? "timed out" : g.capturing ? std::format("captured, {}", g.captureValue + g.grazeBonus) : "failed");
        if (wd.gui) wd.gui->spellCardEnd();
        if (spell_.active == 1) {
            std::uint32_t s = wd.bulletManager ? wd.bulletManager->bonusClear(wd, 8000, true) : 0;
            s = killAllScoring(8000, s);
            if (s != 0) {
                g.addScore(s);
                if (wd.gui) wd.gui->bonus(s);
            }
            if (g.capturing) {
                const auto bonus = static_cast<std::uint32_t>(g.captureValue + g.grazeBonus);
                if (wd.gui) wd.gui->spellCardBonus(bonus);
                g.addScore(bonus);
                // The card's score.dat history entry (best bonus, captures), outside replays
                // (spec 11.5).
                if (wd.onSpellCaptured) wd.onSpellCaptured(spell_.number, bonus);
                ++g.spellsCaptured;
            }
        }
        setSpellActive(0);
        for (std::int32_t b : bosses_) {
            if (b < 0) continue;
            Enemy& boss = enemies_[static_cast<std::size_t>(b)];
            if (boss.special) boss.special->inUse = false;
            boss.special = nullptr;
        }
        wd.playSound(15);
    }
    if (wd.background) wd.background->spellEnd();
}

// ---- drawing (spec 8.15; presentation only) ------------------------------------------------

void EnemyManager::drawEnemy(SpriteSink& sink, Enemy& e, const AnmDrawState& state) {
    const float x = e.pos.x + kPlayfieldX, y = e.pos.y + kPlayfieldY;
    auto place = [&](AnmVm& vm, bool autoRotate, float rot, float z) {
        if (autoRotate) {
            vm.rotation.z = rot;
            vm.set(AnmVm::RotationDirty, true);
        }
        vm.position = {x + vm.offset.x, y + vm.offset.y, z};
        drawVm(sink, vm, state);
    };
    if (e.vm2On[0]) place(e.vms2[0], e.vms2[0].autoRotate != 0, e.angle, 0.29f);
    if (!(e.trailFlags & 16) && !e.invisibleBomb) place(e.vm, e.autoRotate, e.angle, 0.29f);
    if (e.vm2On[1]) place(e.vms2[1], e.vms2[1].autoRotate != 0, -e.angle, 0.3f);  // -angle [QUIRK]
    // The trail (spec_enemy_trails): drawn whenever the flags are non-zero (bit 1 is never
    // tested [QUIRK]), even while the primary is invisible during a bomb.
    if (e.trailFlags != 0) {
        if (e.trailFlags & 8) drawTrailStrip(sink, e);
        else drawTrailCopies(sink, e, state);
    }
}

namespace {

// Mid(a, b) of the trail spec 3.3, with the original's wrong far-side branch [QUIRK].
float trailMid(float a, float b) {
    const float lo = std::min(a, b), hi = std::max(a, b);
    const float d = hi - lo, w = (lo + kZunTwoPi) - hi;
    return d < w ? d * 0.5f + lo : w * 0.5f + lo;
}

}  // namespace

// The sprite-copy trail (flag 8 clear; spec_enemy_trails 4): copies of the primary at
// entries S, 2S, ... (unfilled entries skipped), newest first, through the ordinary 2D draw.
void EnemyManager::drawTrailCopies(SpriteSink& sink, Enemy& e, const AnmDrawState& state) {
    const int count = std::clamp<int>(e.trailCount, 0, 96);
    const int step = std::max<int>(1, e.nodeStep);
    const Vec2f scale0 = e.vm.scale;
    const std::uint32_t color0 = e.vm.color;
    const auto a = static_cast<std::int32_t>(color0 >> 24);
    for (int j = step; j < count; j += step) {
        const TrailNode& n = e.trail[static_cast<std::size_t>(j)];
        if (n.pos.x < -990.0f) continue;
        AnmVm& vm = e.vm;
        if (e.autoRotate) {
            vm.rotation.z = n.angle;
            vm.set(AnmVm::RotationDirty, true);
        }
        if (e.trailFlags & 2) vm.scale.x = scale0.x - (static_cast<float>(j) * scale0.x) / static_cast<float>(count);
        if (e.trailFlags & 4) {
            const std::int32_t faded = a - (a * j) / count;
            vm.color = (color0 & 0x00FFFFFFu) | (static_cast<std::uint32_t>(faded) << 24);
        }
        vm.position = {n.pos.x + vm.offset.x + kPlayfieldX, n.pos.y + vm.offset.y + kPlayfieldY, 0.3f};
        drawVm(sink, vm, state);
    }
    e.vm.scale = scale0;
    e.vm.color = color0;
}

// The triangle-strip trail (flag 8; spec_enemy_trails 3): two vertices per node j = 0, S,
// 2S, ... (stopping at the first unfilled entry), across the recorded angle, the sprite
// stretched along the whole current length. Literal playfield origin, no shake [QUIRK]; the
// primary's ordinary colour; drawn as consecutive quads (the same triangles as the strip).
void EnemyManager::drawTrailStrip(SpriteSink& sink, Enemy& e) {
    const AnmVm& vm = e.vm;
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    const int count = std::clamp<int>(e.trailCount, 0, 96);
    const int step = std::max<int>(1, e.nodeStep);
    if (e.trailCount > 96 || e.nodeStep < 1) eclWarnOnce(std::format("trail count {} / node step {} clamped", e.trailCount, e.nodeStep));
    int nodes = 0;
    for (int j = 0; j < count && e.trail[static_cast<std::size_t>(j)].pos.x >= -990.0f; j += step) ++nodes;
    if (nodes < 2) return;
    const AnmSprite& sp = *vm.sprite;
    const float uStep = (sp.uvEndX - sp.uvStartX) / static_cast<float>(nodes - 1);
    float u = sp.uvEndX + vm.uvScroll.x;
    const float vA = sp.uvStartY + vm.uvScroll.y, vB = sp.uvEndY + vm.uvScroll.y;
    const float h0 = vm.scale.y * sp.pixelHeight / 2.0f;
    const std::uint32_t color0 = vm.color;
    const auto a = static_cast<std::int32_t>(color0 >> 24);
    const Blend blend = vm.has(AnmVm::Additive) ? Blend::Additive : Blend::Alpha;
    auto toNdc = [](float x, float y, Vertex3D& v) {  // Direct3D pixels -> GL (+0.5, ANM 4.9)
        v.x = (x + 0.5f) / 640.0f * 2.0f - 1.0f;
        v.y = 1.0f - (y + 0.5f) / 480.0f * 2.0f;
        v.z = -1.0f;
        v.w = 1.0f;
    };
    Draw3DState ds;
    ds.depthWrite = false;
    ds.clipX = kPlayfieldX;
    ds.clipY = kPlayfieldY;
    ds.clipW = kPlayfieldW;
    ds.clipH = kPlayfieldH;
    Vertex3D prev[2]{};
    bool havePrev = false;
    float prevTheta = 0.0f;
    for (int k = 0, j = 0; k < nodes; ++k, j += step, u -= uStep) {
        const TrailNode& n = e.trail[static_cast<std::size_t>(j)];
        const float theta = j == 0 ? n.angle : trailMid(e.trail[static_cast<std::size_t>(j - 1)].angle, n.angle);
        float h = h0;
        if (e.trailFlags & 2) {
            h *= 1.0f - static_cast<float>(j) / static_cast<float>(count);
            // A node in a straight run is dropped (its u slot still used) [QUIRK: entry S].
            if (j > 0 && j + step < count && havePrev) {
                const float next = trailMid(e.trail[static_cast<std::size_t>(j + step - 1)].angle, e.trail[static_cast<std::size_t>(step)].angle);
                if (std::fabs(prevTheta - theta) < 0.00001f && std::fabs(theta - next) < 0.00001f) continue;
            }
        }
        const auto s = static_cast<float>(std::sin(static_cast<double>(theta)));
        const auto c = static_cast<float>(std::cos(static_cast<double>(theta)));
        std::uint32_t color = color0;
        if (e.trailFlags & 4) color = (color0 & 0x00FFFFFFu) | (static_cast<std::uint32_t>(a - (a * j) / count) << 24);
        Vertex3D cur[2];
        toNdc(n.pos.x + ((c * 0.0f - s * h) + kPlayfieldX), n.pos.y + ((s * 0.0f + c * h) + kPlayfieldY), cur[0]);
        toNdc(n.pos.x + ((c * 0.0f + s * h) + kPlayfieldX), n.pos.y + ((s * 0.0f - c * h) + kPlayfieldY), cur[1]);
        cur[0].u = cur[1].u = u;
        cur[0].v = vA;
        cur[1].v = vB;
        cur[0].color = cur[1].color = argbToRgbaBytes(color);
        if (havePrev) {
            const Vertex3D quad[4] = {prev[0], prev[1], cur[0], cur[1]};
            sink.drawQuad3D(sp.texture, blend, quad, ds);
        }
        prev[0] = cur[0];
        prev[1] = cur[1];
        havePrev = true;
        prevTheta = theta;
    }
}

void EnemyManager::drawLists(SpriteSink& sink, const AnmDrawState& state, int first) {
    if (!loaded_) return;
    for (int layer = first; layer < first + 2; ++layer) {
        const auto l = static_cast<std::size_t>(layer);
        for (int k = drawCount_[l] - 1; k >= 0; --k) {
            Enemy& e = enemies_[static_cast<std::size_t>(drawLists_[l][static_cast<std::size_t>(k)])];
            if (e.active) drawEnemy(sink, e, state);
        }
    }
}

void EnemyManager::drawLow(SpriteSink& sink, const AnmDrawState& state) { drawLists(sink, state, 0); }
void EnemyManager::drawHigh(SpriteSink& sink, const AnmDrawState& state) { drawLists(sink, state, 2); }

}  // namespace touhou::th07
