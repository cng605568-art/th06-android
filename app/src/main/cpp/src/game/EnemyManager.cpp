#include "game/EnemyManager.hpp"

#include <bit>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>

#include "game/EffectManager.hpp"
#include "game/Gui.hpp"
#include "game/StageBackground.hpp"
#include "game/ItemManager.hpp"
#include "game/ScoreFile.hpp"
#include "game/Player.hpp"

namespace th06 {
namespace {

// Variable ids of ECL arguments (EclVarId).
enum : std::int32_t {
    VarI0 = -10001, VarI1 = -10002, VarI2 = -10003, VarI3 = -10004,
    VarF0 = -10005, VarF1 = -10006, VarF2 = -10007, VarF3 = -10008,
    VarIC0 = -10009, VarIC1 = -10010, VarIC2 = -10011, VarIC3 = -10012,
    VarDifficulty = -10013, VarRank = -10014,
    VarSelfX = -10015, VarSelfY = -10016, VarSelfZ = -10017,
    VarPlayerX = -10018, VarPlayerY = -10019, VarPlayerZ = -10020,
    VarPlayerAngle = -10021, VarEnemyTimer = -10022, VarPlayerDistance = -10023,
    VarEnemyLife = -10024, VarPlayerShot = -10025,
};

constexpr int kMaxInstructionsPerTick = 20000;
constexpr int kAnmScriptEnemyStart = kAnmOffsetEnemy;
constexpr int kSound2 = 2;
constexpr int kSoundTotalBossDeath = 20;
constexpr int kItemRandom = kItemDropRandom;

// Random drops: every third enemy with drop code -1 yields the next entry of this table.
constexpr std::int8_t kRandomItems[32] = {
    ItemPowerSmall, ItemPowerSmall, ItemPoint,      ItemPowerSmall, ItemPoint,      ItemPowerSmall, ItemPowerSmall, ItemPoint,
    ItemPoint,      ItemPoint,      ItemPowerSmall, ItemPowerSmall, ItemPowerSmall, ItemPoint,      ItemPoint,      ItemPowerSmall,
    ItemPoint,      ItemPowerSmall, ItemPoint,      ItemPowerSmall, ItemPoint,      ItemPowerSmall, ItemPoint,      ItemPowerSmall,
    ItemPoint,      ItemPowerSmall, ItemPowerSmall, ItemPoint,      ItemPoint,      ItemPoint,      ItemPowerSmall, ItemPowerBig,
};

float f32Bits(std::int32_t v) { return std::bit_cast<float>(v); }
std::int32_t i32Bits(float v) { return std::bit_cast<std::int32_t>(v); }

}  // namespace

void Enemy::clampPos() {
    if (!shouldClampPos) return;
    if (position.x < lowerMoveLimitX) position.x = lowerMoveLimitX;
    else if (position.x > upperMoveLimitX) position.x = upperMoveLimitX;
    if (position.y < lowerMoveLimitY) position.y = lowerMoveLimitY;
    else if (position.y > upperMoveLimitY) position.y = upperMoveLimitY;
}

EnemyManager::EnemyManager() {
    // EnemyManager::Initialize: the enemy template.
    for (AnmVm& vm : template_.vms) vm.anmFileIndex = -1;
    template_.slotOccupied = true;
    template_.interactable = true;
    template_.collidable = true;
    template_.hasBeenInBounds = false;
    template_.hitboxDimensions = {12.0f, 12.0f, 12.0f};
    template_.life = 1;
    template_.score = 100;
    template_.anmPoseLeft = template_.anmPoseRight = template_.anmPoseDefault = -1;
    template_.laserRefs.fill(-1);
    template_.laserStore = 0;
    template_.damageable = true;
    template_.deathMode = EnemyDeath::DespawnNoCallback;
    template_.deathCallbackSub = -1;
    template_.runInterrupt = -1;
    template_.lifeCallbackThreshold = -1;
    template_.timerCallbackThreshold = -1;
    template_.bulletRankSpeedLow = -0.5f;
    template_.bulletRankSpeedHigh = 0.5f;
}

void EnemyManager::startStage(World& world) {
    for (Enemy& e : *enemies_) e.slotOccupied = false;
    bosses_ = {};
    timelineInstr_ = world.ecl ? world.ecl->timelineOffset() : 0;
    timelineTime_ = 0;
    prevTimelineTime_ = -1;
    timelineFinished_ = false;
    spellcard_ = {};
    randomItemSpawnIndex_ = world.rng->u16InRange(3);
    randomItemTableIndex_ = world.rng->u16InRange(8);
}

int EnemyManager::activeCount() const {
    int n = 0;
    for (const Enemy& e : *enemies_) n += e.slotOccupied;
    return n;
}

void EnemyManager::callSub(World& world, EclContext& ctx, int subId) {
    ctx.currentInstr = world.ecl->subOffset(subId);
    ctx.time = 0;
    ctx.subId = static_cast<std::uint16_t>(subId);
}

Enemy* EnemyManager::spawnEnemy(World& world, int subId, const Vec3f& pos, std::int16_t life, std::int16_t itemDrop,
                                std::int32_t score) {
    for (Enemy& e : *enemies_) {
        if (e.slotOccupied) continue;
        e = template_;
        if (life >= 0) e.life = life;
        e.position = pos;
        e.prevPosition = pos;
        callSub(world, e.ctx, subId);
        if (!runEcl(world, e)) {
            e.slotOccupied = false;
            return nullptr;
        }
        e.color = e.primaryVm.color;
        e.itemDrop = static_cast<std::int8_t>(itemDrop);
        if (life >= 0) e.life = life;
        if (score >= 0) e.score = score;
        e.maxLife = e.life;
        return &e;
    }
    world.unported->hit("more than 256 simultaneous enemies (spawn dropped)");
    return nullptr;
}

// ---- variables ---------------------------------------------------------------------

void* EnemyManager::varCell(World& world, Enemy& e, std::int32_t id, VarType* type, void* literal) {
    auto ret = [&](void* p, VarType t) {
        if (type) *type = t;
        return p;
    };
    switch (id) {
        case VarI0: return ret(&e.ctx.int0, VarType::Int);
        case VarI1: return ret(&e.ctx.int1, VarType::Int);
        case VarI2: return ret(&e.ctx.int2, VarType::Int);
        case VarI3: return ret(&e.ctx.int3, VarType::Int);
        case VarF0: return ret(&e.ctx.float0, VarType::Float);
        case VarF1: return ret(&e.ctx.float1, VarType::Float);
        case VarF2: return ret(&e.ctx.float2, VarType::Float);
        case VarF3: return ret(&e.ctx.float3, VarType::Float);
        case VarIC0: return ret(&e.ctx.counter0, VarType::Int);
        case VarIC1: return ret(&e.ctx.counter1, VarType::Int);
        case VarIC2: return ret(&e.ctx.counter2, VarType::Int);
        case VarIC3: return ret(&e.ctx.counter3, VarType::Int);
        case VarDifficulty:
            scratchDifficulty_ = world.state->difficulty;
            return ret(&scratchDifficulty_, VarType::ReadOnly);
        case VarRank:
            scratchRank_ = world.state->rank;
            return ret(&scratchRank_, VarType::ReadOnly);
        case VarSelfX: return ret(&e.position.x, VarType::Float);
        case VarSelfY: return ret(&e.position.y, VarType::Float);
        case VarSelfZ: return ret(&e.position.z, VarType::Float);
        case VarPlayerX: return ret(&world.player->positionCenter.x, VarType::ReadOnly);
        case VarPlayerY: return ret(&world.player->positionCenter.y, VarType::ReadOnly);
        case VarPlayerZ: return ret(&world.player->positionCenter.z, VarType::ReadOnly);
        case VarPlayerAngle:
            scratchPlayerAngle_ = world.player->angleToPlayer(e.position);
            return ret(&scratchPlayerAngle_, VarType::ReadOnly);
        case VarEnemyTimer: return ret(&e.phaseTimer, VarType::Int);
        case VarPlayerDistance:
            scratchPlayerDistance_ = (world.player->positionCenter - e.position).length();
            return ret(&scratchPlayerDistance_, VarType::ReadOnly);
        case VarEnemyLife: return ret(&e.life, VarType::Int);
        case VarPlayerShot:
            scratchShot_ = world.state->shotType;  // character * 2 + shot
            return ret(&scratchShot_, VarType::Int);
        default: return ret(literal, VarType::Undefined);
    }
}

std::int32_t EnemyManager::readInt(World& world, Enemy& e, std::int32_t raw) {
    std::int32_t literal = raw;
    std::int32_t v;
    std::memcpy(&v, varCell(world, e, raw, nullptr, &literal), 4);
    return v;
}

float EnemyManager::readFloat(World& world, Enemy& e, float raw) {
    if (!(raw > -2147483648.0f && raw < 2147483648.0f)) return raw;  // cannot name a variable
    std::int32_t id = static_cast<std::int32_t>(raw);
    void* cell = varCell(world, e, id, nullptr, &id);
    if (cell == &id) return raw;
    float v;
    std::memcpy(&v, cell, 4);
    return v;
}

void EnemyManager::setVar(World& world, Enemy& e, std::int32_t outId, std::int32_t rawValue) {
    std::int32_t literal = rawValue;
    void* rhs = varCell(world, e, rawValue, nullptr, &literal);
    std::int32_t bits;
    std::memcpy(&bits, rhs, 4);
    VarType type;
    std::int32_t dummy = outId;
    void* lhs = varCell(world, e, outId, &type, &dummy);
    if (type == VarType::Int || type == VarType::Float) std::memcpy(lhs, &bits, 4);
}

void EnemyManager::mathOp(World& world, Enemy& e, std::int16_t op, std::int32_t out, std::int32_t lhsRaw,
                          std::int32_t rhsRaw) {
    VarType type;
    std::int32_t dummy = out;
    void* cell = varCell(world, e, out, &type, &dummy);
    if (type == VarType::Int) {
        const std::int32_t a = readInt(world, e, lhsRaw);
        const std::int32_t b = readInt(world, e, rhsRaw);
        std::int32_t r = 0;
        switch (op) {
            case ecl_op::MathIntAdd: case ecl_op::MathFloatAdd: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); break;
            case ecl_op::MathIntSub: case ecl_op::MathFloatSub: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)); break;
            case ecl_op::MathIntMul: case ecl_op::MathFloatMul: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b)); break;
            case ecl_op::MathIntDiv: case ecl_op::MathFloatDiv:
            case ecl_op::MathIntMod: case ecl_op::MathFloatMod: {
                if (b == 0 || (a == INT_MIN && b == -1)) {
                    world.unported->hit("ECL integer division by zero (the original crashes)");
                    return;
                }
                const bool div = op == ecl_op::MathIntDiv || op == ecl_op::MathFloatDiv;
                r = div ? a / b : a % b;
                break;
            }
            default: return;
        }
        std::memcpy(cell, &r, 4);
    } else if (type == VarType::Float) {
        const float a = readFloat(world, e, f32Bits(lhsRaw));
        const float b = readFloat(world, e, f32Bits(rhsRaw));
        float r = 0;
        switch (op) {
            case ecl_op::MathIntAdd: case ecl_op::MathFloatAdd: r = a + b; break;
            case ecl_op::MathIntSub: case ecl_op::MathFloatSub: r = a - b; break;
            case ecl_op::MathIntMul: case ecl_op::MathFloatMul: r = a * b; break;
            case ecl_op::MathIntDiv: case ecl_op::MathFloatDiv: r = a / b; break;
            case ecl_op::MathIntMod: case ecl_op::MathFloatMod: r = std::fmod(a, b); break;
            default: return;
        }
        std::memcpy(cell, &r, 4);
    }
}

// ---- movement helpers --------------------------------------------------------------

void EnemyManager::moveDirTime(World& world, Enemy& e, std::uint32_t at) {
    const std::int32_t time = world.ecl->argAt<std::int32_t>(at, 0);
    const float angle = readFloat(world, e, world.ecl->argAt<float>(at, 4));
    const float speed = world.ecl->argAt<float>(at, 8);
    e.moveInterp = {std::cos(angle) * speed * time / 2.0f, std::sin(angle) * speed * time / 2.0f, 0.0f};
    e.moveInterpStartPos = e.position;
    e.moveInterpStartTime = time;
    e.moveInterpTimer = time;
    e.movementMode = EnemyMove::Interp;
}

void EnemyManager::movePosTime(World& world, Enemy& e, std::uint32_t at) {
    const Vec3f target{readFloat(world, e, world.ecl->argAt<float>(at, 4)),
                       readFloat(world, e, world.ecl->argAt<float>(at, 8)),
                       readFloat(world, e, world.ecl->argAt<float>(at, 12))};
    e.moveInterp = target - e.position;
    e.moveInterpStartPos = e.position;
    e.moveInterpStartTime = world.ecl->argAt<std::int32_t>(at, 0);
    e.moveInterpTimer = e.moveInterpStartTime;
    e.movementMode = EnemyMove::Interp;
    e.axisSpeed = {};
}

void EnemyManager::moveTime(World& world, Enemy& e, std::uint32_t at) {
    const float angle = readFloat(world, e, e.angle);
    const std::int32_t time = world.ecl->argAt<std::int32_t>(at, 0);
    e.moveInterp = {std::cos(angle) * e.speed * time / 2.0f, std::sin(angle) * e.speed * time / 2.0f, 0.0f};
    e.moveInterpStartPos = e.position;
    e.moveInterpStartTime = time;
    e.moveInterpTimer = time;
    e.movementMode = EnemyMove::Interp;
}

// ---- stage-specific "ex" instructions ------------------------------------------------

void EnemyManager::runExInstruction(World& world, Enemy& e, int index, std::uint32_t at) {
    switch (index) {
        case 0: {  // ExInsCirnoRainbowBallJank
            const std::int32_t effect = at ? world.ecl->argAt<std::int32_t>(at, 4) : 0;
            spawnEffect(world, EffectFlash, e.position, 1);
            for (int i = 0; i < BulletManager::kMaxBullets; ++i) {
                Bullet& b = world.bullets->at(i);
                if (b.state == Inactive || b.state == Despawning) continue;
                b.color = 15;  // BULLET_WHITE
                world.anm->setActiveSprite(b.sprites.spriteBullet, b.sprites.spriteBullet.baseSpriteIndex + b.color);
                if (effect == 0) {
                    b.speed = 0.0f;
                    b.velocity = {};
                } else if (effect == 1) {
                    b.exFlags |= ExAcceleration;
                    b.exDuration = 220;
                    b.timer = 0;
                    sincosmul(b.ex4Acceleration, world.rng->f32InRange(kZunTwoPi) - kZunPi, 0.01f);
                }
            }
            break;
        }
        case 1: {  // ExInsShootAtRandomArea
            float range = static_cast<float>(at ? world.ecl->argAt<std::int32_t>(at, 4) : 0);
            e.bulletProps.position = e.position + e.shootOffset;
            e.bulletProps.position.x = (world.rng->f32InRange(range) + e.position.x) - range / 2.0f;
            range *= 0.75f;
            e.bulletProps.position.y = (world.rng->f32InRange(range) + e.position.y) - range / 2.0f;
            world.bullets->spawnBulletPattern(world, e.bulletProps);
            break;
        }
        case 7: {  // stage 6: three rings of eight lasers (or bullet bursts) stepping outwards
            const std::int32_t attack = at ? world.ecl->argAt<std::int32_t>(at, 4) : 0;
            const bool hard = world.state->difficulty >= Hard;
            const float r = world.rng->f32InRange(kZunTwoPi);
            for (int pass = 0; pass < 2; ++pass) {
                const float a0 = (pass == 0 ? -kZunPi : -kZunPi * 7.0f / 8.0f) + r;
                const float turn = pass == 0 ? kZunPi / 4.0f : -kZunPi / 4.0f;
                std::array<Vec3f, 8> points;
                float a = a0;
                for (Vec3f& p : points) {
                    p = e.position + Vec3f{std::cos(a) * 32.0f, std::sin(a) * 32.0f, 0.0f};
                    a += kZunPi / 4.0f;
                }
                a = a0;
                for (int k = 0; k < 3; ++k) {
                    const float segment = k < 2 ? 112.0f : 480.0f;
                    for (Vec3f& p : points) {
                        if (attack == 0) {
                            LaserProps lp;
                            lp.sprite = 1;
                            lp.color = hard ? 8 : 2;
                            lp.position = p;
                            lp.angle = a;
                            lp.speed = 0.0f;
                            lp.startOffset = 0.0f;
                            lp.endOffset = lp.startLength = hard ? 440.0f : segment;
                            lp.width = hard ? 20.0f : 28.0f;
                            lp.startTime = 60 + 16 * k;
                            lp.duration = 90 - 16 * k;
                            lp.despawnDuration = 16;
                            lp.hitboxStartTime = 50;
                            lp.hitboxEndDelay = 16;
                            lp.flags = 2;
                            world.bullets->spawnLaser(world, lp);
                        } else {
                            e.bulletProps.position = p;
                            world.bullets->spawnBulletPattern(world, e.bulletProps);
                        }
                        p += Vec3f{std::cos(a) * segment, std::sin(a) * segment, 0.0f};
                        a += kZunPi / 4.0f;
                    }
                    a += turn - kZunTwoPi;
                }
            }
            break;
        }
        case 12:  // stage 4: shoot from 64 px along each of the first eight lasers (y mirrored, as in the original)
            for (int i = 0; i < 8; ++i) {
                const Laser* l = laserRef(world, e, i);
                if (!l || !l->inUse) continue;
                e.bulletProps.position = e.position + Vec3f{std::cos(l->angle) * 64.0f, -std::sin(l->angle) * 64.0f, 0.0f};
                world.bullets->spawnBulletPattern(world, e.bulletProps);
            }
            break;
        case 14:  // extra: shoot from every 48 px along each of the first eight lasers
            e.ctx.int3 = 0;
            for (int i = 0; i < 8; ++i) {
                const Laser* l = laserRef(world, e, i);
                if (!l || !l->inUse) continue;
                const float c = std::cos(l->angle), sn = std::sin(l->angle);
                for (float t = l->startOffset; t < l->endOffset; t += 48.0f) {
                    e.bulletProps.position = l->pos + Vec3f{c * t, sn * t, 0.0f};
                    world.bullets->spawnBulletPattern(world, e.bulletProps);
                }
                ++e.ctx.int3;
            }
            break;
        case 6:  // stage 6: bat-wing trail
            batWingTrail(world, e);
            break;
        case 10:  // stage 6: the trail, then the bat transformation
            batTransformation(world, e);
            break;
        case 2:  // a pentagram traced by bullets, its centre drifting towards the player
            exPentagram(world, e);
            break;
        case 3: {  // stage 4: Patchouli's three spells, from the player's shot type
            static constexpr std::int32_t kSpells[4][3] = {{0, 3, 1}, {2, 3, 4}, {1, 4, 0}, {4, 2, 3}};
            const auto& row = kSpells[world.state->shotType & 3];
            e.ctx.int1 = row[0];
            e.ctx.int2 = row[1];
            e.ctx.int3 = row[2];
            break;
        }
        case 4:  // stages 5 and 6: time stop on (1) / off (0), or redirect large bullets (>= 2)
            exTimeStop(world, e, at ? world.ecl->argAt<std::int32_t>(at, 4) : 0);
            break;
        case 5:  // stage 5: arcs of nine dagger volleys
            exDaggerArcs(world, e);
            break;
        case 8: {  // stage 6: a dark red ball on every large bullet
            std::int32_t count = 0;
            EnemyShooter r = localRecord();
            for (int i = 0; i < BulletManager::kMaxBullets; ++i) {
                const Bullet& b = world.bullets->at(i);
                if (!liveBullet(b) || bulletSize(world, b) != BulletSize::Large) continue;
                r.position = b.pos;
                r.sprite = 3;  // ball
                r.color = 1;
                r.angle1 = world.rng->f32InRange(kZunTwoPi) - kZunPi;
                r.speed1 = 0.0f;
                r.count1 = r.count2 = 1;
                r.flags = ExSpawnEffectLong;
                r.aimMode = Fan;
                world.bullets->spawnBulletPattern(world, r);
                ++count;  // counted even when the pool was full
            }
            e.ctx.int3 = count;
            break;
        }
        case 9: {  // stage 6: resting small bullets accelerate outwards, curling with distance
            const float r = world.rng->f32InRange(kZunTwoPi) - kZunPi;
            spawnEffect(world, EffectFlash, e.position, 1);
            for (int i = 0; i < BulletManager::kMaxBullets; ++i) {
                Bullet& b = world.bullets->at(i);
                if (!liveBullet(b) || bulletSize(world, b) != BulletSize::Small || b.speed != 0.0f) continue;
                recolor(world, b, 2);
                b.speed = 0.01f;  // marks it, so later calls leave it alone
                const float dx = e.position.x - b.pos.x, dy = e.position.y - b.pos.y;
                const float q = dx * dx + dy * dy;
                const float d = q > 0.1f ? std::sqrt(q) : 0.0f;
                armAcceleration(b, d * kZunPi / 256.0f + r, 120);
            }
            break;
        }
        case 11: {  // stage 6: resting small bullets accelerate in random directions
            world.rng->f32InRange(kZunTwoPi);  // drawn and unused, as in the original
            spawnEffect(world, EffectFlash, e.position, 1);
            for (int i = 0; i < BulletManager::kMaxBullets; ++i) {
                Bullet& b = world.bullets->at(i);
                if (!liveBullet(b) || bulletSize(world, b) != BulletSize::Small || b.speed != 0.0f) continue;
                recolor(world, b, 2);
                b.speed = 0.01f;
                armAcceleration(b, world.rng->f32InRange(kZunTwoPi) - kZunPi, 120);
            }
            break;
        }
        case 13: {  // extra: N copies of the pattern on a circle around the playfield centre
            const std::int32_t n = at ? world.ecl->argAt<std::int32_t>(at, 4) : 0;
            EnemyShooter t = e.bulletProps;
            float a = e.ctx.float2;
            if (e.ctx.int3 % 6 == 0) {
                for (std::int32_t i = 0; i < n; ++i) {
                    t.position.x = std::cos(a) * e.ctx.float3 + kGameRegionWidth / 2.0f;
                    t.position.y = std::sin(a) * e.ctx.float3 + kGameRegionHeight / 2.0f;
                    t.angle1 = a + e.ctx.float1;
                    world.bullets->spawnBulletPattern(world, t);
                    a += kZunTwoPi / static_cast<float>(n);  // not wrapped
                }
            }
            e.ctx.int3++;
            break;
        }
        case 15: {  // extra: large bullets fling nearby resting bullets outwards, then ex 10
            std::int32_t count = 0;
            for (int i = 0; i < BulletManager::kMaxBullets; ++i) {
                const Bullet& outer = world.bullets->at(i);
                if (!liveBullet(outer) || bulletSize(world, outer) != BulletSize::Large) continue;
                ++count;
                const float alpha = std::atan2(outer.pos.y - e.position.y, outer.pos.x - e.position.x);
                for (int j = 0; j < BulletManager::kMaxBullets; ++j) {
                    Bullet& b = world.bullets->at(j);
                    if (!liveBullet(b) || bulletSize(world, b) != BulletSize::Small || b.speed != 0.0f) continue;
                    const float dx = b.pos.x - outer.pos.x, dy = b.pos.y - outer.pos.y;
                    if (!(std::sqrt(dx * dx + dy * dy) < 64.0f)) continue;
                    b.exFlags |= ExAcceleration;
                    b.speed = 0.01f;
                    b.timer = 0;
                    b.exDuration = 120;
                    const float beta = std::atan2(b.pos.y - e.position.y, b.pos.x - e.position.x);
                    b.angle = (beta - alpha) * 2.2f + alpha;  // not wrapped
                    sincosmul(b.ex4Acceleration, b.angle, 0.01f);
                    recolor(world, b, b.color + 1);
                }
            }
            batTransformation(world, e);
            e.ctx.int3 = count;
            break;
        }
        case 16: {  // extra: Flandre's last attack, tuned by her remaining life
            const std::int32_t mode = at ? world.ecl->argAt<std::int32_t>(at, 4) : 0;
            const std::int32_t life = e.phaseTimer >= 7200 ? 0 : e.life;
            const float l = static_cast<float>(life);
            if (mode == 0) {
                e.ctx.float3 = 2.0f - (l * 1.0f) / 6000.0f;
                e.ctx.counter1 = static_cast<std::int32_t>(static_cast<std::uint32_t>(life) * 240u) / 6000 + 40;
            } else {
                const float w = 320.0f - (l * 160.0f) / 6000.0f;
                e.ctx.float2 = world.rng->f32InRange(w) + (192.0f - w / 2.0f);
                const float w2 = 128.0f - (l * 64.0f) / 6000.0f;
                e.ctx.float3 = world.rng->f32InRange(w2) + (96.0f - w2 / 2.0f);
            }
            break;
        }
        default:
            // The original reads past its 17-entry table here (a crash); ignore it instead.
            world.unported->hit("ECL ex-instruction (unknown index)");
            break;
    }
}

// ---- ex-instruction helpers -------------------------------------------------------------

bool EnemyManager::liveBullet(const Bullet& b) { return b.state != Inactive && b.state != Despawning; }

// Large = sprite at least 30 px tall (fireballs, big balls, daggers, bubbles), read from the
// bullet's current sprite like the original.
EnemyManager::BulletSize EnemyManager::bulletSize(World& world, const Bullet& b) {
    const AnmLoadedSprite* s = world.anm->sprite(b.sprites.spriteBullet);
    if (!s) return BulletSize::None;
    return s->heightPx >= 30.0f ? BulletSize::Large : BulletSize::Small;
}

void EnemyManager::recolor(World& world, Bullet& b, int color) {
    b.color = static_cast<std::int16_t>(color);
    world.anm->setActiveSprite(b.sprites.spriteBullet, b.sprites.spriteBullet.baseSpriteIndex + b.color);
}

// 0.01 px/frame^2 along `angle` for `duration` fired frames, from the bullet's next fired
// frame (its timer restarts; a bullet still spawning restarts it again when it comes out).
void EnemyManager::armAcceleration(Bullet& b, float angle, std::int32_t duration) {
    b.exFlags |= ExAcceleration;
    b.timer = 0;
    b.exDuration = duration;
    sincosmul(b.ex4Acceleration, angle, 0.01f);
}

// A temporary bullet record of the original: every field zero, then filled in.
EnemyShooter EnemyManager::localRecord() {
    EnemyShooter r;
    r.sfx = 0;
    return r;
}

void EnemyManager::exPentagram(World& world, Enemy& e) {
    const std::int32_t k = e.ctx.int2, steps = e.ctx.int3;
    if (k >= steps) {
        e.ctx.funcSet = -1;
        return;
    }
    auto wrap = [](float a) { return addNormalizeAngle(a, 0.0f); };
    if (k == 0) {
        starEnemy_ = e.position;
        starPlayer_ = world.player->positionCenter;
        starAngles_[0] = world.rng->f32InRange(kZunTwoPi) - kZunPi;
        starAngles_[1] = wrap(starAngles_[0] + kZunPi * 4.0f / 5.0f);
    }
    if (k % 30 == 0) {
        starAngles_[0] = starAngles_[1];
        for (int j = 1; j < 6; ++j) starAngles_[static_cast<std::size_t>(j)] = wrap(starAngles_[static_cast<std::size_t>(j - 1)] + kZunPi * 4.0f / 5.0f);
    }
    if (k % 6 == 0) {
        EnemyShooter& r = e.bulletProps;
        const float p = static_cast<float>(k) / static_cast<float>(steps);
        Vec3f centre = (starPlayer_ - starEnemy_) * (0.1f * p) + starEnemy_;
        centre.z = 0.0f;
        const float q = p + 0.5f;
        r.angle1 = kZunPi / 3.0f * q;
        const float radius = e.ctx.float3;
        for (int i = 0; i < 5; ++i) {
            const float u = static_cast<float>(k % 30) / 30.0f;
            const float a0 = starAngles_[static_cast<std::size_t>(i)], a1 = starAngles_[static_cast<std::size_t>(i + 1)];
            const Vec3f v0{std::cos(a0) * radius, std::sin(a0) * radius, 0.0f};
            const Vec3f v1{std::cos(a1) * radius, std::sin(a1) * radius, 0.0f};
            Vec3f v = (v1 - v0) * u + v0;
            v.z = 0.0f;
            r.position = centre + v;
            const float speed = r.speed1;
            r.speed1 = world.rng->f32InRange(r.speed2) + r.speed1;
            world.bullets->spawnBulletPattern(world, r);
            r.speed1 = speed;
            r.angle1 -= kZunPi / 6.0f * q;
        }
        world.playSound(22);
    }
    e.ctx.int2 = k + 1;
}

void EnemyManager::exTimeStop(World& world, Enemy& e, std::int32_t mode) {
    GameState& gs = *world.state;
    if (mode < 2) {
        spawnEffect(world, EffectFlash, e.position, 1);
        gs.timeStop = static_cast<std::int8_t>(static_cast<std::uint8_t>(mode & 0xff));
        e.ctx.int2 = 0;
        return;
    }
    // Up to 14 (Easy/Normal) or 52 (Hard and up) large bullets turn green and get a new
    // heading: scattered when far from the player, swirling around her when close.
    const bool hard = gs.difficulty >= Hard;
    int left = hard ? 52 : 14;
    const Vec3f& pc = world.player->positionCenter;
    for (int i = 0; i < BulletManager::kMaxBullets && left > 0; ++i) {
        Bullet& b = world.bullets->at(i);
        if (!liveBullet(b) || bulletSize(world, b) != BulletSize::Large || b.color == 5) continue;
        if (world.rng->u16() % 4 != 0) continue;
        recolor(world, b, 5);
        const float dx = b.pos.x - pc.x, dy = b.pos.y - pc.y;
        if (std::sqrt(dx * dx + dy * dy) > 128.0f) {
            b.angle = hard ? world.rng->f32InRange(kZunTwoPi) : world.rng->f32InRange(kZunPi * 3.0f / 4.0f) + kZunPi / 4.0f;
        } else {
            const float toBullet = dx == 0.0f && dy == 0.0f ? kZunHalfPi : std::atan2(dy, dx);
            b.angle = toBullet + kZunHalfPi + world.rng->f32InRange(kZunTwoPi);  // not wrapped
        }
        sincosmul(b.velocity, b.angle, b.speed);
        --left;
    }
    e.ctx.int2 = 0;
}

void EnemyManager::exDaggerArcs(World& world, Enemy& e) {
    const std::int32_t k = e.ctx.int2;
    if (k % 9 == 0) {
        const bool easy = world.state->difficulty < Hard;
        const std::int32_t n = k / 9;
        const bool odd = n % 2 != 0;
        EnemyShooter r = localRecord();
        r.sprite = 8;  // dagger
        r.aimMode = FanAimed;
        r.count1 = easy ? 1 : 3;
        r.count2 = 1;
        r.angle2 = kZunPi / 6.0f;
        r.flags = 0;
        r.color = 3;
        r.speed1 = 2.0f;
        const float s = 0.5f - static_cast<float>(n) * 0.5f / 9.0f;
        const Vec3f d = world.player->positionCenter - e.position;
        const float len = d.length();
        const Vec3f u = len != 0.0f ? d / len : Vec3f{};
        const float reach = odd ? -256.0f : 256.0f;
        const Vec3f anchor = d * s + u * reach;
        Vec3f w = (u * reach) * -1.0f;
        // R(t): (x, y) -> (x cos t + y sin t, -x sin t + y cos t); turns by -t.
        auto rotate = [](Vec3f v, float c, float sn) {
            return Vec3f{v.x * c + v.y * sn, -v.x * sn + v.y * c, v.z};
        };
        w = rotate(w, std::cos(kZunPi / 4.0f), std::sin(kZunPi / 4.0f));
        const float stepC = std::cos(-kZunPi / 18.0f), stepS = std::sin(-kZunPi / 18.0f);
        r.angle1 = 0.0f;
        float beta = -kZunPi / 4.0f;
        for (int i = 0; i < 9; ++i) {
            w = rotate(w, stepC, stepS);
            r.position = w + e.position + anchor;
            r.speed1 = 2.0f;
            if (odd && easy) r.angle1 = beta;
            r.color = 3;
            world.bullets->spawnBulletPattern(world, r);
            beta += kZunPi / 18.0f;
        }
        world.playSound(7);
    }
    e.ctx.int2 = k + 1;
}

// Stage 6 boss: while the player bombs she turns into bats (no damage, no contact) and comes
// back 60 calls after the bomb ends.
void EnemyManager::batTransformation(World& world, Enemy& e) {
    if (e.life <= 0) return;
    batWingTrail(world, e);
    if (world.state->bombActive) {
        if (e.anmPoseLeft >= 0) {
            world.anm->setAndExecuteScript(e.primaryVm, kAnmOffsetEnemy + 165);
            e.anmPoseLeft = -1;
        }
        e.interactable = false;
        e.batRestoreTimer = 60;
    } else if (e.batRestoreTimer > 0 && --e.batRestoreTimer == 0) {
        if (e.anmPoseLeft < 0) {
            world.anm->setAndExecuteScript(e.primaryVm, kAnmOffsetEnemy + 160);
            e.anmPoseLeft = 161;
        }
        e.interactable = true;
    }
}

// ---- the VM --------------------------------------------------------------------------

bool EnemyManager::runEcl(World& world, Enemy& e) {
    EclFile& ecl = *world.ecl;
    EclRawInstr cur;
    std::uint32_t at;
    int budget = kMaxInstructionsPerTick;

restart_sub_changed:
    at = e.ctx.currentInstr;
    if (!ecl.instrAt(at, cur)) {
        log::warn("ECL sub {}: instruction at 0x{:x} out of range; enemy removed", e.ctx.subId, at);
        return false;
    }
    if (e.runInterrupt >= 0) goto run_interrupt;

    while (e.ctx.time == cur.time) {
        if (--budget < 0) {
            log::warn("ECL sub {} runs away (>{} instructions in one frame); enemy removed", e.ctx.subId,
                      kMaxInstructionsPerTick);
            return false;
        }
        ++instructionsExecuted_;
        if (!(cur.difficultyMask & (1u << world.state->difficulty))) goto next_instruction;

        switch (cur.opCode) {
            case ecl_op::EnemyDelete:
                return false;
            case ecl_op::Loop: {
                const std::int32_t var = ecl.argAt<std::int32_t>(at, 8);
                std::int32_t n = readInt(world, e, var) - 1;
                setVar(world, e, var, n);
                if (n <= 0) break;
                [[fallthrough]];
            }
            case ecl_op::Jump:
            handle_jump:
                e.ctx.time = ecl.argAt<std::int32_t>(at, 0);
                at = static_cast<std::uint32_t>(static_cast<std::int64_t>(at) + ecl.argAt<std::int32_t>(at, 4));
                if (!ecl.instrAt(at, cur)) {
                    log::warn("ECL sub {}: jump out of range; enemy removed", e.ctx.subId);
                    return false;
                }
                continue;
            case ecl_op::SetInt:
            case ecl_op::SetFloat:
                setVar(world, e, ecl.argAt<std::int32_t>(at, 0), ecl.argAt<std::int32_t>(at, 4));
                break;
            case ecl_op::MathReduceAngle: {
                const std::int32_t var = ecl.argAt<std::int32_t>(at, 0);
                std::int32_t literal = var;
                float f;
                std::memcpy(&f, varCell(world, e, var, nullptr, &literal), 4);
                setVar(world, e, var, i32Bits(addNormalizeAngle(f, 0.0f)));
                break;
            }
            case ecl_op::SetIntRand: {
                const auto range = static_cast<std::uint32_t>(readInt(world, e, ecl.argAt<std::int32_t>(at, 4)));
                setVar(world, e, ecl.argAt<std::int32_t>(at, 0), static_cast<std::int32_t>(world.rng->u32InRange(range)));
                break;
            }
            case ecl_op::SetIntRandMin: {
                const auto range = static_cast<std::uint32_t>(readInt(world, e, ecl.argAt<std::int32_t>(at, 4)));
                const std::int32_t minVal = readInt(world, e, ecl.argAt<std::int32_t>(at, 8));
                const auto v = static_cast<std::int32_t>(world.rng->u32InRange(range) + static_cast<std::uint32_t>(minVal));
                setVar(world, e, ecl.argAt<std::int32_t>(at, 0), v);
                break;
            }
            case ecl_op::SetFloatRand: {
                const float range = readFloat(world, e, ecl.argAt<float>(at, 4));
                setVar(world, e, ecl.argAt<std::int32_t>(at, 0), i32Bits(world.rng->f32InRange(range)));
                break;
            }
            case ecl_op::SetFloatRandMin: {
                const float range = readFloat(world, e, ecl.argAt<float>(at, 4));
                const float minVal = readFloat(world, e, ecl.argAt<float>(at, 8));
                setVar(world, e, ecl.argAt<std::int32_t>(at, 0), i32Bits(world.rng->f32InRange(range) + minVal));
                break;
            }
            case ecl_op::SetVarSelfX: setVar(world, e, ecl.argAt<std::int32_t>(at, 0), i32Bits(e.position.x)); break;
            case ecl_op::SetVarSelfY: setVar(world, e, ecl.argAt<std::int32_t>(at, 0), i32Bits(e.position.y)); break;
            case ecl_op::SetVarSelfZ: setVar(world, e, ecl.argAt<std::int32_t>(at, 0), i32Bits(e.position.z)); break;
            case ecl_op::MathIntAdd: case ecl_op::MathFloatAdd:
            case ecl_op::MathIntSub: case ecl_op::MathFloatSub:
            case ecl_op::MathIntMul: case ecl_op::MathFloatMul:
            case ecl_op::MathIntDiv: case ecl_op::MathFloatDiv:
            case ecl_op::MathIntMod: case ecl_op::MathFloatMod:
                mathOp(world, e, cur.opCode, ecl.argAt<std::int32_t>(at, 0), ecl.argAt<std::int32_t>(at, 4),
                       ecl.argAt<std::int32_t>(at, 8));
                break;
            case ecl_op::MathInc:
            case ecl_op::MathDec: {
                const std::int32_t var = ecl.argAt<std::int32_t>(at, 0);
                std::int32_t literal = var;
                void* cell = varCell(world, e, var, nullptr, &literal);
                std::int32_t v;
                std::memcpy(&v, cell, 4);
                v = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) + (cur.opCode == ecl_op::MathInc ? 1u : 0xffffffffu));
                if (cell == &literal) ecl.writeArg<std::int32_t>(at, 0, v);  // the script edits itself
                else std::memcpy(cell, &v, 4);
                break;
            }
            case ecl_op::MathLineAngle: {
                VarType type;
                const std::int32_t out = ecl.argAt<std::int32_t>(at, 0);
                std::int32_t dummy = out;
                void* cell = varCell(world, e, out, &type, &dummy);
                if (type == VarType::Float) {
                    const float ax = readFloat(world, e, ecl.argAt<float>(at, 4));
                    const float ay = readFloat(world, e, ecl.argAt<float>(at, 8));
                    const float bx = readFloat(world, e, ecl.argAt<float>(at, 12));
                    const float by = readFloat(world, e, ecl.argAt<float>(at, 16));
                    const float r = std::atan2(by - ay, bx - ax);
                    std::memcpy(cell, &r, 4);
                }
                break;
            }
            case ecl_op::CmpInt: {
                const std::int32_t l = readInt(world, e, ecl.argAt<std::int32_t>(at, 0));
                const std::int32_t r = readInt(world, e, ecl.argAt<std::int32_t>(at, 4));
                e.ctx.compareRegister = l == r ? 0 : l < r ? -1 : 1;
                break;
            }
            case ecl_op::CmpFloat: {
                const float l = readFloat(world, e, ecl.argAt<float>(at, 0));
                const float r = readFloat(world, e, ecl.argAt<float>(at, 4));
                e.ctx.compareRegister = l == r ? 0 : l < r ? -1 : 1;
                break;
            }
            case ecl_op::JumpLss: if (e.ctx.compareRegister < 0) goto handle_jump; break;
            case ecl_op::JumpLeq: if (e.ctx.compareRegister <= 0) goto handle_jump; break;
            case ecl_op::JumpEqu: if (e.ctx.compareRegister == 0) goto handle_jump; break;
            case ecl_op::JumpGre: if (e.ctx.compareRegister > 0) goto handle_jump; break;
            case ecl_op::JumpGeq: if (e.ctx.compareRegister >= 0) goto handle_jump; break;
            case ecl_op::JumpNeq: if (e.ctx.compareRegister != 0) goto handle_jump; break;
            case ecl_op::Call:
            handle_call: {
                const std::int32_t subId = ecl.argAt<std::int32_t>(at, 0);
                if (subId < 0 || subId >= ecl.subCount()) {
                    log::warn("ECL: call to missing sub {}; enemy removed", subId);
                    return false;
                }
                e.ctx.currentInstr = at + static_cast<std::uint32_t>(cur.offsetToNext);
                if (!e.disableCallStack) e.savedContextStack[static_cast<std::size_t>(e.stackDepth)] = e.ctx;
                callSub(world, e.ctx, subId);
                if (!e.disableCallStack && e.stackDepth < Enemy::kMaxStackDepth) e.stackDepth++;
                e.ctx.int0 = ecl.argAt<std::int32_t>(at, 4);
                e.ctx.float0 = ecl.argAt<float>(at, 8);
                goto restart_sub_changed;
            }
            case ecl_op::Ret:
                if (e.stackDepth <= 0) {
                    log::warn("ECL sub {}: return with an empty call stack; enemy removed", e.ctx.subId);
                    return false;
                }
                e.stackDepth--;
                e.ctx = e.savedContextStack[static_cast<std::size_t>(e.stackDepth)];
                goto restart_sub_changed;
            case ecl_op::CallLss: case ecl_op::CallLeq: case ecl_op::CallEqu:
            case ecl_op::CallGre: case ecl_op::CallGeq: case ecl_op::CallNeq: {
                const std::int32_t v = readInt(world, e, ecl.argAt<std::int32_t>(at, 12));
                const std::int32_t k = ecl.argAt<std::int32_t>(at, 16);
                bool take = false;
                switch (cur.opCode) {
                    case ecl_op::CallLss: take = v < k; break;
                    case ecl_op::CallLeq: take = v <= k; break;
                    case ecl_op::CallEqu: take = v == k; break;
                    case ecl_op::CallGre: take = v > k; break;
                    case ecl_op::CallGeq: take = v >= k; break;
                    default: take = v != k; break;
                }
                if (take) goto handle_call;
                break;
            }
            case ecl_op::AnmSetMain:
                world.anm->setAndExecuteScript(e.primaryVm, ecl.argAt<std::int32_t>(at, 0) + kAnmScriptEnemyStart);
                break;
            case ecl_op::AnmSetSlot: {
                const std::int32_t slot = ecl.argAt<std::int32_t>(at, 0);
                if (slot < 0 || slot >= Enemy::kAnmSlots) {
                    log::warn("ECL: ANM slot {} out of range", slot);  // original: "sub anim overflow"
                    break;
                }
                world.anm->setAndExecuteScript(e.vms[static_cast<std::size_t>(slot)],
                                               ecl.argAt<std::int32_t>(at, 4) + kAnmScriptEnemyStart);
                break;
            }
            case ecl_op::MovePosition:
                e.position = {readFloat(world, e, ecl.argAt<float>(at, 0)), readFloat(world, e, ecl.argAt<float>(at, 4)),
                              readFloat(world, e, ecl.argAt<float>(at, 8))};
                e.clampPos();
                break;
            case ecl_op::MoveAxisSpeed:
                e.axisSpeed = {readFloat(world, e, ecl.argAt<float>(at, 0)), readFloat(world, e, ecl.argAt<float>(at, 4)),
                               readFloat(world, e, ecl.argAt<float>(at, 8))};
                e.movementMode = EnemyMove::AxisSpeed;
                break;
            case ecl_op::MoveVelocity:
                e.angle = readFloat(world, e, ecl.argAt<float>(at, 0));
                e.speed = readFloat(world, e, ecl.argAt<float>(at, 4));
                e.movementMode = EnemyMove::Velocity;
                break;
            case ecl_op::MoveAngularVelocity:
                e.angularVelocity = readFloat(world, e, ecl.argAt<float>(at, 0));
                e.movementMode = EnemyMove::Velocity;
                break;
            case ecl_op::MoveTowardsPlayer:
                e.angle = world.player->angleToPlayer(e.position) + ecl.argAt<float>(at, 0);
                e.speed = readFloat(world, e, ecl.argAt<float>(at, 4));
                e.movementMode = EnemyMove::Velocity;
                break;
            case ecl_op::MoveSpeed:
                e.speed = readFloat(world, e, ecl.argAt<float>(at, 0));
                e.movementMode = EnemyMove::Velocity;
                break;
            case ecl_op::MoveAcceleration:
                e.acceleration = readFloat(world, e, ecl.argAt<float>(at, 0));
                e.movementMode = EnemyMove::Velocity;
                break;
            case ecl_op::BulletFanAimed: case ecl_op::BulletFan: case ecl_op::BulletCircleAimed:
            case ecl_op::BulletCircle: case ecl_op::BulletOffsetCircleAimed: case ecl_op::BulletOffsetCircle:
            case ecl_op::BulletRandomAngle: case ecl_op::BulletRandomSpeed: case ecl_op::BulletRandom: {
                const auto args = ecl.argAt<EclRawInstrBulletArgs>(at, 0);
                EnemyShooter& s = e.bulletProps;
                const GameState& gs = *world.state;
                s.sprite = args.sprite;
                s.aimMode = static_cast<std::uint16_t>(cur.opCode - ecl_op::BulletFanAimed);
                s.count1 = static_cast<std::int16_t>(readInt(world, e, args.count1) +
                                                     gs.rankLerpInt(e.bulletRankAmount1Low, e.bulletRankAmount1High));
                if (s.count1 <= 0) s.count1 = 1;
                s.count2 = static_cast<std::int16_t>(readInt(world, e, args.count2) +
                                                     gs.rankLerpInt(e.bulletRankAmount2Low, e.bulletRankAmount2High));
                if (s.count2 <= 0) s.count2 = 1;
                s.position = e.position + e.shootOffset;
                s.angle1 = addNormalizeAngle(readFloat(world, e, args.angle1), 0.0f);
                s.speed1 = readFloat(world, e, args.speed1);
                if (s.speed1 != 0.0f) {
                    s.speed1 += gs.rankLerpFloat(e.bulletRankSpeedLow, e.bulletRankSpeedHigh);
                    if (s.speed1 < 0.3f) s.speed1 = 0.3f;
                }
                s.angle2 = readFloat(world, e, args.angle2);
                s.speed2 = readFloat(world, e, args.speed2);
                s.speed2 += gs.rankLerpFloat(e.bulletRankSpeedLow, e.bulletRankSpeedHigh) / 2.0f;
                if (s.speed2 < 0.3f) s.speed2 = 0.3f;
                s.flags = static_cast<std::uint32_t>(args.flags);
                s.color = static_cast<std::int16_t>(readInt(world, e, args.color));
                if (!e.shootingDisabled) world.bullets->spawnBulletPattern(world, s);
                break;
            }
            case ecl_op::BulletEffects:
                for (int i = 0; i < 4; ++i) e.bulletProps.exInts[i] = readInt(world, e, ecl.argAt<std::int32_t>(at, 4 * i));
                for (int i = 0; i < 4; ++i) e.bulletProps.exFloats[i] = readFloat(world, e, ecl.argAt<float>(at, 16 + 4 * i));
                break;
            case ecl_op::AnmDeathEffects:
                e.deathParticle1 = ecl.argAt<std::uint8_t>(at, 0);
                e.deathParticle2 = ecl.argAt<std::uint8_t>(at, 1);
                e.deathAnm3 = ecl.argAt<std::uint8_t>(at, 2);
                break;
            case ecl_op::ShootInterval:
                e.shootInterval = ecl.argAt<std::int32_t>(at, 0);
                e.shootInterval += world.state->rankLerpInt(e.shootInterval / 5, -e.shootInterval / 5);
                e.shootIntervalTimer = 0;
                break;
            case ecl_op::ShootIntervalDelayed:
                e.shootInterval = ecl.argAt<std::int32_t>(at, 0);
                e.shootInterval += world.state->rankLerpInt(e.shootInterval / 5, -e.shootInterval / 5);
                if (e.shootInterval != 0) {
                    e.shootIntervalTimer = static_cast<std::int32_t>(world.rng->u32InRange(static_cast<std::uint32_t>(e.shootInterval)));
                }
                break;
            case ecl_op::ShootDisable: e.shootingDisabled = true; break;
            case ecl_op::ShootEnable: e.shootingDisabled = false; break;
            case ecl_op::ShootNow:
                e.bulletProps.position = e.position + e.shootOffset;
                world.bullets->spawnBulletPattern(world, e.bulletProps);
                break;
            case ecl_op::ShootOffset:
                e.shootOffset = {readFloat(world, e, ecl.argAt<float>(at, 0)), readFloat(world, e, ecl.argAt<float>(at, 4)),
                                 readFloat(world, e, ecl.argAt<float>(at, 8))};
                break;
            case ecl_op::LaserCreate:
            case ecl_op::LaserCreateAimed: {
                const auto args = ecl.argAt<EclRawInstrLaserArgs>(at, 0);
                LaserProps p;
                p.sprite = args.sprite;
                p.color = args.color;
                p.position = e.position + e.shootOffset;
                p.angle = readFloat(world, e, args.angle);
                p.speed = readFloat(world, e, args.speed);
                p.startOffset = readFloat(world, e, args.startOffset);
                p.endOffset = readFloat(world, e, args.endOffset);
                p.startLength = readFloat(world, e, args.startLength);
                p.width = args.width;
                p.startTime = args.startTime;
                p.duration = args.duration;
                p.despawnDuration = args.despawnDuration;
                p.hitboxStartTime = args.hitboxStartTime;
                p.hitboxEndDelay = args.hitboxEndDelay;
                p.flags = static_cast<std::uint16_t>(args.flags);
                p.aimed = cur.opCode == ecl_op::LaserCreateAimed;
                const int slot = world.bullets->spawnLaser(world, p);
                if (e.laserStore >= 0 && e.laserStore < static_cast<std::int32_t>(e.laserRefs.size())) {
                    e.laserRefs[static_cast<std::size_t>(e.laserStore)] = static_cast<std::int16_t>(slot);
                } else {
                    log::warn("ECL: laser index {} out of range", e.laserStore);  // original: unchecked write
                }
                break;
            }
            case ecl_op::LaserIndex: e.laserStore = readInt(world, e, ecl.argAt<std::int32_t>(at, 0)); break;
            case ecl_op::LaserRotate:
                if (Laser* l = laserRef(world, e, ecl.argAt<std::int32_t>(at, 0))) {
                    l->angle += readFloat(world, e, ecl.argAt<float>(at, 4));
                }
                break;
            case ecl_op::LaserRotateFromPlayer:
                if (Laser* l = laserRef(world, e, ecl.argAt<std::int32_t>(at, 0))) {
                    l->angle = world.player->angleToPlayer(l->pos) + readFloat(world, e, ecl.argAt<float>(at, 4));
                }
                break;
            case ecl_op::LaserOffset:
                if (Laser* l = laserRef(world, e, ecl.argAt<std::int32_t>(at, 0))) {
                    l->pos = e.position + Vec3f{ecl.argAt<float>(at, 4), ecl.argAt<float>(at, 8), ecl.argAt<float>(at, 12)};
                }
                break;
            case ecl_op::LaserTest: {
                const Laser* l = laserRef(world, e, ecl.argAt<std::int32_t>(at, 0));
                e.ctx.compareRegister = (l && l->inUse) ? 0 : 1;
                break;
            }
            case ecl_op::LaserCancel:
                if (Laser* l = laserRef(world, e, ecl.argAt<std::int32_t>(at, 0)); l && l->inUse && l->state < LaserDespawning) {
                    l->state = LaserDespawning;
                    l->timer = 0;
                }
                break;
            case ecl_op::LaserClearAll: e.laserRefs.fill(-1); break;
            case ecl_op::BossSet: {
                const std::int32_t id = ecl.argAt<std::int32_t>(at, 0);
                if (id >= 0) {
                    if (id < 4) bosses_[static_cast<std::size_t>(id)] = &e;
                    world.state->bossPresent = true;
                    world.state->bossFillTarget = 1.0f;
                    e.isBoss = true;
                    e.bossId = static_cast<std::uint8_t>(id);
                } else {
                    world.state->bossPresent = false;
                    if (e.bossId < 4) bosses_[e.bossId] = nullptr;
                    e.isBoss = false;
                }
                break;
            }
            case ecl_op::SpellcardEffect: {  // a spell circle orbiting this enemy
                const int slot = spawnEffect(world, EffectOrbit, e.position, 1,
                                             EffectManager::colorTable(ecl.argAt<std::int32_t>(at, 0)));
                if (world.effects) {
                    world.effects->at(slot).axis = {ecl.argAt<float>(at, 4), ecl.argAt<float>(at, 8), ecl.argAt<float>(at, 12)};
                }
                e.effectDistance = ecl.argAt<float>(at, 16);
                if (e.effectCount < static_cast<std::int32_t>(e.effectRefs.size())) {
                    e.effectRefs[static_cast<std::size_t>(e.effectCount++)] = static_cast<std::int16_t>(slot);
                }
                break;
            }
            case ecl_op::MoveVelocityInterpDecelerate: moveDirTime(world, e, at); e.moveInterpMode = EnemyInterp::Decelerate; break;
            case ecl_op::MoveVelocityInterpDecelerateFast: moveDirTime(world, e, at); e.moveInterpMode = EnemyInterp::DecelerateFast; break;
            case ecl_op::MoveVelocityInterpAccelerate: moveDirTime(world, e, at); e.moveInterpMode = EnemyInterp::Accelerate; break;
            case ecl_op::MoveVelocityInterpAccelerateFast: moveDirTime(world, e, at); e.moveInterpMode = EnemyInterp::AccelerateFast; break;
            case ecl_op::MovePositionInterpLinear: movePosTime(world, e, at); e.moveInterpMode = EnemyInterp::Linear; break;
            case ecl_op::MovePositionInterpDecelerate: movePosTime(world, e, at); e.moveInterpMode = EnemyInterp::Decelerate; break;
            case ecl_op::MovePositionInterpDecelerateFast: movePosTime(world, e, at); e.moveInterpMode = EnemyInterp::DecelerateFast; break;
            case ecl_op::MovePositionInterpAccelerate: movePosTime(world, e, at); e.moveInterpMode = EnemyInterp::Accelerate; break;
            case ecl_op::MovePositionInterpAccelerateFast: movePosTime(world, e, at); e.moveInterpMode = EnemyInterp::AccelerateFast; break;
            case ecl_op::MoveAsInterpDecelerate: moveTime(world, e, at); e.moveInterpMode = EnemyInterp::Decelerate; break;
            case ecl_op::MoveAsInterpDecelerateFast: moveTime(world, e, at); e.moveInterpMode = EnemyInterp::DecelerateFast; break;
            case ecl_op::MoveAsInterpAccelerate: moveTime(world, e, at); e.moveInterpMode = EnemyInterp::Accelerate; break;
            case ecl_op::MoveAsInterpAccelerateFast: moveTime(world, e, at); e.moveInterpMode = EnemyInterp::AccelerateFast; break;
            case ecl_op::MoveBoundsSet:
                e.lowerMoveLimitX = ecl.argAt<float>(at, 0);
                e.lowerMoveLimitY = ecl.argAt<float>(at, 4);
                e.upperMoveLimitX = ecl.argAt<float>(at, 8);
                e.upperMoveLimitY = ecl.argAt<float>(at, 12);
                e.shouldClampPos = true;
                break;
            case ecl_op::MoveBoundsDisable: e.shouldClampPos = false; break;
            case ecl_op::MoveRand: {
                const float lo = ecl.argAt<float>(at, 0), hi = ecl.argAt<float>(at, 4);
                e.angle = world.rng->f32InRange(hi - lo) + lo;
                break;
            }
            case ecl_op::MoveRandInBounds: {
                const float lo = ecl.argAt<float>(at, 0), hi = ecl.argAt<float>(at, 4);
                e.angle = world.rng->f32InRange(hi - lo) + lo;
                if (e.position.x < e.lowerMoveLimitX + 96.0f) {
                    if (e.angle > kZunHalfPi) e.angle = kZunPi - e.angle;
                    else if (e.angle < -kZunHalfPi) e.angle = -kZunPi - e.angle;
                }
                if (e.position.x > e.upperMoveLimitX - 96.0f) {
                    if (e.angle < kZunHalfPi && e.angle >= 0.0f) e.angle = kZunPi - e.angle;
                    else if (e.angle > -kZunHalfPi && e.angle <= 0.0f) e.angle = -kZunPi - e.angle;
                }
                if (e.position.y < e.lowerMoveLimitY + 48.0f && e.angle < 0.0f) e.angle = -e.angle;
                if (e.position.y > e.upperMoveLimitY - 48.0f && e.angle > 0.0f) e.angle = -e.angle;
                break;
            }
            case ecl_op::AnmSetPoses:
                e.anmPoseDefault = ecl.argAt<std::int16_t>(at, 0);
                e.anmPoseNeutralFromLeft = ecl.argAt<std::int16_t>(at, 2);
                e.anmPoseNeutralFromRight = ecl.argAt<std::int16_t>(at, 4);
                e.anmPoseLeft = ecl.argAt<std::int16_t>(at, 6);
                e.anmPoseRight = ecl.argAt<std::int16_t>(at, 8);
                e.anmPoseCurrent = 0xff;
                break;
            case ecl_op::EnemySetHitbox:
                e.hitboxDimensions = {ecl.argAt<float>(at, 0), ecl.argAt<float>(at, 4), ecl.argAt<float>(at, 8)};
                break;
            case ecl_op::EnemyFlagCollision: e.collidable = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::EnemyFlagCanTakeDamage: e.damageable = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::EffectSound: world.playSound(ecl.argAt<std::int32_t>(at, 0)); break;
            case ecl_op::EnemyFlagsDeath: e.deathMode = static_cast<EnemyDeath>(ecl.argAt<std::int32_t>(at, 0) & 7); break;
            case ecl_op::DeathCallbackSub: e.deathCallbackSub = ecl.argAt<std::int32_t>(at, 0); break;
            case ecl_op::EnemyInterruptSet: {
                const std::int32_t slot = ecl.argAt<std::int32_t>(at, 4);
                if (slot >= 0 && slot < 8) e.interrupts[static_cast<std::size_t>(slot)] = ecl.argAt<std::int32_t>(at, 0);
                break;
            }
            case ecl_op::EnemyInterrupt:
                e.runInterrupt = ecl.argAt<std::int32_t>(at, 0);
            run_interrupt: {
                e.ctx.currentInstr = at + static_cast<std::uint32_t>(cur.offsetToNext);
                if (!e.disableCallStack) e.savedContextStack[static_cast<std::size_t>(e.stackDepth)] = e.ctx;
                const std::int32_t slot = e.runInterrupt;
                const std::int32_t sub = slot >= 0 && slot < 8 ? e.interrupts[static_cast<std::size_t>(slot)] : -1;
                if (sub < 0 || sub >= ecl.subCount()) {
                    log::warn("ECL: interrupt {} has no sub; enemy removed", slot);
                    return false;
                }
                callSub(world, e.ctx, sub);
                if (e.stackDepth < Enemy::kMaxStackDepth) e.stackDepth++;
                e.runInterrupt = -1;
                goto restart_sub_changed;
            }
            case ecl_op::EnemyLifeSet: e.life = e.maxLife = ecl.argAt<std::int32_t>(at, 0); break;
            case ecl_op::SpellcardStart: {
                std::vector<std::uint8_t> name;  // NUL-terminated Shift-JIS after two shorts
                const std::span<const std::uint8_t> bytes = ecl.bytes();
                for (std::size_t i = at + sizeof(EclRawInstr) + 4; i < at + static_cast<std::size_t>(cur.offsetToNext) &&
                                                                   i < bytes.size() && bytes[i] != 0;
                     ++i) {
                    name.push_back(bytes[i]);
                }
                if (world.gui) world.gui->spellStart(world, ecl.argAt<std::int16_t>(at, 0), name, ecl.argAt<std::int16_t>(at, 2));
                if (world.background) world.background->spellStart();
                spellcard_.isCapturing = true;
                spellcard_.isActive = 1;
                spellcard_.idx = ecl.argAt<std::int16_t>(at, 2);
                spellcard_.captureScore = spellcardBaseBonus(spellcard_.idx);
                if (world.scores) world.scores->spellDeclared(spellcard_.idx, name, static_cast<std::uint32_t>(spellcard_.captureScore));
                world.bullets->removeAllBullets(world, true);
                e.resetRank();
                break;
            }
            case ecl_op::SpellcardEnd:
                if (world.background) world.background->spellEnd();
                if (spellcard_.isActive) {
                    if (world.gui && spellcard_.isActive <= 2) world.gui->spellNameExit();
                    if (spellcard_.isActive == 1) {
                        world.bullets->despawnBullets(world, 12800, true);
                        if (spellcard_.isCapturing) {  // capture: base + a tenth of it per second left
                            const std::int32_t bonus =
                                spellcard_.captureScore + spellcard_.captureScore * world.state->bossSeconds / 10;
                            if (world.gui) world.gui->showSpellBonus(bonus);
                            world.state->score += static_cast<std::uint32_t>(bonus);
                            ++world.state->spellsCaptured;
                            if (world.scores) world.scores->spellCaptured(spellcard_.idx, world.state->shotType);
                        }
                    }
                    spellcard_.isActive = 0;
                }
                break;
            case ecl_op::PhaseTimerSet: e.phaseTimer = ecl.argAt<std::int32_t>(at, 0); break;
            case ecl_op::LifeCallbackThreshold: e.lifeCallbackThreshold = ecl.argAt<std::int32_t>(at, 0); break;
            case ecl_op::LifeCallbackSub: e.lifeCallbackSub = ecl.argAt<std::int32_t>(at, 0); break;
            case ecl_op::TimerCallbackThreshold:
                e.timerCallbackThreshold = ecl.argAt<std::int32_t>(at, 0);
                e.phaseTimer = 0;
                break;
            case ecl_op::TimerCallbackSub: e.timerCallbackSub = ecl.argAt<std::int32_t>(at, 0); break;
            case ecl_op::EnemyFlagInteractable: e.interactable = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::EffectParticle:
                spawnEffect(world, ecl.argAt<std::int32_t>(at, 0), e.position, ecl.argAt<std::int32_t>(at, 4),
                            ecl.argAt<std::uint32_t>(at, 8));
                break;
            case ecl_op::DropItems: {
                const std::int32_t n = ecl.argAt<std::int32_t>(at, 0);  // literal, never a variable
                for (std::int32_t i = 0; i < n; ++i) {
                    Vec3f p = e.position;
                    p.x += world.rng->f32InRange(144.0f) - 72.0f;
                    p.y += world.rng->f32InRange(144.0f) - 72.0f;
                    const int type = world.state->power < GameState::kMaxPower ? (i == 0 ? ItemPowerBig : ItemPowerSmall)
                                                                               : ItemPoint;
                    if (world.items) world.items->spawn(world, p, type, ItemState::Falling);
                }
                break;
            }
            case ecl_op::AnmFlagRotation: e.rotateAnm = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::ExInsCall: runExInstruction(world, e, ecl.argAt<std::int32_t>(at, 0), at); break;
            case ecl_op::ExInsRepeat: {
                const std::int32_t idx = ecl.argAt<std::int32_t>(at, 0);
                e.ctx.funcSet = idx >= 0 ? idx : -1;
                break;
            }
            case ecl_op::EclTimeAdd: e.ctx.time += readInt(world, e, ecl.argAt<std::int32_t>(at, 0)); break;
            case ecl_op::DropItemId:
                if (world.items) world.items->spawn(world, e.position, ecl.argAt<std::int32_t>(at, 0), ItemState::Falling);
                break;
            case ecl_op::StdUnpause:
                if (world.background) world.background->unpause();
                break;
            case ecl_op::BossSetLifeCount:
                world.state->bossLifeCount = ecl.argAt<std::int32_t>(at, 0);
                world.state->completion += 1800;  // each boss life counts towards the completion rate
                break;
            case ecl_op::EnemyCreate: {
                const auto a = ecl.argAt<EclRawInstrEnemyCreateArgs>(at, 0);
                const Vec3f pos{readFloat(world, e, a.x), readFloat(world, e, a.y), readFloat(world, e, a.z)};
                spawnEnemy(world, a.subId, pos, a.life, a.itemDrop, a.score);
                break;
            }
            case ecl_op::EnemyKillAll: killAllNonBoss(world); break;
            case ecl_op::AnmInterruptMain: e.primaryVm.pendingInterrupt = static_cast<std::int16_t>(ecl.argAt<std::int32_t>(at, 0)); break;
            case ecl_op::AnmInterruptSlot: {
                const std::int32_t slot = ecl.argAt<std::int32_t>(at, 0);
                if (slot >= 0 && slot < Enemy::kAnmSlots) {
                    e.vms[static_cast<std::size_t>(slot)].pendingInterrupt = static_cast<std::int16_t>(ecl.argAt<std::int32_t>(at, 4));
                }
                break;
            }
            case ecl_op::BulletCancel: world.bullets->removeAllBullets(world, true); break;
            case ecl_op::BulletSound: {
                const std::int32_t sfx = ecl.argAt<std::int32_t>(at, 0);
                if (sfx >= 0) {
                    e.bulletProps.sfx = sfx;
                    e.bulletProps.flags |= ExSpawnSound;
                } else {
                    e.bulletProps.flags &= ~static_cast<std::uint32_t>(ExSpawnSound);
                }
                break;
            }
            case ecl_op::EnemyFlagDisableCallstack: e.disableCallStack = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::BulletRankInfluence:
                e.bulletRankSpeedLow = ecl.argAt<float>(at, 0);
                e.bulletRankSpeedHigh = ecl.argAt<float>(at, 4);
                e.bulletRankAmount1Low = static_cast<std::int16_t>(ecl.argAt<std::int32_t>(at, 8));
                e.bulletRankAmount1High = static_cast<std::int16_t>(ecl.argAt<std::int32_t>(at, 12));
                e.bulletRankAmount2Low = static_cast<std::int16_t>(ecl.argAt<std::int32_t>(at, 16));
                e.bulletRankAmount2High = static_cast<std::int16_t>(ecl.argAt<std::int32_t>(at, 20));
                break;
            case ecl_op::EnemyFlagInvisible: e.invisible = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::PhaseAdvance:
                e.timerCallbackSub = e.deathCallbackSub;
                e.phaseTimer = 0;
                break;
            case ecl_op::SpellcardFlagTimeout: e.timeoutSpell = ecl.argAt<std::int32_t>(at, 0) != 0; break;
            case ecl_op::Nop:
            case ecl_op::DebugWatch:
            default:
                break;
        }
    next_instruction:
        at += static_cast<std::uint32_t>(cur.offsetToNext);
        if (!ecl.instrAt(at, cur)) {
            log::warn("ECL sub {}: ran past the end of the file; enemy removed", e.ctx.subId);
            return false;
        }
    }

    switch (e.movementMode) {
        case EnemyMove::Velocity:
            e.angle = addNormalizeAngle(e.angle, e.angularVelocity);
            e.speed = e.acceleration + e.speed;
            sincosmul(e.axisSpeed, e.angle, e.speed);
            e.axisSpeed.z = 0.0f;
            break;
        case EnemyMove::Interp: {
            e.moveInterpTimer--;
            float t = static_cast<float>(e.moveInterpTimer) / static_cast<float>(e.moveInterpStartTime);
            if (t >= 1.0f) t = 1.0f;
            switch (e.moveInterpMode) {
                case EnemyInterp::Linear: t = 1.0f - t; break;
                case EnemyInterp::Decelerate: t = 1.0f - t * t; break;
                case EnemyInterp::DecelerateFast: t = 1.0f - t * t * t * t; break;
                case EnemyInterp::Accelerate: t = 1.0f - t; t *= t; break;
                case EnemyInterp::AccelerateFast: t = 1.0f - t; t = t * t * t * t; break;
            }
            e.axisSpeed = e.moveInterp * t + e.moveInterpStartPos - e.position;
            e.angle = std::atan2(e.axisSpeed.y, e.axisSpeed.x);
            if (e.moveInterpTimer <= 0) {
                e.movementMode = EnemyMove::AxisSpeed;
                e.position = e.moveInterpStartPos + e.moveInterp;
                e.axisSpeed = {};
            }
            break;
        }
        default:
            break;
    }

    if (e.life > 0) {
        if (e.shootInterval > 0) {
            e.shootIntervalTimer++;
            if (e.shootIntervalTimer >= e.shootInterval) {
                e.bulletProps.position = e.position + e.shootOffset;
                world.bullets->spawnBulletPattern(world, e.bulletProps);
                e.shootIntervalTimer = 0;
            }
        }
        if (e.anmPoseLeft >= 0) {
            const std::int16_t pose = e.axisSpeed.x < 0.0f ? 1 : e.axisSpeed.x > 0.0f ? 2 : 0;
            if (e.anmPoseCurrent != pose) {
                int script;
                if (pose == 0) {
                    script = e.anmPoseCurrent == 0xff ? e.anmPoseDefault
                             : e.anmPoseCurrent == 1  ? e.anmPoseNeutralFromLeft
                                                      : e.anmPoseNeutralFromRight;
                } else {
                    script = pose == 1 ? e.anmPoseLeft : e.anmPoseRight;
                }
                world.anm->setAndExecuteScript(e.primaryVm, script + kAnmOffsetEnemy);
                e.anmPoseCurrent = pose;
            }
        }
        if (e.ctx.funcSet >= 0) runExInstruction(world, e, e.ctx.funcSet, 0);
    }
    e.ctx.currentInstr = at;
    e.ctx.time++;
    return true;
}

// ---- callbacks, timeline, update -----------------------------------------------------

Laser* EnemyManager::laserRef(World& world, Enemy& e, std::int32_t slot) {
    if (slot < 0 || slot >= static_cast<std::int32_t>(e.laserRefs.size())) return nullptr;
    const std::int16_t index = e.laserRefs[static_cast<std::size_t>(slot)];
    return index >= 0 ? &world.bullets->laser(index) : nullptr;
}

void EnemyManager::dropDeathItem(World& world, Enemy& e, bool hitByBomb) {
    if (!world.items) return;
    const ItemState state = hitByBomb ? ItemState::Magnet : ItemState::Falling;  // bomb kills drop homing items
    if (e.itemDrop >= 0) {
        spawnEffect(world, e.deathParticle2 + 4, e.position, 3);
        world.items->spawn(world, e.position, e.itemDrop, state);
    } else if (e.itemDrop == kItemRandom) {
        if (randomItemSpawnIndex_ % 3 == 0) {
            spawnEffect(world, e.deathParticle2 + 4, e.position, 6);
            world.items->spawn(world, e.position, kRandomItems[randomItemTableIndex_], state);
            randomItemTableIndex_ = static_cast<std::uint16_t>((randomItemTableIndex_ + 1) % 32);
        }
        ++randomItemSpawnIndex_;
    }
}

void EnemyManager::killAllNonBoss(World& world) {
    for (Enemy& other : *enemies_) {
        if (!other.slotOccupied || other.isBoss) continue;
        other.life = 0;
        if (!other.interactable && other.deathCallbackSub >= 0) {
            callSub(world, other.ctx, other.deathCallbackSub);
            other.deathCallbackSub = -1;
        }
    }
}

bool EnemyManager::handleLifeCallback(World& world, Enemy& e) {
    if (e.life >= e.lifeCallbackThreshold) return false;
    e.life = e.lifeCallbackThreshold;
    callSub(world, e.ctx, e.lifeCallbackSub);
    e.lifeCallbackThreshold = -1;
    e.timerCallbackSub = e.deathCallbackSub;
    e.resetRank();
    e.stackDepth = 0;
    killAllNonBoss(world);
    return true;
}

bool EnemyManager::handleTimerCallback(World& world, Enemy& e) {
    if (e.isBoss) world.state->bossSeconds = (e.timerCallbackThreshold - e.phaseTimer) / 60;
    if (e.phaseTimer < e.timerCallbackThreshold) return false;
    if (e.lifeCallbackThreshold > 0) {
        e.life = e.lifeCallbackThreshold;
        e.lifeCallbackThreshold = -1;
    }
    callSub(world, e.ctx, e.timerCallbackSub);
    e.timerCallbackThreshold = -1;
    e.timerCallbackSub = e.deathCallbackSub;
    e.phaseTimer = 0;
    if (!e.timeoutSpell) {
        spellcard_.isCapturing = false;
        if (spellcard_.isActive) spellcard_.isActive++;
        world.bullets->removeAllBullets(world, false);
    }
    killAllNonBoss(world);
    e.resetRank();
    e.stackDepth = 0;
    return true;
}

std::int32_t EnemyManager::spellcardBaseBonus(int idx) {
    if (idx < 7) return 200'000;   // stages 1-2
    if (idx < 14) return 250'000;  // stage 3
    if (idx < 32) return 300'000;  // stage 4
    if (idx < 40) return 400'000;  // stage 5
    if (idx < 46) return 500'000;  // stage 6
    if (idx < 51) return 600'000;  // stage 6 (last cards)
    return 700'000;                // Extra
}

void EnemyManager::releaseEffects(World& world, Enemy& e) {
    for (int i = 0; i < e.effectCount; ++i) {
        if (world.effects) world.effects->at(e.effectRefs[static_cast<std::size_t>(i)]).fading = true;
    }
    e.effectCount = 0;
}

// Spell circles follow their enemy, grow to its radius and turn 1/200 of a circle a frame.
void EnemyManager::driveEffects(World& world, Enemy& e) {
    if (!world.effects) return;
    for (int i = 0; i < e.effectCount; ++i) {
        EffectManager::Effect& fx = world.effects->at(e.effectRefs[static_cast<std::size_t>(i)]);
        fx.anchor = e.position;
        if (fx.radius < e.effectDistance) fx.radius += 0.3f;
        float a = fx.halfAngle + kZunPi / 100.0f;
        while (a > kZunPi) a -= kZunTwoPi;
        while (a < -kZunPi) a += kZunTwoPi;
        fx.halfAngle = a;
    }
}

// Red particles drifting off both sides of the enemy along a sweeping angle, firing more
// often the longer it runs.
void EnemyManager::batWingTrail(World& world, Enemy& e) {
    if (e.invisible) {
        releaseEffects(world, e);
        return;
    }
    e.batAngle += kZunPi / 180.0f;
    if (e.batAngle >= kZunPi / 4.0f) e.batAngle -= kZunHalfPi;
    const std::int32_t t = e.batTimer;
    if (e.batTimerTicked && (t > 120 || (t > 60 && t % 2 == 0) || (t > 30 && t % 4 == 0) || t % 8 == 0)) {
        const int h = (t % 16) / 2;
        const int b = h > 0 ? h + world.rng->u16InRange(static_cast<std::uint16_t>(h)) : 0;
        const float dist = static_cast<float>(b) * 160.0f / 16.0f + 32.0f;
        const float ang = e.batAngle - static_cast<float>(b) * kZunPi / 40.0f;
        EffectManager::Effect unused;
        for (const float side : {1.0f, -1.0f}) {
            const Vec3f p{e.position.x + side * std::cos(ang) * dist, e.position.y + std::sin(ang) * dist, e.position.z};
            const int slot = spawnEffect(world, EffectDrift, p, 1, 0xff3030ffu);
            EffectManager::Effect& fx = world.effects ? world.effects->at(slot) : unused;
            fx.velocity = {(world.rng->f32InRange(40.0f) - 20.0f) / 60.0f, 8.0f * static_cast<float>(b) / 60.0f - 4.0f / 15.0f,
                           0.0f};
            fx.acceleration = fx.velocity / -120.0f;
        }
    }
    ++e.batTimer;
    e.batTimerTicked = true;
}

void EnemyManager::despawn(World& world, Enemy& e) {
    if (e.deathMode == EnemyDeath::DespawnNoCallback) e.slotOccupied = false;
    else e.interactable = false;
    if (e.isBoss) world.state->bossPresent = false;
}

void EnemyManager::runTimeline(World& world) {
    bossWaiting_ = false;
    const EclFile& ecl = *world.ecl;
    GameState& gs = *world.state;
    // Subrank rises at an interval that shortens with lives remaining (paused during dialogue).
    const bool talking = world.gui && world.gui->msgActive();
    const std::int32_t interval = 10 * 4 * 60 - gs.livesRemaining * 4 * 60;
    if (!talking && timelineTime_ != prevTimelineTime_ && interval > 0 && timelineTime_ % interval == 0) gs.increaseSubrank(100);
    prevTimelineTime_ = timelineTime_;

    TimelineInstr t;
    TimelineInstrArgs args;
    while (timelineInstr_ < ecl.timelineEnd() && ecl.timelineAt(timelineInstr_, t, args) && t.time >= 0) {
        if (timelineTime_ == t.time) {
            Vec3f pos{std::bit_cast<float>(args.uintVar1), std::bit_cast<float>(args.uintVar2),
                      std::bit_cast<float>(args.uintVar3)};
            const auto life = static_cast<std::int16_t>(args.ushortVar1);
            const auto item = static_cast<std::int16_t>(args.ushortVar2);
            const auto score = static_cast<std::int32_t>(args.uintVar4);
            auto randomise = [&](Vec3f p) {
                if (p.x <= -990.0f) p.x = world.rng->f32InRange(kGameRegionWidth - 16.0f);
                if (p.y <= -990.0f) p.y = world.rng->f32InRange(kGameRegionHeight - 32.0f);
                if (p.z <= -990.0f) p.z = world.rng->f32InRange(800.0f);
                return p;
            };
            Enemy* spawned = nullptr;
            switch (t.opCode) {
                case timeline_op::EnemyCreate:
                    if (!gs.bossPresent) spawnEnemy(world, t.arg0, pos, life, item, score);
                    break;
                case timeline_op::DummyCreate:
                    if (!gs.bossPresent) spawnEnemy(world, t.arg0, pos, -1, kItemRandom, -1);
                    break;
                case timeline_op::EnemyCreateMirror:
                    if (!gs.bossPresent && (spawned = spawnEnemy(world, t.arg0, pos, life, item, score))) spawned->mirrored = true;
                    break;
                case timeline_op::DummyCreateMirror:
                    if (!gs.bossPresent && (spawned = spawnEnemy(world, t.arg0, pos, -1, kItemRandom, -1))) spawned->mirrored = true;
                    break;
                case timeline_op::EnemyCreateRandom:
                    if (!gs.bossPresent) spawnEnemy(world, t.arg0, randomise(pos), life, item, score);
                    break;
                case timeline_op::DummyCreateRandom:
                    if (!gs.bossPresent) spawnEnemy(world, t.arg0, randomise(pos), -1, kItemRandom, -1);
                    break;
                case timeline_op::EnemyCreateMirrorRandom:
                    if (!gs.bossPresent && (spawned = spawnEnemy(world, t.arg0, randomise(pos), life, item, score))) {
                        spawned->mirrored = true;
                    }
                    break;
                case timeline_op::DummyCreateMirrorRandom:
                    if (!gs.bossPresent && (spawned = spawnEnemy(world, t.arg0, randomise(pos), -1, kItemRandom, -1))) {
                        spawned->mirrored = true;
                    }
                    break;
                case timeline_op::MsgRead:
                    if (world.gui && world.gui->hasDialogue()) world.gui->msgRead(world, t.arg0);
                    else world.unported->hit("dialogue (MSG scripts)");
                    break;
                case timeline_op::MsgWait:
                    if (world.gui && world.gui->msgBlocksTimeline()) {
                        timelineTime_--;
                        return;
                    }
                    break;
                case timeline_op::BossInterrupt: {
                    Enemy* b = boss(static_cast<int>(args.uintVar1));
                    if (b) b->runInterrupt = static_cast<std::int32_t>(args.uintVar2);
                    break;
                }
                case timeline_op::PlayerPower:
                    gs.power = t.arg0;
                    break;
                case timeline_op::BossWait: {
                    Enemy* b = boss(t.arg0);
                    if (b && b->slotOccupied) {
                        timelineTime_--;
                        bossWaiting_ = true;
                        return;
                    }
                    break;
                }
                default:
                    break;
            }
        } else if (timelineTime_ < t.time) {
            break;
        }
        timelineInstr_ += static_cast<std::uint32_t>(t.size);
    }
    if (timelineInstr_ >= ecl.timelineEnd()) timelineFinished_ = true;
}

void EnemyManager::update(World& world) {
    if (!world.ecl) return;
    GameState& gs = *world.state;
    runTimeline(world);
    // The completion rate counts frames of play outside dialogue and boss waits.
    if (!(world.gui && world.gui->msgActive()) && !bossWaiting_) ++gs.completion;
    int index = 0;
    for (Enemy& e : *enemies_) {
        const int enemyIdx = index++;
        if (!e.slotOccupied) continue;
        e.prevPosition = e.position;

        // Move
        e.position.x += e.mirrored ? -e.axisSpeed.x : e.axisSpeed.x;
        e.position.y += e.axisSpeed.y;
        e.position.z += e.axisSpeed.z;
        e.clampPos();
        const AnmLoadedSprite* s = world.anm->sprite(e.primaryVm);
        const float w = s ? s->widthPx : 0.0f, h = s ? s->heightPx : 0.0f;
        if (!e.hasBeenInBounds && gs.isInBounds(e.position.x, e.position.y, w, h)) e.hasBeenInBounds = true;
        if (e.hasBeenInBounds && !gs.isInBounds(e.position.x, e.position.y, w, h)) {
            e.slotOccupied = false;
            releaseEffects(world, e);
            despawn(world, e);
            continue;
        }
        if (e.lifeCallbackThreshold >= 0) handleLifeCallback(world, e);
        if (e.timerCallbackThreshold >= 0) handleTimerCallback(world, e);
        if (!runEcl(world, e)) {
            e.slotOccupied = false;
            releaseEffects(world, e);
            despawn(world, e);
            continue;
        }
        e.clampPos();
        e.primaryVm.color = e.color;
        world.anm->executeScript(e.primaryVm);
        e.color = e.primaryVm.color;
        for (AnmVm& vm : e.vms) {
            if (vm.anmFileIndex >= 0 && world.anm->executeScript(vm)) vm.anmFileIndex = -1;
        }

        if (e.hasBeenInBounds && !e.invisible) {
            const std::int32_t lifeBefore = e.life;
            if (e.collidable && e.interactable) {
                const Vec3f box = e.hitboxDimensions / 1.5f;
                if (world.player->checkCollision(world, e.position, box) == Player::CollisionHit && e.interactable &&
                    !e.isBoss) {
                    e.life -= 10;
                }
            }
            bool hitByBomb = false;
            if (e.interactable) {
                int damage = world.player->calcDamageToEnemy(world, e.position, e.hitboxDimensions, &hitByBomb);
                world.player->considerHomingTarget(e.position);
                if (damage >= 70) damage = 70;
                gs.score = static_cast<std::uint32_t>((damage / 5) * 10) + gs.score;
                if (spellcard_.isActive) {
                    if (!hitByBomb) {
                        if (damage > 7) damage /= 7;
                        else if (damage != 0) damage = 1;
                    } else if (spellcard_.usedBomb) {
                        if (damage > 3) damage /= 3;
                        else if (damage != 0) damage = 1;
                    } else {
                        damage = 0;
                    }
                }
                if (e.damageable) e.life -= damage;
            }
            if (e.life <= 0 && e.interactable) {
                e.lifeCallbackThreshold = -1;
                e.timerCallbackThreshold = -1;
                switch (e.deathMode) {
                    case EnemyDeath::SetHpTo1:
                        e.life = 1;
                        e.damageable = false;
                        e.deathMode = EnemyDeath::DespawnNoCallback;
                        gs.bossPresent = false;
                        for (int k = 0; k < 3; ++k) spawnEffect(world, e.deathParticle1, e.position, 1);
                        break;
                    case EnemyDeath::DisableInteraction:
                        gs.score += static_cast<std::uint32_t>(e.score);
                        e.interactable = false;
                        if (e.isBoss) {
                            gs.bossPresent = false;
                            releaseEffects(world, e);
                        }
                        goto drop_items;
                    case EnemyDeath::DespawnNoCallback:
                        gs.score += static_cast<std::uint32_t>(e.score);
                        e.slotOccupied = false;
                        if (e.isBoss) {
                            gs.bossPresent = false;
                            releaseEffects(world, e);
                        }
                        [[fallthrough]];
                    case EnemyDeath::DropItemsOnly:
                    drop_items:
                        dropDeathItem(world, e, hitByBomb);
                        if (e.isBoss && !spellcard_.isActive) world.bullets->despawnBullets(world, 12800, false);
                        e.life = 0;
                        break;
                }
                world.playSound((enemyIdx % 2) + kSound2);
                spawnEffect(world, e.deathParticle1, e.position, 1);
                spawnEffect(world, e.deathParticle2 + 4, e.position, 4);
                if (e.deathCallbackSub >= 0) {
                    e.resetRank();
                    e.stackDepth = 0;
                    callSub(world, e.ctx, e.deathCallbackSub);
                    e.deathCallbackSub = -1;
                }
            }
            // The boss HUD's bar follows the boss (the last boss in slot order wins).
            if (e.isBoss && e.maxLife > 0 && !(world.gui && world.gui->msgActive())) gs.bossFillTarget = static_cast<float>(e.life) / static_cast<float>(e.maxLife);
            if (e.hitFlashTimer != 0) {
                e.hitFlashTimer--;
                e.primaryVm.colorAdd = false;
            } else if (lifeBefore > e.life) {
                world.playSound(kSoundTotalBossDeath);
                e.primaryVm.colorAdd = true;
                e.hitFlashTimer = 4;
            } else {
                e.primaryVm.colorAdd = false;
            }
        }
        driveEffects(world, e);
        if (gs.timeStop == 0) e.phaseTimer++;  // spell timers pause during a time stop
    }
    timelineTime_++;
}

void EnemyManager::draw(World& world, SpriteSink& sink, float alpha) {
    for (Enemy& e : *enemies_) {
        if (!e.slotOccupied || e.invisible) continue;
        const Vec3f p = lerp(e.prevPosition, e.position, alpha);
        auto drawSlot = [&](AnmVm& vm) {
            if (vm.anmFileIndex < 0) return;
            if (vm.autoRotate != 0) vm.rotation.z = e.angle;
            vm.pos = p + vm.posOffset;
            vm.pos.z = 0.495f;
            world.anm->drawWorld(sink, vm, world.state->playfieldX, world.state->playfieldY);
        };
        for (int i = 0; i < Enemy::kAnmSlots / 2; ++i) drawSlot(e.vms[static_cast<std::size_t>(i)]);
        if (e.rotateAnm) e.primaryVm.rotation.z = e.angle;
        e.primaryVm.pos = p + e.primaryVm.posOffset;
        e.primaryVm.pos.z = 0.494f;
        world.anm->drawWorld(sink, e.primaryVm, world.state->playfieldX, world.state->playfieldY);
        for (int i = Enemy::kAnmSlots / 2; i < Enemy::kAnmSlots; ++i) drawSlot(e.vms[static_cast<std::size_t>(i)]);
    }
}

}  // namespace th06
