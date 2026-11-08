#include "th07/Ecl.hpp"

#include <algorithm>
#include <format>
#include <set>

#include "core/Log.hpp"
#include "th07/Background.hpp"
#include "th07/Bullets.hpp"
#include "th07/Effects.hpp"
#include "th07/Enemies.hpp"
#include "th07/ExeTables.hpp"
#include "th07/Player.hpp"

namespace touhou::th07 {

void eclWarnOnce(const std::string& message) {
    static std::set<std::string> seen;
    if (seen.insert(message).second) log::warn("ECL: {}", message);
}

float eclEase(int mode, float t) {
    switch (mode) {
        case 1: return t * t;
        case 2: return t * t * t;
        case 3: return t * t * t * t;
        case 4: {
            float u = 1.0f - t;
            u = u * u;
            return 1.0f - u;
        }
        case 5: {
            float u = 1.0f - t;
            u = u * u * u;
            return 1.0f - u;
        }
        case 6: {
            float u = 1.0f - t;
            u = u * u * u * u;
            return 1.0f - u;
        }
        default: return t;
    }
}

// ---- the file -------------------------------------------------------------------------------

void EclFile::clear() {
    bytes_.clear();
    size_ = 0;
    subs_ = timelines_ = 0;
    subOffsets_.clear();
    timelineOffsets_.fill(0);
}

bool EclFile::validInstruction(std::uint32_t off) const {
    if (static_cast<std::uint64_t>(off) + 12 > size_) return false;
    const std::uint16_t s = eclU16(bytes_.data() + off + 6);
    return s >= 12 && static_cast<std::uint64_t>(off) + s <= size_;
}

bool EclFile::load(std::span<const std::uint8_t> b, std::string* error) {
    clear();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        clear();
        return false;
    };
    if (b.size() < kHeaderSize || b.size() > 0x7FFFFFFF) return fail("ECL header cut off");
    const std::int16_t subs = eclI16(b.data()), timelines = eclI16(b.data() + 2);
    if (subs < 0 || timelines < 0 || timelines > kTimelines) return fail("bad ECL header counts");
    if (kHeaderSize + 4ull * static_cast<unsigned>(subs) > b.size()) return fail("ECL sub table cut off");
    const auto n = static_cast<std::uint32_t>(b.size());
    for (int i = 0; i < timelines; ++i) {
        const std::uint32_t off = static_cast<std::uint32_t>(eclI32(b.data() + 4 + 4 * i));
        if (static_cast<std::uint64_t>(off) + 8 > n) return fail(std::format("timeline {} outside the file", i));
        timelineOffsets_[static_cast<std::size_t>(i)] = off;
    }
    subOffsets_.resize(static_cast<std::size_t>(subs));
    for (int i = 0; i < subs; ++i) {
        const std::uint32_t off = static_cast<std::uint32_t>(eclI32(b.data() + kHeaderSize + 4 * i));
        if (static_cast<std::uint64_t>(off) + 12 > n) return fail(std::format("sub {} outside the file", i));
        subOffsets_[static_cast<std::size_t>(i)] = off;
    }
    bytes_.assign(b.begin(), b.end());
    bytes_.resize(b.size() + kPad, 0);
    size_ = n;
    subs_ = subs;
    timelines_ = timelines;
    audit();
    return true;
}

// Load-time checks of things the decompilation could not settle (spec 2.4, 3.3, 6.7):
// unknown opcodes, the argument of every "restore game speed" call (a value >= 2 or < 0 sets
// the sticky force-integer timer flag), and side-effecting variables (random numbers) in
// positions that are read more than once or together with another argument.
void EclFile::audit() {
    auto randomId = [](std::uint32_t word) {
        float f;
        std::memcpy(&f, &word, 4);
        const auto i = static_cast<std::int32_t>(word);
        const std::int32_t t = x86ToInt(f);
        return i == 10055 || i == 10056 || i == 10060 || t == 10055 || t == 10056 || t == 10060;
    };
    // Words read more than once (any random variable there matters), and words read inside
    // one expression or call (two random variables there would need the unknown order).
    auto multiRead = [](int op) -> std::uint32_t {
        switch (op) {
            case 3: return 1u << 2;
            case 54: return (1u << 0) | (1u << 3);
            case 97: return 0x3;
            case 99: case 81: case 122: case 145: case 148: return 0x1;
            case 151: return (1u << 2) | (1u << 3);
            case 159: return 1u << 2;
            default: return 0;
        }
    };
    auto sameExpression = [](int op) -> std::uint32_t {
        switch (op) {
            case 7: case 9: case 12: case 13: case 14: case 15: case 16: case 19: case 20: case 21: case 22: case 23:
                return 0x6;
            case 26: return 0x1E;
            case 151: case 159: return 0xE;
            case 92: case 93: return 0x70;
            case 139: return 0x6;
            case 117: case 118: return 0x3;
            case 43: case 44: return 0x6;
            default: return 0;
        }
    };
    int restores = 0, forcing = 0;
    for (int s = 0; s < subs_; ++s) {
        std::uint32_t off = subOffsets_[static_cast<std::size_t>(s)];
        for (int count = 0; count < 200000 && validInstruction(off); ++count) {
            const std::uint8_t* p = bytes_.data() + off;
            const std::int32_t time = eclI32(p);
            const std::int16_t op = eclI16(p + 4);
            const std::uint16_t size = eclU16(p + 6), mask = eclU16(p + 10);
            if (op == -1) break;  // the end marker (time -1) closing every sub
            if (op > 161 || op < 0) eclWarnOnce(std::format("sub {} uses opcode {}, which this port does not know", s, op));
            if ((op == 121 || op == 122) && size >= 20 && eclI32(p + 12) == 11 && !(mask & 1)) {
                ++restores;
                const std::int32_t a = eclI32(p + 16);
                if (a >= 2 || a < 0) ++forcing;
            }
            std::uint32_t random = 0;
            for (int k = 0; k < 8 && 12u + 4u * static_cast<unsigned>(k) + 4u <= size; ++k) {
                std::uint32_t word;
                std::memcpy(&word, p + 12 + 4 * k, 4);
                if ((mask & (1u << k)) && randomId(word)) random |= 1u << k;
            }
            const std::uint32_t group = random & sameExpression(op);
            if ((random & multiRead(op)) || (group & (group - 1))) {
                eclWarnOnce(std::format("sub {} offset {:#x}: opcode {} reads random variables (mask {:#x}) in an order the "
                                        "specification does not settle",
                                        s, off, op, random));
            }
            if (time == -1) break;
            off += size;
        }
    }
    if (forcing) {
        log::warn("ECL: {} of {} restore-game-speed calls have an argument >= 2 or < 0 (they set the force-integer timer flag)",
                  forcing, restores);
    }
}

// ---- operands ------------------------------------------------------------------------------

// Argument access for one instruction (spec 1.5): I/F resolve variables when the mask bit is
// set; IW/FW destinations are a writable variable or else the argument word itself.
struct EclOps {
    EnemyManager& m;
    Enemy& e;
    std::uint8_t* ins;
    std::uint16_t mask;

    std::uint8_t* word(int k) const { return ins + 12 + 4 * k; }
    bool bit(int k) const { return (mask >> k) & 1u; }
    std::int32_t L(int k) const { return eclI32(word(k)); }
    float LF(int k) const { return eclF32(word(k)); }
    std::uint8_t B(int k) const { return word(k)[0]; }
    std::int16_t H(int k, int h) const { return eclI16(word(k) + 2 * h); }
    // Reads with an explicit mask bit (the bullet and laser layouts are shifted).
    std::int32_t IB(int k, int b) const { return ((mask >> b) & 1u) ? m.readInt(e, L(k)) : L(k); }
    float FB(int k, int b) const { return ((mask >> b) & 1u) ? m.readFloat(e, LF(k)) : LF(k); }
    std::int32_t I(int k) const { return IB(k, k); }
    float F(int k) const { return FB(k, k); }
    std::int32_t getIW(int k) const { return bit(k) && EnemyManager::intWritable(L(k)) ? m.readInt(e, L(k)) : L(k); }
    void setIW(int k, std::int32_t v) const {
        if (bit(k) && m.writeInt(e, L(k), v)) return;
        eclPutI32(word(k), v);
    }
    float getFW(int k) const { return bit(k) && EnemyManager::floatWritable(x86ToInt(LF(k))) ? m.readFloat(e, LF(k)) : LF(k); }
    void setFW(int k, float v) const {
        if (bit(k) && m.writeFloat(e, x86ToInt(LF(k)), v)) return;
        eclPutF32(word(k), v);
    }
};

// ---- the variable space --------------------------------------------------------------------

float EnemyManager::angleToPlayer(const Vec3f& from) const {
    const Vec3f p = w_ && w_->player ? w_->player->position() : Vec3f{192.0f, 384.0f, 0.0f};
    const float dx = p.x - from.x, dy = p.y - from.y;
    if (dx == 0.0f && dy == 0.0f) return kZunPi / 2.0f;
    return zatan2(dy, dx);
}

namespace {

Vec3f playerPos(const World& w) { return w.player ? w.player->position() : Vec3f{192.0f, 384.0f, 0.0f}; }

float playerDistance(const World& w, const Vec3f& from) {
    const Vec3f p = playerPos(w);
    const float dx = p.x - from.x, dy = p.y - from.y, dz = p.z - from.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

std::int32_t EnemyManager::readInt(Enemy& e, std::int32_t id) {
    const World& wd = w();
    GameState& g = *wd.state;
    EclArgs& a = e.ctx.args;
    if (id >= 10000 && id <= 10003) return a.intA[static_cast<std::size_t>(id - 10000)];
    if (id >= 10004 && id <= 10011) return x86ToInt(a.floatA[static_cast<std::size_t>(id - 10004)]);
    if (id >= 10012 && id <= 10015) return a.intB[static_cast<std::size_t>(id - 10012)];
    if (id >= 10029 && id <= 10032) return a.intG[static_cast<std::size_t>(id - 10029)];
    if (id >= 10033 && id <= 10036) return x86ToInt(a.floatG[static_cast<std::size_t>(id - 10033)]);
    if (id >= 10037 && id <= 10040) return g_eclGlobals.ints[static_cast<std::size_t>(id - 10037)];
    if (id >= 10041 && id <= 10044) return x86ToInt(g_eclGlobals.floats[static_cast<std::size_t>(id - 10041)]);
    if (id >= 10066 && id <= 10069) return e.lifeThresholds[static_cast<std::size_t>(id - 10066)];
    switch (id) {
        case 10016: return g.difficulty;
        case 10017: return g.rank;
        case 10018: return x86ToInt(e.pos.x);
        case 10019: return x86ToInt(e.pos.y);
        case 10020: return x86ToInt(e.pos.z);
        case 10021: return x86ToInt(playerPos(wd).x);
        case 10022: return x86ToInt(playerPos(wd).y);
        case 10023: return x86ToInt(playerPos(wd).z);
        case 10024: return x86ToInt(angleToPlayer(e.pos));
        case 10025: return e.timer.current;
        case 10026: return x86ToInt(playerDistance(wd, e.pos));
        case 10027: return e.life;
        case 10028: return g.shotIndex;
        case 10045: return x86ToInt(e.angle);
        case 10046: return x86ToInt(e.angularVelocity);
        case 10047: return x86ToInt(e.speed);
        case 10048: return x86ToInt(e.accel);
        case 10049: return x86ToInt(e.orbitRadius);
        case 10050: return x86ToInt(e.moveOrigin.x);
        case 10051: return x86ToInt(e.moveOrigin.y);
        case 10052: return x86ToInt(e.moveOrigin.z);
        case 10053: return x86ToInt(e.orbitAngle);
        case 10054: return x86ToInt(e.orbitAngularVelocity);
        case 10055: return static_cast<std::int32_t>(wd.rng->u32());
        case 10056:
            return static_cast<std::int32_t>(wd.rng->u32InRange(static_cast<std::uint32_t>(a.intG[0]))) + a.intG[1];
        case 10061: return e.damageThisFrame;
        case 10062: return e.bossId;
        case 10063: return x86ToInt(e.delta.x);
        case 10064: return x86ToInt(e.delta.y);
        case 10065: return x86ToInt(e.delta.z);
        case 10070: return e.itemDrop;
        case 10071: return e.score;
        case 10072: return x86ToInt(a.floatB[0]);
        case 10073: return x86ToInt(a.floatB[1]);
        default: return id;  // unknown ids (10057-10060 included) read as themselves [QUIRK]
    }
}

float EnemyManager::readFloat(Enemy& e, float raw) {
    const World& wd = w();
    GameState& g = *wd.state;
    EclArgs& a = e.ctx.args;
    const std::int32_t id = x86ToInt(raw);
    if (id >= 10000 && id <= 10003) return static_cast<float>(a.intA[static_cast<std::size_t>(id - 10000)]);
    if (id >= 10004 && id <= 10011) return a.floatA[static_cast<std::size_t>(id - 10004)];
    if (id >= 10012 && id <= 10015) return static_cast<float>(a.intB[static_cast<std::size_t>(id - 10012)]);
    if (id >= 10029 && id <= 10032) return static_cast<float>(a.intG[static_cast<std::size_t>(id - 10029)]);
    if (id >= 10033 && id <= 10036) return a.floatG[static_cast<std::size_t>(id - 10033)];
    if (id >= 10037 && id <= 10040) return static_cast<float>(g_eclGlobals.ints[static_cast<std::size_t>(id - 10037)]);
    if (id >= 10041 && id <= 10044) return g_eclGlobals.floats[static_cast<std::size_t>(id - 10041)];
    if (id >= 10066 && id <= 10069) return static_cast<float>(e.lifeThresholds[static_cast<std::size_t>(id - 10066)]);
    switch (id) {
        case 10016: return static_cast<float>(g.difficulty);
        case 10017: return static_cast<float>(g.rank);
        case 10018: return e.pos.x;
        case 10019: return e.pos.y;
        case 10020: return e.pos.z;
        case 10021: return playerPos(wd).x;
        case 10022: return playerPos(wd).y;
        case 10023: return playerPos(wd).z;
        case 10024: return angleToPlayer(e.pos);
        case 10025: return static_cast<float>(e.timer.current);
        case 10026: return playerDistance(wd, e.pos);
        case 10027: return static_cast<float>(e.life);
        case 10028: return static_cast<float>(g.shotIndex);
        case 10045: return e.angle;
        case 10046: return e.angularVelocity;
        case 10047: return e.speed;
        case 10048: return e.accel;
        case 10049: return e.orbitRadius;
        case 10050: return e.moveOrigin.x;
        case 10051: return e.moveOrigin.y;
        case 10052: return e.moveOrigin.z;
        case 10053: return e.orbitAngle;
        case 10054: return e.orbitAngularVelocity;
        case 10055: return wd.rng->f32ZeroToOne();
        case 10056: return wd.rng->f32InRange(a.floatG[0]) + a.floatG[1];
        case 10057: return e.moveDelta.x;
        case 10058: return e.moveDelta.y;
        case 10059: return e.moveDelta.z;
        case 10060: return wd.rng->f32InRange(kZunTwoPi) - kZunPi;
        case 10061: return static_cast<float>(e.damageThisFrame);
        case 10062: return static_cast<float>(e.bossId);
        case 10063: return e.delta.x;
        case 10064: return e.delta.y;
        case 10065: return e.delta.z;
        case 10070: return static_cast<float>(e.itemDrop);
        case 10071: return static_cast<float>(e.score);
        case 10072: return a.floatB[0];
        case 10073: return a.floatB[1];
        default: return raw;  // unknown: the argument word itself
    }
}

bool EnemyManager::intWritable(std::int32_t id) {
    return (id >= 10000 && id <= 10003) || (id >= 10012 && id <= 10017) || id == 10025 || id == 10027 ||
           (id >= 10029 && id <= 10032) || (id >= 10037 && id <= 10040) || id == 10070 || id == 10071;
}

bool EnemyManager::floatWritable(std::int32_t id) {
    return (id >= 10004 && id <= 10011) || (id >= 10018 && id <= 10023) || (id >= 10033 && id <= 10036) ||
           (id >= 10041 && id <= 10054) || (id >= 10057 && id <= 10059) || id == 10072 || id == 10073;
}

bool EnemyManager::writeInt(Enemy& e, std::int32_t id, std::int32_t v) {
    EclArgs& a = e.ctx.args;
    GameState& g = *w().state;
    if (id >= 10000 && id <= 10003) a.intA[static_cast<std::size_t>(id - 10000)] = v;
    else if (id >= 10012 && id <= 10015) a.intB[static_cast<std::size_t>(id - 10012)] = v;
    else if (id == 10016) g.difficulty = v;  // the difficulty bit of the masks stays as computed at stage start [QUIRK]
    else if (id == 10017) g.rank = v;        // [QUIRK]
    else if (id == 10025) e.timer.current = v;
    else if (id == 10027) e.life = v;
    else if (id >= 10029 && id <= 10032) a.intG[static_cast<std::size_t>(id - 10029)] = v;
    else if (id >= 10037 && id <= 10040) g_eclGlobals.ints[static_cast<std::size_t>(id - 10037)] = v;
    else if (id == 10070) e.itemDrop = static_cast<std::int8_t>(v);  // a signed byte field
    else if (id == 10071) e.score = v;
    else return false;
    return true;
}

bool EnemyManager::writeFloat(Enemy& e, std::int32_t id, float v) {
    EclArgs& a = e.ctx.args;
    if (id >= 10004 && id <= 10011) a.floatA[static_cast<std::size_t>(id - 10004)] = v;
    else if (id >= 10033 && id <= 10036) a.floatG[static_cast<std::size_t>(id - 10033)] = v;
    else if (id >= 10041 && id <= 10044) g_eclGlobals.floats[static_cast<std::size_t>(id - 10041)] = v;
    else if (id == 10072 || id == 10073) a.floatB[static_cast<std::size_t>(id - 10072)] = v;
    else if (id >= 10021 && id <= 10023) {  // moves the player [QUIRK]
        if (!w().player) return true;
        Vec3f& p = w().player->scriptPosition();
        (id == 10021 ? p.x : id == 10022 ? p.y : p.z) = v;
    } else {
        switch (id) {
            case 10018: e.pos.x = v; break;
            case 10019: e.pos.y = v; break;
            case 10020: e.pos.z = v; break;
            case 10045: e.angle = v; break;
            case 10046: e.angularVelocity = v; break;
            case 10047: e.speed = v; break;
            case 10048: e.accel = v; break;
            case 10049: e.orbitRadius = v; break;
            case 10050: e.moveOrigin.x = v; break;
            case 10051: e.moveOrigin.y = v; break;
            case 10052: e.moveOrigin.z = v; break;
            case 10053: e.orbitAngle = v; break;
            case 10054: e.orbitAngularVelocity = v; break;
            case 10057: e.moveDelta.x = v; break;
            case 10058: e.moveDelta.y = v; break;
            case 10059: e.moveDelta.z = v; break;
            default: return false;
        }
    }
    return true;
}

// ---- the VM --------------------------------------------------------------------------------

void EnemyManager::startSub(Enemy& e, std::int32_t sub) {
    if (!ecl_.validSub(sub)) {
        eclWarnOnce(std::format("sub {} does not exist (the file has {}); the enemy's script stops", sub, ecl_.subCount()));
        e.ctx.pos = ecl_.size();  // no instruction there: the script never runs again
    } else {
        e.ctx.pos = ecl_.subOffset(sub);
    }
    e.ctx.time.assign(0);
    e.ctx.wait.assign(0);
    e.ctx.sub = static_cast<std::int16_t>(sub);
}

void EnemyManager::startScript(AnmVm& vm, std::int32_t script) { vm.start(*w().anm, *w().rng, kScriptBase + script); }

void EnemyManager::fireStored(Enemy& e) {
    if (w().bulletManager) w().bulletManager->fire(w(), e.shooter);
}

namespace {

// The interrupt entry (spec 5.3): the position saved is the current instruction's end.
void interruptEntry(EnemyManager& m, Enemy& e, const EclFile& f, std::uint32_t cur, void (EnemyManager::*start)(Enemy&, std::int32_t)) {
    const std::uint32_t size = static_cast<std::uint64_t>(cur) + 12 <= f.size() ? eclU16(f.at(cur) + 6) : 0;
    e.ctx.pos = cur + size;
    if (!e.noStack) e.stack[static_cast<std::size_t>(e.depth)] = e.ctx;
    const std::int32_t slot = e.pendingInterrupt;
    if (slot >= 0 && slot < 32) (m.*start)(e, e.interrupts[static_cast<std::size_t>(slot)]);
    else eclWarnOnce(std::format("interrupt slot {} (only 32 exist)", slot));
    if (e.depth < 15) ++e.depth;  // even with the no-stack flag [QUIRK]
    e.pendingInterrupt = -1;
}

}  // namespace

bool EnemyManager::runVm(Enemy& e) {
    GameState& g = *w().state;
    std::uint32_t cur = 0;
    int budget = 1000000;  // a script that loops within one frame would hang the original
restart:
    cur = e.ctx.pos;
    if (e.pendingInterrupt >= 0) {
        interruptEntry(*this, e, ecl_, cur, &EnemyManager::startSub);
        goto restart;
    }
    if (e.periodicSub >= 0) {
        e.periodicCounter.tick();
        if (e.periodicCounter.current >= e.periodicInterval.current) {
            e.periodicCounter.assign(0);
            e.stack[static_cast<std::size_t>(e.depth)] = e.ctx;  // no no-stack check here
            e.ctx.args = e.periodicArgs;
            startSub(e, e.periodicSub);
            if (e.depth < 15) ++e.depth;
            cur = e.ctx.pos;
            e.ctx.periodic = true;
        }
    }
    for (;;) {
        if (e.ctx.wait.current > 0) {
            e.ctx.wait.decrement(1);
            e.ctx.time.decrement(1);  // the end-of-step tick cancels it
            break;
        }
        if (!ecl_.validInstruction(cur)) {
            if (cur < ecl_.size()) eclWarnOnce(std::format("malformed instruction at {:#x}; the script stops there", cur));
            break;
        }
        const std::uint8_t* p = ecl_.at(cur);
        if (eclI32(p) != e.ctx.time.current) break;
        const std::uint16_t size = eclU16(p + 6);
        if (!(p[9] & difficultyBit_)) {
            cur += size;
            continue;
        }
        if (--budget < 0) {
            eclWarnOnce(std::format("sub {} runs away (too many instructions in one step)", e.ctx.sub));
            break;
        }
        const Exec r = execute(e, cur);
        if (r == Exec::Delete) return false;
        if (r == Exec::Restart) goto restart;
        if (r == Exec::Next) cur += size;
    }
    // 5. Movement mode; 6. per-frame work while alive.
    movementModeUpdate(e);
    if (e.life > 0) {
        if (e.shootInterval > 0) {
            e.shootTimer.tick();
            if (e.shootTimer.current >= e.shootInterval) {
                e.shooter.pos = e.pos + e.shootOffset;
                fireStored(e);  // the "bullets disabled" flag is not checked here [QUIRK]
                e.shootTimer.assign(0);
            }
        }
        poseUpdate(e);
        if (e.ctx.exRoutine >= 0 && ecl_.validInstruction(e.ctx.exPos)) exRoutine(e, e.ctx.exRoutine, ecl_.at(e.ctx.exPos));
        runInterpolators(e);
    }
    e.ctx.pos = cur;
    e.ctx.time.tick();
    // 8. The capture value drains per VM step of the main boss (twice in a frame with two
    // steps [QUIRK]).
    if (e.boss && e.bossId == 0 && spell_.active != 0 && g.capturing) {
        if (!e.survival) {
            const float v = static_cast<float>(spell_.base) - (spellTimer_.asFloat() * static_cast<float>(spell_.drain)) / 60.0f;
            g.captureValue = x86ToInt(v);
            g.captureValue -= g.captureValue % 10;
        }
        spellTimer_.tick();
    }
    // 9. Extra and Phantasm bosses vanish while the player bombs during their cards.
    if (e.boss && g.stage >= 7) {
        if (g.bombActive && spell_.active != 0 && spell_.number >= 118) {
            e.invisibleBomb = true;
            e.spellDelay = 1;
        } else if (e.spellDelay > 0) {
            --e.spellDelay;
        } else {
            e.invisibleBomb = false;
        }
    }
    return true;
}

void EnemyManager::movementModeUpdate(Enemy& e) {
    const float m = g_timing.speed;
    auto countdown = [&]() {
        if (e.moveDuration > 0) {
            e.moveTimer.decrement(1);
            if (e.moveTimer.current <= 0) e.moveMode = Enemy::Axis;
        }
    };
    switch (e.moveMode) {
        case Enemy::Orbit: {
            e.orbitAngle = addNormalizeAngle(e.orbitAngle, m * e.orbitAngularVelocity);
            e.orbitRadius = m * e.orbitRadialVelocity + e.orbitRadius;
            Vec3f v;
            fromAngle(v, e.orbitAngle, e.orbitRadius);
            e.velocity.x = (v.x + e.moveOrigin.x) - e.pos.x;
            e.velocity.y = (v.y + e.moveOrigin.y) - e.pos.y;
            e.angle = zatan2(e.velocity.y, e.velocity.x);
            countdown();
            break;
        }
        case Enemy::Polar:
            e.angle = addNormalizeAngle(e.angle, m * e.angularVelocity);
            e.speed = m * e.accel + e.speed;
            fromAngle(e.velocity, e.angle, e.speed);
            e.velocity.z = 0.0f;
            countdown();
            break;
        case Enemy::Interpolate: {
            e.moveTimer.decrement(1);
            float t = 1.0f - e.moveTimer.asFloat() / static_cast<float>(e.moveDuration);
            if (t < 0.0f) t = 0.0f;
            t = eclEase(e.easing, t);
            e.velocity.x = ((e.moveDelta.x * t) + e.moveOrigin.x) - e.pos.x;
            e.velocity.y = ((e.moveDelta.y * t) + e.moveOrigin.y) - e.pos.y;
            e.velocity.z = ((e.moveDelta.z * t) + e.moveOrigin.z) - e.pos.z;
            if (e.mirror) e.velocity.x = -e.velocity.x;
            e.angle = zatan2(e.velocity.y, e.velocity.x);
            if (e.moveTimer.current <= 0) {
                e.moveMode = Enemy::Axis;
                e.pos = e.moveOrigin + e.moveDelta;
                e.velocity = {};
            }
            break;
        }
        default: break;  // axis: the velocity is kept
    }
}

void EnemyManager::poseUpdate(Enemy& e) {
    if (e.pose[3] < 0) return;
    int dir = e.velocity.x < -0.01f ? 1 : e.velocity.x > 0.01f ? 2 : 0;
    if (e.mirror && dir != 0) dir = 3 - dir;
    if (dir == e.poseState) return;
    std::int16_t script;
    if (dir == 0) script = e.poseState == 255 ? e.pose[0] : e.poseState == 1 ? e.pose[1] : e.pose[2];
    else script = dir == 1 ? e.pose[3] : e.pose[4];
    startScript(e.vm, script);
    e.poseState = static_cast<std::uint8_t>(dir);
}

float EnemyManager::interpValue(Enemy& e, EclInterp& s, float t) {
    // The stored values are always read as variable ids, whatever the instruction's mask
    // said [QUIRK].
    if (s.routine == EclInterp::Linear) {
        const float a = readFloat(e, s.p0), b = readFloat(e, s.p1);
        return (b - a) * t + a;
    }
    const float p0 = readFloat(e, s.p0), p1 = readFloat(e, s.p1), m0 = readFloat(e, s.m0), m1 = readFloat(e, s.m1);
    const float h00 = ((t - 1.0f) * (t - 1.0f)) * (2.0f * t + 1.0f);
    const float h01 = (t * t) * (3.0f - 2.0f * t);
    const float h10 = ((1.0f - t) * (1.0f - t)) * t;
    const float h11 = ((t - 1.0f) * t) * t;
    return ((h00 * p0 + h01 * p1) + h10 * m0) + h11 * m1;
}

void EnemyManager::runInterpolators(Enemy& e) {
    const Vec3f remembered = e.pos;
    bool touched = false;
    for (EclInterp& s : e.ctx.interps) {
        if (s.routine == EclInterp::None) continue;
        s.timer.tick();
        if (s.timer.current >= s.duration) s.timer.assign(s.duration);
        float t = s.timer.asFloat() / static_cast<float>(s.duration);  // duration 0: NaN [QUIRK]
        t = eclEase(s.easing, t);
        const float v = interpValue(e, s, t);
        if (!writeFloat(e, x86ToInt(s.target), v)) s.target = v;  // overwrites its own target [QUIRK]
        if (s.timer.current >= s.duration) s.routine = EclInterp::None;
        if (s.target == 10018.0f || s.target == 10019.0f || s.target == 10020.0f) touched = true;
    }
    if (touched) {
        // The interpolated position becomes a velocity for the movement step (scaled by the
        // game speed and mirrored there [QUIRK]); z is not converted, so it has no effect.
        e.velocity.x = e.pos.x - remembered.x;
        e.velocity.y = e.pos.y - remembered.y;
        e.angle = zatan2(e.velocity.y, e.velocity.x);
        e.pos = remembered;
    }
}

// ---- the instructions ----------------------------------------------------------------------

EnemyManager::Exec EnemyManager::execute(Enemy& e, std::uint32_t& cur) {
    const World& wd = w();
    GameState& g = *wd.state;
    ZunRng& rng = *wd.rng;
    std::uint8_t* p = ecl_.at(cur);
    const std::int16_t op = eclI16(p + 4);
    const std::uint16_t size = eclU16(p + 6);
    const EclOps o{*this, e, p, eclU16(p + 10)};
    BulletManager* bm = wd.bulletManager;
    auto jump = [&](std::int32_t time, std::int32_t offset) {
        e.ctx.time.current = time;
        cur = static_cast<std::uint32_t>(static_cast<std::int64_t>(cur) + offset);
        return Exec::Jumped;
    };
    auto laserRef = [&](std::int32_t k) -> Laser* {
        if (k < 0 || k >= 32) {
            eclWarnOnce(std::format("laser reference {} (only 32 exist)", k));
            return nullptr;
        }
        const std::int16_t slot = e.lasers[static_cast<std::size_t>(k)];
        return slot >= 0 && bm ? &bm->laser(slot) : nullptr;
    };

    switch (op) {
        // ---- control and arithmetic ----
        case 0: break;
        case 1: return Exec::Delete;
        case 2: return jump(o.L(0), o.L(1));
        case 3:
            o.setIW(2, o.getIW(2) - 1);
            if (o.I(2) > 0) return jump(o.L(0), o.L(1));
            break;
        case 4: o.setIW(0, o.I(1)); break;
        case 5: o.setFW(0, o.F(1)); break;
        case 6: o.setIW(0, static_cast<std::int32_t>(rng.u32InRange(static_cast<std::uint32_t>(o.I(1))))); break;
        case 7: {
            const auto r = static_cast<std::int32_t>(rng.u32InRange(static_cast<std::uint32_t>(o.I(1))));
            o.setIW(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(r) + static_cast<std::uint32_t>(o.I(2))));
            break;
        }
        case 8: o.setFW(0, rng.f32InRange(o.F(1))); break;
        case 9: {
            const float r = rng.f32InRange(o.F(1));
            o.setFW(0, r + o.F(2));
            break;
        }
        case 10: {
            const std::int32_t v = o.I(1);
            const std::int32_t sign = (rng.u16() & 1) ? 1 : -1;
            o.setIW(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(sign) * static_cast<std::uint32_t>(v)));
            break;
        }
        case 11: {
            const float v = o.F(1);
            o.setFW(0, (rng.u16() & 1) ? v : -v);
            break;
        }
        case 12: case 13: case 14: case 15: case 16: {
            const std::int32_t a = o.I(1), b = o.I(2);
            const auto ua = static_cast<std::uint32_t>(a), ub = static_cast<std::uint32_t>(b);
            std::int32_t r = 0;
            if (op == 12) r = static_cast<std::int32_t>(ua + ub);
            else if (op == 13) r = static_cast<std::int32_t>(ua - ub);
            else if (op == 14) r = static_cast<std::int32_t>(ua * ub);
            else if (b == 0 || (a == INT32_MIN && b == -1)) {
                eclWarnOnce(std::format("integer {} by {} in sub {} (the original crashes); nothing written",
                                        op == 15 ? "division" : "modulo", b, e.ctx.sub));
                break;
            } else {
                r = op == 15 ? a / b : a % b;
            }
            o.setIW(0, r);
            break;
        }
        case 17: o.setIW(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.getIW(0)) + 1u)); break;
        case 18: o.setIW(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(o.getIW(0)) - 1u)); break;
        case 19: case 20: case 21: case 22: case 23: {
            const float a = o.F(1), b = o.F(2);
            float r;
            if (op == 19) r = a + b;
            else if (op == 20) r = a - b;
            else if (op == 21) r = a * b;
            else if (op == 22) r = a / b;
            else r = static_cast<float>(std::fmod(static_cast<double>(a), static_cast<double>(b)));
            o.setFW(0, r);
            break;
        }
        case 24: o.setFW(0, zsin(o.F(1))); break;
        case 25: o.setFW(0, zcos(o.F(1))); break;
        case 26: {
            const float x1 = o.F(1), y1 = o.F(2), x2 = o.F(3), y2 = o.F(4);
            o.setFW(0, zatan2(y2 - y1, x2 - x1));
            break;
        }
        case 27: {
            const float target = o.LF(0);
            EclInterp* slot = nullptr;
            for (EclInterp& s : e.ctx.interps) {
                if (s.routine == EclInterp::None || s.target == target) {
                    slot = &s;
                    break;
                }
            }
            if (!slot) break;
            slot->timer.assign(0);
            slot->target = target;
            slot->duration = o.I(1);
            slot->routineIndex = o.I(2);
            slot->easing = o.I(3);
            if (slot->routineIndex >= 0 && slot->routineIndex <= 6) slot->routine = EclInterp::Linear;
            else if (slot->routineIndex == 7) slot->routine = EclInterp::Hermite;
            else {
                eclWarnOnce(std::format("interpolation routine {} (only 0-7 exist)", slot->routineIndex));
                slot->routine = EclInterp::None;
            }
            slot->p0 = o.F(4);
            slot->p1 = o.F(5);
            slot->m0 = o.F(6);
            slot->m1 = o.F(7);
            break;
        }
        case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: {
            bool holds;
            const int kind = (op - 28) / 2;
            if ((op & 1) == 0) {
                const std::int32_t a = o.I(0), b = o.I(1);
                holds = kind == 0 ? a == b : kind == 1 ? a != b : kind == 2 ? a < b : kind == 3 ? a <= b : kind == 4 ? a > b : a >= b;
            } else {
                const float a = o.F(0), b = o.F(1);
                holds = kind == 0 ? a == b : kind == 1 ? a != b : kind == 2 ? a < b : kind == 3 ? a <= b : kind == 4 ? a > b : a >= b;
            }
            if (holds) return jump(o.L(2), o.L(3));
            break;
        }
        case 40: o.setFW(0, addNormalizeAngle(o.F(0), 0.0f)); break;
        case 41: {
            e.ctx.pos = cur + size;
            if (!e.noStack) e.stack[static_cast<std::size_t>(e.depth)] = e.ctx;
            startSub(e, o.L(0));
            e.ctx.args.intG = g_eclGlobals.ints;
            e.ctx.args.floatG = g_eclGlobals.floats;
            if (!e.noStack && e.depth < 15) ++e.depth;
            return Exec::Restart;
        }
        case 42:
            if (e.depth <= 0) {
                eclWarnOnce(std::format("return at stack depth 0 in sub {} (undefined in the original); ignored", e.ctx.sub));
                break;
            }
            --e.depth;
            if (e.ctx.periodic) {
                e.periodicArgs = e.ctx.args;
                e.ctx.periodic = false;
            }
            e.ctx = e.stack[static_cast<std::size_t>(e.depth)];
            return Exec::Restart;
        case 145: {
            // [GAP] Missing from the specification's tables, which only list it as a boss-slot
            // instruction reading I(0) twice; the scripts use it as (boss slot, interrupt),
            // e.g. stage 4's conductor sub signalling the three sisters. Implemented as
            // timeline opcode 10: that boss's pending interrupt.
            Enemy* b = bossAt(o.I(0));
            (void)o.I(0);
            if (b) b->pendingInterrupt = o.I(1);
            else eclWarnOnce("opcode 145 signals a missing boss (the original crashes)");
            break;
        }
        case 43:
        case 44: {
            Enemy* b = bossAt(o.I(2));
            if (!b) {
                eclWarnOnce(std::format("opcode {} reads a missing boss (the original crashes)", op));
                break;
            }
            if (op == 43) o.setIW(0, o.bit(1) ? readInt(*b, o.L(1)) : o.L(1));
            else o.setFW(0, o.bit(1) ? readFloat(*b, o.LF(1)) : o.LF(1));
            break;
        }
        case 45: e.ctx.wait.assign(o.I(0)); break;
        case 123: e.ctx.time.increment(o.I(0)); break;  // a frame-timer add (fractional while slowed) [QUIRK]
        case 159: {
            const float end = o.F(1), start = o.F(2), t = o.F(3);
            o.setFW(0, (end - start) * t + o.F(2));
            break;
        }
        case 51: {
            const float lo = o.F(1), hi = o.F(2);
            o.setFW(0, rng.f32InRange(hi - lo) + lo);
            break;
        }
        case 151: {
            const float a = o.F(2), m = o.F(3);
            o.setFW(1, sinTimes(a, m));
            const float a2 = o.F(2), m2 = o.F(3);  // re-read: they may name the variable just written [QUIRK]
            o.setFW(0, cosTimes(a2, m2));
            break;
        }

        // ---- movement ----
        case 46:
            e.pos = {o.F(0), o.F(1), o.F(2)};
            clampToBounds(e);
            break;
        case 47:
            e.velocity = {o.F(0), o.F(1), o.F(2)};
            e.angle = zatan2(e.velocity.y, e.velocity.x);
            e.moveMode = Enemy::Axis;
            break;
        case 48: e.angularVelocity = o.F(0), e.moveMode = Enemy::Polar; break;
        case 49: e.speed = o.F(0), e.moveMode = Enemy::Polar; break;
        case 50: e.accel = o.F(0), e.moveMode = Enemy::Polar; break;
        case 53:
            e.angle = angleToPlayer(e.pos) + o.F(0);
            e.speed = o.F(1);
            e.moveMode = Enemy::Polar;
            break;
        case 54:
            if (o.I(0) <= 0) {
                e.angle = addNormalizeAngle(o.F(2), 0.0f);
                e.speed = o.F(3);
                e.moveMode = Enemy::Polar;
                const std::int32_t t = o.I(0);
                e.moveTimer.assign(t);
                e.moveDuration = t;
            } else {
                const float a = addNormalizeAngle(o.F(2), 0.0f);
                const float sx = o.F(3);
                const auto tx = static_cast<float>(o.I(0));
                e.moveDelta.x = cosTimes(a, sx) * tx;
                const float sy = o.F(3);
                const auto ty = static_cast<float>(o.I(0));
                e.moveDelta.y = sinTimes(a, sy) * ty;
                e.moveDelta.z = 0.0f;
                e.moveOrigin = e.pos;
                const std::int32_t t = o.I(0);
                e.moveTimer.assign(t);
                e.moveDuration = t;
                e.easing = static_cast<std::uint8_t>(o.I(1) & 7);
                e.moveMode = Enemy::Interpolate;
                if (e.mirror) e.moveDelta.x = -e.moveDelta.x;
            }
            break;
        case 55: {
            const Vec3f target{o.F(2), o.F(3), o.F(4)};
            e.moveDelta = target - e.pos;
            e.moveOrigin = e.pos;
            const std::int32_t t = o.I(0);
            e.moveTimer.assign(t);
            e.moveDuration = t;
            e.easing = static_cast<std::uint8_t>(o.I(1) & 7);
            e.moveMode = Enemy::Interpolate;
            e.velocity = {};
            if (e.mirror) e.moveDelta.x = -e.moveDelta.x;
            break;
        }
        case 56: {
            const std::int32_t t = o.I(0);
            e.moveTimer.assign(t);
            e.moveDuration = t;
            e.moveOrigin = {o.F(1), o.F(2), o.F(3)};
            e.orbitAngle = o.F(4);
            e.orbitAngularVelocity = o.F(5);
            e.orbitRadius = o.F(6);
            e.orbitRadialVelocity = o.F(7);
            e.moveMode = Enemy::Orbit;
            break;
        }
        case 57: e.orbitRadius = o.F(0), e.orbitRadialVelocity = o.F(1); break;
        case 58: e.orbitAngle = o.F(0), e.orbitAngularVelocity = o.F(1); break;
        case 59: case 60: case 61: {
            e.moveMode = op == 59 ? Enemy::Polar : op == 60 ? Enemy::Orbit : Enemy::Interpolate;
            const std::int32_t t = o.I(0);
            e.moveTimer.assign(t);
            e.moveDuration = t;
            break;
        }
        case 62:
            e.boundsLowX = o.F(0);
            e.boundsLowY = o.F(1);
            e.boundsHighX = o.F(2);
            e.boundsHighY = o.F(3);
            e.bounds = true;
            break;
        case 63: e.bounds = false; break;
        case 52:
        case 155: {
            const float px = playerPos(wd).x;
            constexpr float kHalfPi = kZunPi / 2.0f, kQuarterPi = kZunPi / 4.0f, kThreeQuarterPi = (kZunPi * 3.0f) / 4.0f;
            float a;
            const bool left = op == 52 ? px < e.pos.x : (px < e.pos.x && e.pos.x > 96.0f) || e.pos.x > 288.0f;
            if (left) a = addNormalizeAngle(rng.f32InRange(kHalfPi) + kThreeQuarterPi, 0.0f);
            else a = rng.f32InRange(kHalfPi) - kQuarterPi;
            if (op == 52) {
                // The bounds are read even when they are disabled [QUIRK].
                if (e.pos.x < e.boundsLowX + 96.0f) {
                    if (a > kHalfPi) a = kZunPi - a;
                    else if (a < -kHalfPi) a = -kZunPi - a;
                }
                if (e.boundsHighX - 96.0f < e.pos.x) {
                    if (a >= 0.0f && a < kHalfPi) a = kZunPi - e.angle;  // the movement angle, not a [QUIRK]
                    else if (a > -kHalfPi && a <= 0.0f) a = -kZunPi - a;
                }
                if (e.boundsLowY + 48.0f > e.pos.y && a < 0.0f) a = -a;
                if (e.boundsHighY - 48.0f < e.pos.y && a > 0.0f) a = -a;
            }
            o.setFW(0, a);
            break;
        }

        // ---- animation, flags, life, timers ----
        case 95: startScript(e.vm, o.I(0)); break;
        case 96:
            e.pose = {o.H(0, 0), o.H(0, 1), o.H(1, 0), o.H(1, 1), o.H(2, 0)};
            e.poseState = 255;
            break;
        case 97: {
            const bool on = o.I(1) >= 0;
            const std::int32_t slot = o.I(0);
            const std::int32_t script = on ? o.I(1) : -1;
            (void)o.I(0);  // the slot is read twice
            if (slot < 0 || slot >= 2) {
                eclWarnOnce(std::format("secondary ANM VM {} (only 2 exist)", slot));
                break;
            }
            if (on) startScript(e.vms2[static_cast<std::size_t>(slot)], script);
            e.vm2On[static_cast<std::size_t>(slot)] = on;
            break;
        }
        case 98:
            e.death1 = static_cast<std::int8_t>(o.word(0)[0]);
            e.death2 = o.word(0)[1];
            e.death3 = o.word(0)[2];
            break;
        case 120: e.autoRotate = o.B(0) & 1; break;
        case 128: e.vm.interrupt(o.I(0)); break;
        case 129: {
            const std::int32_t slot = o.L(0);
            if (slot >= 0 && slot < 2) e.vms2[static_cast<std::size_t>(slot)].interrupt(o.H(1, 0));
            else eclWarnOnce(std::format("secondary ANM VM {} (only 2 exist)", slot));
            break;
        }
        case 150:
            e.vm.rotation.z = o.F(0);
            e.vm.set(AnmVm::RotationDirty, true);
            break;
        case 101: e.hitbox = {o.F(0), o.F(1), o.F(2)}; break;
        case 153: e.box2 = {o.F(0), o.F(1), o.F(2)}; break;
        case 102: e.contact = o.B(0) & 1; break;
        case 103: e.damageable = o.B(0) & 1; break;
        case 104: e.hittable = o.B(0) & 1; break;
        case 116: e.canDie = o.B(0) & 1; break;
        case 132: e.noCollision = o.B(0) & 1; break;
        case 135: e.survival = o.B(0) & 1; break;
        case 136:
            e.projectile = o.B(0) & 1;
            e.drawLayer = 2;
            break;
        case 137: e.oobDisabled = o.B(0) & 1; break;
        case 130: e.noStack = o.B(0) & 1; break;
        case 161: e.freezeBomb = o.I(0) & 1; break;
        case 106: e.deathType = o.B(0) & 7; break;
        case 107: e.deathSub = o.B(0); break;  // a byte: 0-255, never -1 [QUIRK]
        case 108: {
            const std::int32_t sub = o.I(0), slot = o.I(1);
            if (slot >= 0 && slot < 32) e.interrupts[static_cast<std::size_t>(slot)] = sub;
            else eclWarnOnce(std::format("interrupt slot {} (only 32 exist)", slot));
            break;
        }
        case 109:
            e.pendingInterrupt = o.I(0);
            interruptEntry(*this, e, ecl_, cur, &EnemyManager::startSub);
            return Exec::Restart;
        case 110:
            e.life = e.maxLife = o.I(0);
            if (e.boss && e.bossId == 0) {
                g.boss.segmentA.fill(0.0f);
                g.boss.segmentB.fill(0.0f);
            }
            break;
        case 111: e.timer.assign(o.I(0)); break;
        case 112: e.lifeThresholds[0] = o.I(0); break;
        case 113: e.lifeSubs[0] = o.I(0); break;
        case 148: {
            const std::int32_t i = o.I(0);
            if (i < 0 || i >= 4) {
                eclWarnOnce(std::format("life callback {} (only 4 exist)", i));
                break;
            }
            e.lifeThresholds[static_cast<std::size_t>(i)] = o.I(1);
            const std::int32_t j = o.I(0);
            if (j >= 0 && j < 4) e.lifeSubs[static_cast<std::size_t>(j)] = o.I(2);
            break;
        }
        case 114:
            e.timerThreshold = o.I(0);
            e.timer.assign(0);
            break;
        case 115: e.timerSub = o.I(0); break;
        case 133:
            e.timerSub = e.deathSub;
            e.timer.assign(0);
            break;
        case 142: e.invincible.assign(o.I(0)); break;
        case 144:
            e.periodicInterval.assign(o.I(0));
            e.periodicSub = o.I(1);
            e.periodicCounter.assign(0);
            e.periodicArgs = e.ctx.args;
            break;
        case 121: exRoutine(e, o.I(0), p); break;
        case 122:
            if (o.I(0) >= 0) {
                e.ctx.exRoutine = static_cast<std::int8_t>(o.I(0));
                e.ctx.exPos = cur;
            } else {
                e.ctx.exRoutine = -1;
            }
            break;
        case 138: {
            e.trailFlags = o.B(0);
            std::int32_t count = o.I(1);
            if (count < 0 || count > 96) {
                eclWarnOnce(std::format("trail of {} nodes (96 exist)", count));
                count = std::clamp(count, 0, 96);
            }
            e.trailCount = static_cast<std::int16_t>(count);
            e.trailInterval = static_cast<std::int16_t>(o.I(2));
            e.nodeStep = static_cast<std::int16_t>(o.I(3));
            break;
        }
        case 105: wd.playSound(o.I(0)); break;
        case 140:  // render only: effect 20 (weather) is drawn with this colour multiplier
            if (wd.effects) wd.effects->setColorMultiplier(o.F(0), o.F(1), o.F(2), o.F(3));
            break;
        case 147: deadStore_ = o.I(0); break;
        case 125:
            if (wd.background) wd.background->setResumeLabel(o.I(0));
            break;
        case 126:
            g.boss.lifeMarkers = o.I(0);
            g.completion += 1800;
            break;

        // ---- bullets ----
        case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72: {
            if (e.life <= 0) break;
            Shooter& s = e.shooter;
            const std::int32_t typeRaw = o.H(0, 0), colorRaw = o.H(0, 1);
            s.type = static_cast<std::int16_t>(o.bit(0) ? readInt(e, typeRaw) : typeRaw);
            s.aimMode = op - 64;
            s.count1 = static_cast<std::int16_t>(o.IB(1, 2));
            s.count2 = static_cast<std::int16_t>(o.IB(2, 3));
            s.pos = e.pos + e.shootOffset;
            s.angle1 = o.FB(5, 6);
            s.speed1 = o.FB(3, 4);
            s.angle2 = o.FB(6, 7);
            s.speed2 = o.FB(4, 5);
            if (spell_.active == 0) {
                const std::int32_t r = g.rank;
                const std::int32_t rank1 = r * (e.rankAmount1High - e.rankAmount1Low) / 32 + e.rankAmount1Low;
                const std::int32_t rank2 = r * (e.rankAmount2High - e.rankAmount2Low) / 32 + e.rankAmount2Low;
                s.count1 = static_cast<std::int16_t>(s.count1 + rank1);
                if (s.count1 <= 0) s.count1 = 1;
                s.count2 = static_cast<std::int16_t>(s.count2 + rank2);
                if (s.count2 <= 0) s.count2 = 1;
                const float rankSpeed = (static_cast<float>(r) * (e.rankSpeedHigh - e.rankSpeedLow)) / 32.0f + e.rankSpeedLow;
                if (s.speed1 != 0.0f) {
                    s.speed1 += rankSpeed;
                    if (s.speed1 < 0.3f) s.speed1 = 0.3f;
                }
                s.speed2 += rankSpeed / 2.0f;
                if (s.speed2 < 0.3f) s.speed2 = 0.3f;
            }
            s.flags = static_cast<std::uint32_t>(o.L(7));  // replaces opcode 81's sound flag [QUIRK]
            s.color = static_cast<std::int16_t>(o.bit(1) ? readInt(e, colorRaw) : colorRaw);
            if (!e.bulletsDisabled) fireStored(e);
            break;
        }
        case 73:
        case 74: {
            e.shootInterval = o.I(0);
            if (e.shootInterval != 0) {
                const std::int32_t i = e.shootInterval;
                e.shootInterval += g.rank * ((-(i / 5)) - (i / 5)) / 32 + i / 5;
                if (op == 73) e.shootTimer.assign(0);
                else e.shootTimer.assign(static_cast<std::int32_t>(rng.u32InRange(static_cast<std::uint32_t>(e.shootInterval))));
            }
            break;
        }
        case 75: e.bulletsDisabled = true; break;
        case 76: e.bulletsDisabled = false; break;
        case 77:
            e.shooter.pos = e.pos + e.shootOffset;
            fireStored(e);
            break;
        case 78: e.shootOffset = {o.F(0), o.F(1), o.F(2)}; break;
        case 79: {
            const std::int32_t i = o.I(0);
            if (i < 0 || i >= 6) {
                eclWarnOnce(std::format("bullet command {} (only 6 exist)", i));
                break;
            }
            BulletCommand& c = e.shooter.commands[static_cast<std::size_t>(i)];
            c.type = static_cast<std::uint32_t>(o.I(1));
            c.immediate = o.I(2);
            c.duration = o.I(3);
            c.loops = o.I(4);
            c.a = o.F(5);
            c.b = o.F(6);
            break;
        }
        case 80:
            if (bm) bm->clearAll(wd, 1);
            break;
        case 146:
            if (bm) bm->clearAll(wd, 0);
            break;
        case 143:
            if (bm) bm->radiusClear(wd, e.pos, o.F(0));
            break;
        case 81:
            if (o.I(0) >= 0) {
                e.shooter.sound = o.I(0);
                e.shooter.flags |= 0x200;
            } else {
                e.shooter.flags &= ~0x200u;
            }
            e.shooter.bulletSound = o.I(1);
            break;
        case 131:
            e.rankSpeedLow = o.F(0);
            e.rankSpeedHigh = o.F(1);
            e.rankAmount1Low = static_cast<std::int16_t>(o.I(2));
            e.rankAmount1High = static_cast<std::int16_t>(o.I(3));
            e.rankAmount2Low = static_cast<std::int16_t>(o.I(4));
            e.rankAmount2High = static_cast<std::int16_t>(o.I(5));
            break;

        // ---- lasers ----
        case 82:
        case 83: {
            if (!bm) break;
            Shooter s;
            s.pos = e.pos + e.shootOffset;
            s.type = o.H(0, 0);
            const std::int32_t colorRaw = o.H(0, 1);
            s.color = static_cast<std::int16_t>(o.bit(1) ? readInt(e, colorRaw) : colorRaw);
            s.angle1 = o.FB(1, 2);
            s.speed1 = o.FB(2, 3);
            const float start = o.FB(3, 4), end = o.FB(4, 5), startLength = o.FB(5, 6);
            Laser* l = bm->spawnLaser(wd, s, start, end, startLength, o.LF(6), o.L(7), o.L(8), o.L(9), o.L(10), o.L(11),
                                      static_cast<std::uint16_t>(o.L(12)), op == 83);
            const int slot = bm->laserIndex(l);
            if (slot < 0) eclWarnOnce("all 64 lasers in use (the original returns a reference past the pool); none kept");
            if (e.laserIndex >= 0 && e.laserIndex < 32) e.lasers[static_cast<std::size_t>(e.laserIndex)] = static_cast<std::int16_t>(slot);
            else eclWarnOnce(std::format("laser index {} (only 32 exist)", e.laserIndex));
            break;
        }
        case 84: e.laserIndex = o.I(0); break;
        case 85:
            if (Laser* l = laserRef(o.I(0))) l->angle = addNormalizeAngle(l->angle, o.F(1));
            break;
        case 152:
            if (Laser* l = laserRef(o.I(0))) l->angle = o.F(1);
            break;
        case 86:
            if (Laser* l = laserRef(o.I(0))) l->angle = angleToPlayer(l->origin) + o.F(1);
            break;
        case 87:
            if (Laser* l = laserRef(o.I(0))) l->origin = e.pos + Vec3f{o.F(1), o.F(2), o.F(3)};
            break;
        case 156:
            if (Laser* l = laserRef(o.I(0))) l->hideWarning = static_cast<std::uint8_t>(o.I(1));
            break;
        case 157:
            if (Laser* l = laserRef(o.I(0))) l->startLength = o.F(1);
            break;
        case 158:
            if (Laser* l = laserRef(o.I(0))) {
                l->start = o.F(1);
                l->end = o.F(2);
            }
            break;
        case 88: {
            const Laser* l = laserRef(o.I(0));
            e.ctx.laserTest = !l || !l->inUse;  // nothing reads it
            break;
        }
        case 89:
            if (Laser* l = laserRef(o.I(0)); l && l->inUse && l->state < 2) bm->stopLaser(*l);
            break;
        case 134: e.lasers.fill(-1); break;

        // ---- spawning, bosses, spell cards, items, effects, cherry ----
        case 92:
        case 93: {
            if (e.life <= 0) break;
            const std::int32_t sub = o.L(0);
            Vec3f at{o.F(1), o.F(2), o.F(3)};
            if (op == 93) at = e.pos + at;
            // Call arguments, evaluated right to left as Microsoft compilers do [AMBIGUOUS].
            const std::int32_t score = o.I(6), item = o.I(5), life = o.I(4);
            const EclArgs args = e.ctx.args;
            spawnWithContext(sub, at, life, item, score, args);
            break;
        }
        case 94: (void)killAllScoring(8000, 0); break;
        case 99: {
            if (o.I(0) >= 0) {
                const std::int32_t slot = o.I(0);
                if (slot < kBossSlots) bosses_[static_cast<std::size_t>(slot)] = indexOf(e);
                else eclWarnOnce(std::format("boss slot {} (only 8 exist)", slot));
                g.boss.present = true;
                g.boss.bar = 1.0f;
                e.boss = true;
                e.bossId = static_cast<std::uint8_t>(o.I(0));
                log::info("ECL: enemy {} is boss {} (frame {}, life {})", indexOf(e), e.bossId, g.stageFrame, e.life);
            } else {
                if (e.bossId < 4) g.boss.present = false;
                if (e.bossId < kBossSlots) bosses_[e.bossId] = -1;  // a never-boss enemy clears slot 0 [QUIRK]
                e.boss = false;
                if (e.bossId < 4) g.boss.markerX[e.bossId] = -999.0f;
                releaseCircles(e);
            }
            break;
        }
        case 139: {
            const std::int32_t seg = o.I(0);
            const std::int32_t a = o.I(1), b = o.I(2);
            if (seg < 0 || seg >= 8) {
                eclWarnOnce(std::format("boss bar segment {} (only 8 exist)", seg));
                break;
            }
            g.boss.segmentA[static_cast<std::size_t>(seg)] = static_cast<float>(a) / static_cast<float>(e.maxLife);
            g.boss.segmentB[static_cast<std::size_t>(seg)] = static_cast<float>(b) / static_cast<float>(e.maxLife);
            g.boss.segmentColor[static_cast<std::size_t>(seg)] = o.I(3);
            break;
        }
        case 90: spellCardStart(e, p); break;
        case 91: spellCardEnd(); break;
        case 100: {
            if (!wd.effects || !wd.tables) break;
            const std::int32_t idx = o.L(0);
            const std::uint32_t color =
                idx >= 0 && idx < ExeTables::kColors ? wd.tables->colorsPale[static_cast<std::size_t>(idx)] : 0xFFFFFFFFu;
            Effect& c = wd.effects->spawn(wd, 13, e.pos, 1, color);
            c.direction = {o.LF(1), o.LF(2), o.LF(3)};
            e.circleRadius = o.LF(4);
            if (e.circleCount < 24) e.circles[static_cast<std::size_t>(e.circleCount++)] = &c;
            else eclWarnOnce("more than 24 spell circles on one enemy");
            break;
        }
        case 149:
            e.circleFrozen = o.I(0) & 1;
            if (!e.circleFrozen) {
                if (e.special) e.special->pos = {o.F(1), o.F(2), o.F(3)};
                else eclWarnOnce("opcode 149 without a spell card circle (the original writes through a stale pointer)");
            }
            break;
        case 117:
        case 118: {
            if (!wd.effects) break;
            const std::int32_t id = o.I(0), count = o.I(1);
            // The colour is read through the writable int map: a readable-only id gives the
            // literal (and draws nothing).
            const auto color = static_cast<std::uint32_t>(o.bit(2) && intWritable(o.L(2)) ? readInt(e, o.L(2)) : o.L(2));
            if (op == 117) wd.effects->spawn(wd, id, e.pos, count, color);
            else wd.effects->spawnWithVector(wd, id, e.pos, {o.F(3), o.F(4), o.F(5)}, count, color);
            break;
        }
        case 119:
        case 154: {
            const std::int32_t n = o.I(0);
            for (std::int32_t j = 0; j < n; ++j) {
                Vec3f at = e.pos;
                at.x += rng.f32InRange(128.0f) - 64.0f;
                at.y += rng.f32InRange(128.0f) - 64.0f;
                int type = ItemField::Point;
                if (op == 119 && g.power < 128.0f) type = j == 0 ? ItemField::BigPower : ItemField::SmallPower;
                if (wd.items) wd.items->spawn(at, type, ItemField::Normal);
            }
            break;
        }
        case 124:
            if (wd.items) wd.items->spawn(e.pos, o.I(0), ItemField::Normal);
            break;
        case 160: wd.cherryAddWithGauge(o.I(0)); break;

        case 127:
        case 141: break;
        default:
            eclWarnOnce(std::format("opcode {} is not known; ignored", op));
            break;
    }
    return Exec::Next;
}

}  // namespace touhou::th07
