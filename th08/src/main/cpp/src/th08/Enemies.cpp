#include "th08/Enemies.hpp"

#include <algorithm>
#include <format>
#include <set>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Background.hpp"
#include "th08/Effects.hpp"
#include "th08/ScreenEffects.hpp"
#include "th08/ShotData.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Gui.hpp"
#include "th08/Player.hpp"

namespace touhou::th08 {

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
    if (eclI32(b.data()) != 0x800) return fail("not a TH08 ECL file (version)");
    const std::int16_t subs = eclI16(b.data() + 4), timelines = eclI16(b.data() + 6);
    if (subs < 0 || timelines < 0 || timelines > kTimelines) return fail("bad ECL header counts");
    if (kHeaderSize + 4ull * static_cast<unsigned>(subs) > b.size()) return fail("ECL sub table cut off");
    const auto n = static_cast<std::uint32_t>(b.size());
    for (int i = 0; i < timelines; ++i) {
        const auto off = static_cast<std::uint32_t>(eclI32(b.data() + 8 + 4 * i));
        if (static_cast<std::uint64_t>(off) + 8 > n) return fail(std::format("timeline {} outside the file", i));
        timelineOffsets_[static_cast<std::size_t>(i)] = off;
    }
    subOffsets_.resize(static_cast<std::size_t>(subs));
    for (int i = 0; i < subs; ++i) {
        const auto off = static_cast<std::uint32_t>(eclI32(b.data() + kHeaderSize + 4 * i));
        if (static_cast<std::uint64_t>(off) + 12 > n) return fail(std::format("sub {} outside the file", i));
        subOffsets_[static_cast<std::size_t>(i)] = off;
    }
    bytes_.assign(b.begin(), b.end());
    bytes_.resize(b.size() + kPad, 0);
    size_ = n;
    subs_ = subs;
    timelines_ = timelines;
    return true;
}

// ---- argument access ------------------------------------------------------------------------

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

// ---- the variable space (spec 2.2) ----------------------------------------------------------

Vec3f EnemyManager::playerPos() const { return w_ && w_->player ? w_->player->position() : Vec3f{192.0f, 384.0f, 0.0f}; }

float EnemyManager::angleToPlayer(const Vec3f& from) const {
    const Vec3f p = playerPos();
    const float dx = p.x - from.x, dy = p.y - from.y;
    if (dx == 0.0f && dy == 0.0f) return kZunPi / 2.0f;
    return zatan2(dy, dx);
}

namespace {

int familiarCountOf(const Enemy& e) {
    const Enemy* root = e.parent ? e.parent : &e;
    if (!e.parent && !e.next) return 0;
    int n = 0;
    for (const Enemy* f = root->next; f; f = f->next) ++n;
    return n;
}

}  // namespace

std::int32_t EnemyManager::readInt(Enemy& e, std::int32_t id) {
    const World& wd = w();
    GameState& g = *wd.state;
    EclArgs& a = cur_ ? cur_->args : e.ctx.args;
    if (id >= 10000 && id <= 10007) return a.intA[static_cast<std::size_t>(id - 10000)];
    if (id >= 10008 && id <= 10015) return e.intLocals[static_cast<std::size_t>(id - 10008)];
    if (id >= 10016 && id <= 10023) return x86ToInt(a.floatA[static_cast<std::size_t>(id - 10016)]);
    if (id >= 10024 && id <= 10031) return x86ToInt(e.floatLocals[static_cast<std::size_t>(id - 10024)]);
    if (id >= 10036 && id <= 10039) return a.intB[static_cast<std::size_t>(id - 10036)];
    if (id >= 10053 && id <= 10056) return a.intG[static_cast<std::size_t>(id - 10053)];
    if (id >= 10057 && id <= 10060) return x86ToInt(a.floatG[static_cast<std::size_t>(id - 10057)]);
    if (id >= 10061 && id <= 10064) return globals_.ints[static_cast<std::size_t>(id - 10061)];
    if (id >= 10065 && id <= 10068) return x86ToInt(globals_.floats[static_cast<std::size_t>(id - 10065)]);
    if (id >= 10088 && id <= 10091) return e.lifeThresholds[static_cast<std::size_t>(id - 10088)];
    const Vec3f pp = playerPos();
    switch (id) {
        case 10032: return static_cast<std::int32_t>(wd.rng->u32() & 0x7FFFFFFFu);
        case 10033: return x86ToInt(wd.rng->f32ZeroToOne());
        case 10034: return static_cast<std::int32_t>(wd.rng->u32());
        case 10035: return x86ToInt(static_cast<float>(wd.rng->u32()) / 2147483648.0f - 1.0f);
        case 10040: return g.difficulty;
        case 10041: return g.rank;
        case 10042: return x86ToInt(e.abs.x);
        case 10043: return x86ToInt(e.abs.y);
        case 10044: return x86ToInt(e.abs.z);
        case 10045: return x86ToInt(pp.x);
        case 10046: return x86ToInt(pp.y);
        case 10047: return x86ToInt(pp.z);
        case 10048: return x86ToInt(angleToPlayer(e.abs));
        case 10049: return e.timer.current;
        case 10050: {
            const float dx = pp.x - e.abs.x, dy = pp.y - e.abs.y, dz = pp.z - e.abs.z;
            return x86ToInt(std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        case 10051: return e.life;
        case 10052: return g.character;
        case 10069: return x86ToInt(e.angle);
        case 10070: return x86ToInt(e.angularVelocity);
        case 10071: return x86ToInt(e.speed);
        case 10072: return x86ToInt(e.accel);
        case 10073: return x86ToInt(e.orbitRadius);
        case 10074: return x86ToInt(e.moveOrigin.x);
        case 10075: return x86ToInt(e.moveOrigin.y);
        case 10076: return x86ToInt(e.moveOrigin.z);
        case 10077: return x86ToInt(e.orbitAngle);
        case 10078: return x86ToInt(e.orbitAngularVelocity);
        case 10083: return e.damageThisFrame;
        case 10084: return e.bossId;
        case 10085: return x86ToInt(e.delta.x);
        case 10086: return x86ToInt(e.delta.y);
        case 10087: return x86ToInt(e.delta.z);
        case 10092: return e.itemDrop;
        case 10093: return e.score;
        case 10094: return x86ToInt(a.floatB[0]);
        case 10095: return x86ToInt(a.floatB[1]);
        case 10096: return familiarCountOf(e);
        case 10097: return wd.player && wd.player->youkaiInFront() ? 1 : 0;
        case 10098: return g.timePoints >= g.timeQuota ? 2 : 0;
        case 10099: return spell_.active ? (spell_.capturable ? 1 : 0) : 0;
        case 10100: return spell_.timer.current;
        default: return id;
    }
}

float EnemyManager::readFloat(Enemy& e, float raw) {
    const World& wd = w();
    GameState& g = *wd.state;
    EclArgs& a = cur_ ? cur_->args : e.ctx.args;
    const std::int32_t id = x86ToInt(raw);
    if (id >= 10000 && id <= 10007) return static_cast<float>(a.intA[static_cast<std::size_t>(id - 10000)]);
    if (id >= 10008 && id <= 10015) return static_cast<float>(e.intLocals[static_cast<std::size_t>(id - 10008)]);
    if (id >= 10016 && id <= 10023) return a.floatA[static_cast<std::size_t>(id - 10016)];
    if (id >= 10024 && id <= 10031) return e.floatLocals[static_cast<std::size_t>(id - 10024)];
    if (id >= 10036 && id <= 10039) return static_cast<float>(a.intB[static_cast<std::size_t>(id - 10036)]);
    if (id >= 10053 && id <= 10056) return static_cast<float>(a.intG[static_cast<std::size_t>(id - 10053)]);
    if (id >= 10057 && id <= 10060) return a.floatG[static_cast<std::size_t>(id - 10057)];
    if (id >= 10061 && id <= 10064) return static_cast<float>(globals_.ints[static_cast<std::size_t>(id - 10061)]);
    if (id >= 10065 && id <= 10068) return globals_.floats[static_cast<std::size_t>(id - 10065)];
    if (id >= 10088 && id <= 10091) return static_cast<float>(e.lifeThresholds[static_cast<std::size_t>(id - 10088)]);
    const Vec3f pp = playerPos();
    switch (id) {
        case 10032: return static_cast<float>(static_cast<std::int64_t>(wd.rng->u32() & 0x7FFFFFFFu));
        case 10033: return wd.rng->f32ZeroToOne();
        case 10034: return static_cast<float>(static_cast<std::int32_t>(wd.rng->u32()));
        case 10035: return static_cast<float>(wd.rng->u32()) / 2147483648.0f - 1.0f;
        case 10040: return static_cast<float>(g.difficulty);
        case 10041: return static_cast<float>(g.rank);
        case 10042: return e.abs.x;
        case 10043: return e.abs.y;
        case 10044: return e.abs.z;
        case 10045: return pp.x;
        case 10046: return pp.y;
        case 10047: return pp.z;
        case 10048: return angleToPlayer(e.abs);
        case 10049: return static_cast<float>(e.timer.current);
        case 10050: {
            const float dx = pp.x - e.abs.x, dy = pp.y - e.abs.y, dz = pp.z - e.abs.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        case 10051: return static_cast<float>(e.life);
        case 10052: return static_cast<float>(g.character);
        case 10069: return e.angle;
        case 10070: return e.angularVelocity;
        case 10071: return e.speed;
        case 10072: return e.accel;
        case 10073: return e.orbitRadius;
        case 10074: return e.moveOrigin.x;
        case 10075: return e.moveOrigin.y;
        case 10076: return e.moveOrigin.z;
        case 10077: return e.orbitAngle;
        case 10078: return e.orbitAngularVelocity;
        case 10079: return e.moveDelta.x;
        case 10080: return e.moveDelta.y;
        case 10081: return e.moveDelta.z;
        case 10082: return wd.rng->f32InRange(kZunTwoPi) - kZunPi;
        case 10083: return static_cast<float>(e.damageThisFrame);
        case 10084: return static_cast<float>(e.bossId);
        case 10085: return e.delta.x;
        case 10086: return e.delta.y;
        case 10087: return e.delta.z;
        case 10092: return static_cast<float>(e.itemDrop);
        case 10093: return static_cast<float>(e.score);
        case 10094: return a.floatB[0];
        case 10095: return a.floatB[1];
        case 10096: return static_cast<float>(familiarCountOf(e));
        case 10097: return wd.player && wd.player->youkaiInFront() ? 1.0f : 0.0f;
        case 10099: return spell_.active ? (spell_.capturable ? 1.0f : 0.0f) : 0.0f;
        default: return raw;  // unknown (10098 and 10100 included): the argument word
    }
}

bool EnemyManager::writableInt(std::int32_t id) {
    return (id >= 10000 && id <= 10015) || (id >= 10036 && id <= 10041) || id == 10049 || id == 10051 ||
           (id >= 10053 && id <= 10056) || (id >= 10061 && id <= 10064) || id == 10092 || id == 10093;
}

bool EnemyManager::writableFloat(std::int32_t id) {
    return (id >= 10016 && id <= 10031) || (id >= 10042 && id <= 10047) || (id >= 10057 && id <= 10060) ||
           (id >= 10065 && id <= 10081) || id == 10094 || id == 10095;
}

bool EnemyManager::writeInt(Enemy& e, std::int32_t id, std::int32_t v) {
    if (!intWritable(id)) return false;
    EclArgs& a = cur_ ? cur_->args : e.ctx.args;
    GameState& g = *w().state;
    if (id <= 10007) a.intA[static_cast<std::size_t>(id - 10000)] = v;
    else if (id <= 10015) e.intLocals[static_cast<std::size_t>(id - 10008)] = v;
    else if (id >= 10036 && id <= 10039) a.intB[static_cast<std::size_t>(id - 10036)] = v;
    else if (id == 10040) g.difficulty = v;  // [QUIRK]
    else if (id == 10041) g.rank = v;        // [QUIRK]
    else if (id == 10049) e.timer.current = v;
    else if (id == 10051) e.life = v;
    else if (id >= 10053 && id <= 10056) a.intG[static_cast<std::size_t>(id - 10053)] = v;
    else if (id >= 10061 && id <= 10064) globals_.ints[static_cast<std::size_t>(id - 10061)] = v;
    else if (id == 10092) e.itemDrop = static_cast<std::int8_t>(v);
    else if (id == 10093) e.score = v;
    return true;
}

bool EnemyManager::writeFloat(Enemy& e, std::int32_t id, float v) {
    if (!floatWritable(id)) return false;
    EclArgs& a = cur_ ? cur_->args : e.ctx.args;
    if (id <= 10023) a.floatA[static_cast<std::size_t>(id - 10016)] = v;
    else if (id <= 10031) e.floatLocals[static_cast<std::size_t>(id - 10024)] = v;
    else if (id >= 10057 && id <= 10060) a.floatG[static_cast<std::size_t>(id - 10057)] = v;
    else if (id >= 10065 && id <= 10068) globals_.floats[static_cast<std::size_t>(id - 10065)] = v;
    else if (id == 10094 || id == 10095) a.floatB[static_cast<std::size_t>(id - 10094)] = v;
    else if (id >= 10045 && id <= 10047) {  // moves the player [QUIRK]
        if (w().player) {
            Vec3f& p = w().player->scriptPosition();
            (id == 10045 ? p.x : id == 10046 ? p.y : p.z) = v;
        }
    } else {
        switch (id) {
            case 10042: e.pos.x = v; break;  // writes go to P [QUIRK]
            case 10043: e.pos.y = v; break;
            case 10044: e.pos.z = v; break;
            case 10069: e.angle = v; break;
            case 10070: e.angularVelocity = v; break;
            case 10071: e.speed = v; break;
            case 10072: e.accel = v; break;
            case 10073: e.orbitRadius = v; break;
            case 10074: e.moveOrigin.x = v; break;
            case 10075: e.moveOrigin.y = v; break;
            case 10076: e.moveOrigin.z = v; break;
            case 10077: e.orbitAngle = v; break;
            case 10078: e.orbitAngularVelocity = v; break;
            case 10079: e.moveDelta.x = v; break;
            case 10080: e.moveDelta.y = v; break;
            case 10081: e.moveDelta.z = v; break;
            default: return false;
        }
    }
    return true;
}

// ---- set-up ---------------------------------------------------------------------------------

EnemyManager::EnemyManager() : enemies_(kEnemies + 1), template_(std::make_unique<Enemy>()) {
    for (auto& l : drawLists_) l.reserve(kEnemies);
}

EnemyManager::~EnemyManager() = default;

void EnemyManager::resetTemplate() {
    *template_ = Enemy{};
    Enemy& t = *template_;
    t.active = false;
    t.lasers.fill(-1);
    t.vm2File = {-1, -1};
}

namespace {

const char* stageEnemyAnm(int stage) {
    static const char* const kNames[9] = {"stg1enm.anm", "stg2enm.anm", "stg3enm.anm", "stg4aenm.anm", "stg4benm.anm",
                                          "stg5enm.anm", "stg6enm.anm", "stg7enm.anm", "stg8enm.anm"};
    return kNames[std::clamp(stage, 0, 8)];
}
const char* stageEcl(int stage) {
    static const char* const kNames[9] = {"ecldata1", "ecldata2", "ecldata3", "ecldata4a", "ecldata4b",
                                          "ecldata5", "ecldata6", "ecldata7", "ecldata8"};
    return kNames[std::clamp(stage, 0, 8)];
}

}  // namespace

bool EnemyManager::stageStart(const World& w, const Vfs& vfs, TextureLibrary& textures, int stage, bool spellPractice,
                              std::string* error) {
    w_ = &w;
    release(*w.anm);
    stage_ = stage;
    if (!w.anm->load(vfs, textures, "enemy.anm", kSlotFile0, error)) return false;
    if (!w.anm->load(vfs, textures, stageEnemyAnm(stage), kSlotFile1, error)) return false;
    const std::string name = std::format("{}{}.ecl", stageEcl(stage), spellPractice ? "sp" : "");
    const auto bytes = vfs.read(name);
    if (!bytes) {
        if (error) *error = name + " is not in the archive";
        return false;
    }
    if (!ecl_.load(*bytes, error)) return false;
    loaded_ = true;
    resetTemplate();
    for (Enemy& e : enemies_) e = Enemy{}, e.active = false;
    bosses_.fill(nullptr);
    globals_ = EclGlobals{};
    timelines_ = {};
    timelineGate_ = 0;
    signals_.fill(-1);
    spell_ = SpellCard{};
    bossPresent_ = false;
    bossBar_ = 0;
    managerTimer_ = zeroTimer();
    const int d = w.state->difficulty;
    rankMask_ = d >= 4 ? 0xFu : (1u << d);
    dropCounter_ = w.rng->u16InRange(3);
    dropIndex_ = w.rng->u16InRange(8);
    return true;
}

void EnemyManager::release(AnmLibrary& anm) {
    if (!loaded_) return;
    anm.release(kSlotFile0);
    anm.release(kSlotFile1);
    for (Enemy& e : enemies_) {
        for (auto& t : e.threads) t.reset();
    }
    ecl_.clear();
    loaded_ = false;
}

Enemy* EnemyManager::bossAt(int slot) const {
    return slot >= 0 && slot < kBossSlots ? bosses_[static_cast<std::size_t>(slot)] : nullptr;
}

// ---- the VM ---------------------------------------------------------------------------------

void EnemyManager::startSub(EclContext& c, std::int32_t sub) {
    if (sub < 0) return;  // nothing happens (spec 5.2)
    if (!ecl_.validSub(sub)) {
        eclWarnOnce(std::format("sub {} does not exist (the file has {})", sub, ecl_.subCount()));
        c.pos = ecl_.size();
    } else {
        c.pos = ecl_.subOffset(sub);
    }
    c.time.assign(0);
    c.wait.assign(0);
    c.sub = static_cast<std::int16_t>(sub);
}

void EnemyManager::freeThreads(Enemy& e) {
    for (auto& t : e.threads) t.reset();
}

void EnemyManager::startScript(Enemy& e, AnmVm& vm, std::int32_t script, bool file1) {
    (void)e;
    if (script < 0) return;
    vm.start(*w().anm, *w().rng, file1 ? kSlotFile1 : kSlotFile0, script);
}

// The interrupt entry (spec 5.3): the main context is pushed onto the current stack and
// restarted on the pending interrupt's sub.
void EnemyManager::interruptEntry(Enemy& e, std::uint32_t ip) {
    const std::uint32_t size = static_cast<std::uint64_t>(ip) + 12 <= ecl_.size() ? eclU16(ecl_.at(ip) + 6) : 0;
    cur().pos = ip + size;
    std::int16_t& depth = *curDepth_;
    if (!e.noStack) (*curStack_)[static_cast<std::size_t>(std::clamp<int>(depth, 0, 15))] = e.ctx;
    const std::int16_t slot = e.pendingInterrupt;
    if (slot >= 0 && slot < 32) startSub(e.ctx, e.interrupts[static_cast<std::size_t>(slot)]);
    if (depth < 15) ++depth;
    e.pendingInterrupt = -1;
}

void EnemyManager::callSub(Enemy& e, std::int32_t sub, std::uint32_t next) {
    cur().pos = next;
    std::int16_t& depth = *curDepth_;
    if (!e.noStack) (*curStack_)[static_cast<std::size_t>(std::clamp<int>(depth, 0, 15))] = cur();
    startSub(cur(), sub);
    cur().args.intG = globals_.ints;
    cur().args.floatG = globals_.floats;
    if (!e.noStack && depth < 15) ++depth;
}

float EnemyManager::interpValue(Enemy& e, EclInterp& s, float t) {
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

void EnemyManager::runInterpolators(Enemy& e, EclContext& c) {
    const Vec3f remembered = e.pos;
    bool touched = false;
    for (EclInterp& s : c.interps) {
        if (s.routine == EclInterp::None) continue;
        s.timer.tick();
        if (s.timer.current >= s.duration) s.timer.assign(s.duration);
        float t = s.timer.asFloat() / static_cast<float>(s.duration);
        t = eclEase(s.easing, t);
        const float v = interpValue(e, s, t);
        if (!writeFloat(e, x86ToInt(s.target), v)) s.target = v;  // overwrites its own target [QUIRK]
        if (s.timer.current >= s.duration) s.routine = EclInterp::None;
        if (s.target == 10042.0f || s.target == 10043.0f || s.target == 10044.0f) touched = true;
    }
    if (touched) {
        e.velocity.x = e.pos.x - remembered.x;
        e.velocity.y = e.pos.y - remembered.y;
        e.angle = zatan2(e.velocity.y, e.velocity.x);
        e.pos = remembered;
    }
}

// The end of one context's pass (spec 5.1 step 4).
void EnemyManager::endOfContext(Enemy& e, std::uint32_t ip) {
    EclContext& c = cur();
    if (e.life > 0) {
        if (c.exRoutine >= 0 && ecl_.validInstruction(c.exPos)) exRoutine(e, c.exRoutine, ecl_.at(c.exPos));
        runInterpolators(e, c);
    }
    c.pos = ip;
    c.time.tick();
}

bool EnemyManager::runVm(Enemy& e) {
    running_ = &e;
    cur_ = &e.ctx;
    curStack_ = &e.stack;
    curDepth_ = &e.depth;
    threadCursor_ = -1;
    std::uint32_t ip = e.ctx.pos;
    int budget = 1000000;
    bool reload = e.pendingInterrupt >= 0;
    if (reload) {
        interruptEntry(e, ip);
        ip = cur().pos;
    }
    for (;;) {
        // The loop over this context's instructions.
        bool ended = false;  // a thread returned at depth 0 and was freed
        while (!ended) {
            e.abs = e.pos + e.offset;
            e.abs.z = 0.0f;
            EclContext& c = cur();
            if (c.wait.current > 0) {
                c.wait.decrement(1);
                c.time.decrement(1);
                break;
            }
            if (!ecl_.validInstruction(ip)) break;
            const std::uint8_t* p = ecl_.at(ip);
            if (eclI32(p) != c.time.current) break;
            const std::uint16_t size = eclU16(p + 6);
            const std::uint32_t req = rankMask_ | e.extraMask;
            if ((p[9] & req) != req) {
                ip += size;
                continue;
            }
            if (--budget < 0) {
                eclWarnOnce(std::format("sub {} runs away (too many instructions in one pass)", c.sub));
                break;
            }
            const Exec r = execute(e, ip);
            switch (r) {
                case Exec::Delete:
                    cur_ = nullptr;
                    running_ = nullptr;
                    return false;
                case Exec::Next: ip += size; break;
                case Exec::Jumped: break;
                case Exec::Reload:
                    ip = cur().pos;
                    if (e.pendingInterrupt >= 0) {
                        interruptEntry(e, ip);
                        ip = cur().pos;
                    }
                    break;
                case Exec::ThreadEnded: ended = true; break;
            }
        }
        // End of this context (a finished thread skips it).
        if (!ended) endOfContext(e, ip);
        // The next thread.
        bool found = false;
        for (int k = threadCursor_ + 1; k < 4; ++k) {
            if (!e.threads[static_cast<std::size_t>(k)]) continue;
            EclThread& t = *e.threads[static_cast<std::size_t>(k)];
            cur_ = &t.ctx;
            curStack_ = &t.stack;
            curDepth_ = &t.depth;
            t.ctx.thread = k + 1;
            threadCursor_ = k;
            ip = t.ctx.pos;
            found = true;
            break;
        }
        if (!found) break;
    }
    cur_ = &e.ctx;
    curStack_ = &e.stack;
    curDepth_ = &e.depth;
    movementModeUpdate(e);
    if (e.life > 0) {
        if (e.shootInterval > 0) {
            e.shootTimer.tick();
            if (e.shootTimer.current >= e.shootInterval) {
                bulletOp(e, e.deferred.data());
                e.shootTimer.assign(0);
            }
        }
        poseUpdate(e);
    }
    cur_ = nullptr;
    running_ = nullptr;
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
        default: break;
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
    startScript(e, e.vm, script, e.file1);
    e.poseState = static_cast<std::uint8_t>(dir);
}

// TH07 opcode 52's exit angle (one float draw; uses P and the bounds, its pi - angle quirk).
float EnemyManager::exitAngle(Enemy& e) {
    ZunRng& rng = *w().rng;
    const float px = playerPos().x;
    constexpr float kHalfPi = kZunPi / 2.0f, kQuarterPi = kZunPi / 4.0f, kThreeQuarterPi = (kZunPi * 3.0f) / 4.0f;
    float a;
    if (px < e.pos.x) a = addNormalizeAngle(rng.f32InRange(kHalfPi) + kThreeQuarterPi, 0.0f);
    else a = rng.f32InRange(kHalfPi) - kQuarterPi;
    if (e.pos.x < e.boundsLowX + 96.0f) {
        if (a > kHalfPi) a = kZunPi - a;
        else if (a < -kHalfPi) a = -kZunPi - a;
    }
    if (e.boundsHighX - 96.0f < e.pos.x) {
        if (a >= 0.0f && a < kHalfPi) a = kZunPi - e.angle;  // the movement angle [QUIRK]
        else if (a > -kHalfPi && a <= 0.0f) a = -kZunPi - a;
    }
    if (e.boundsLowY + 48.0f > e.pos.y && a < 0.0f) a = -a;
    if (e.boundsHighY - 48.0f < e.pos.y && a > 0.0f) a = -a;
    return a;
}

// The bullet op helper (spec 6.9), on an instruction (or the deferred copy).
void EnemyManager::bulletOp(Enemy& e, const std::uint8_t* insConst) {
    auto* ins = const_cast<std::uint8_t*>(insConst);
    const std::int16_t op = eclI16(ins + 4);
    if (op < 96 || op > 104) return;
    const EclOps o{*this, e, ins, eclU16(ins + 10)};
    const auto flags = static_cast<std::uint32_t>(o.L(8));
    if ((flags & 0x8000) && !e.youkaiState) return;
    if ((flags & 0x10000) && e.youkaiState) return;
    const Vec3f pp = playerPos();
    if (e.noFireRadius2 > 0.0f) {
        const float dx = e.abs.x - pp.x, dy = e.abs.y - pp.y;
        if (dx * dx + dy * dy < e.noFireRadius2) return;
    }
    Shooter& s = e.shooter;
    s.pos = e.abs + e.shootOffset;
    s.type = static_cast<std::int16_t>(((o.mask >> 0) & 1u) ? readInt(e, o.H(0, 0)) : o.H(0, 0));
    s.aimMode = static_cast<std::uint16_t>(op - 96);
    s.count1 = static_cast<std::int16_t>(o.IB(1, 2));
    s.count2 = static_cast<std::int16_t>(o.IB(2, 3));
    s.angle1 = o.FB(5, 6);
    s.speed1 = o.FB(3, 4);
    s.angle2 = o.FB(6, 7);
    s.speed2 = o.FB(4, 5);
    if (!w().spellCardActive) {
        const std::int32_t r = w().state->rank;
        auto rankI = [&](std::int16_t lo, std::int16_t hi) { return lo + (hi - lo) * r / 32; };
        s.count1 = static_cast<std::int16_t>(std::max(1, s.count1 + rankI(e.rankAmounts[0], e.rankAmounts[1])));
        s.count2 = static_cast<std::int16_t>(std::max(1, s.count2 + rankI(e.rankAmounts[2], e.rankAmounts[3])));
        const float rs = e.rankSpeedLow + (e.rankSpeedHigh - e.rankSpeedLow) * static_cast<float>(r) / 32.0f;
        if (s.speed1 != 0.0f) {
            s.speed1 = std::max(0.3f, s.speed1 + rs);
            s.speed2 = std::max(0.3f, s.speed2 + rs / 2.0f);
        }
    }
    s.flags = flags;
    s.color = static_cast<std::int16_t>(((o.mask >> 1) & 1u) ? readInt(e, o.H(0, 1)) : o.H(0, 1));
    if (w().bullets) w().bullets->fire(w(), s);
}

// ---- the instructions -----------------------------------------------------------------------

EnemyManager::Exec EnemyManager::execute(Enemy& e, std::uint32_t& ip) {
    const World& wd = w();
    GameState& g = *wd.state;
    ZunRng& rng = *wd.rng;
    std::uint8_t* p = ecl_.at(ip);
    const std::int16_t op = eclI16(p + 4);
    const std::uint16_t size = eclU16(p + 6);
    const EclOps o{*this, e, p, eclU16(p + 10)};
    BulletManager* bm = wd.bullets;
    EclContext& c = cur();
    auto jump = [&](std::int32_t time, std::int32_t offset) {
        c.time.current = time;
        ip = static_cast<std::uint32_t>(static_cast<std::int64_t>(ip) + offset);
        return Exec::Jumped;
    };
    auto laserRef = [&](std::int32_t k) -> Laser* {
        if (k < 0 || k >= 32) return nullptr;
        const std::int16_t slot = e.lasers[static_cast<std::size_t>(k)];
        return slot >= 0 && bm ? &bm->laser(slot) : nullptr;
    };
    auto intOp = [&](auto f) {
        const std::int32_t b = o.I(1);
        const std::int32_t a = o.getIW(0);
        o.setIW(0, f(a, b));
    };
    auto floatOp = [&](auto f) {
        const float b = o.F(1);
        const float a = o.getFW(0);
        o.setFW(0, f(a, b));
    };
    switch (op) {
        // ---- control ----
        case 1: return Exec::Delete;
        case 2: c.wait.assign(o.I(0)); break;
        case 4: return jump(o.L(0), o.L(1));
        case 5: {
            o.setIW(2, o.getIW(2) - 1);
            if (o.I(2) > 0) return jump(o.L(0), o.L(1));
            break;
        }
        case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49: case 50: case 51: {
            bool holds;
            const int kind = (op - 40) / 2;
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
        case 52:
            callSub(e, o.L(0), ip + size);
            return Exec::Reload;
        case 53: {
            std::int16_t& depth = *curDepth_;
            --depth;
            if (depth < 0) {
                if (c.thread > 0) {
                    const int k = c.thread - 1;
                    depth = 0;
                    e.threads[static_cast<std::size_t>(k)].reset();
                    cur_ = &e.ctx;
                    curStack_ = &e.stack;
                    curDepth_ = &e.depth;
                    return Exec::ThreadEnded;
                }
                eclWarnOnce("a return at depth 0 in a main context (ignored)");
                depth = 0;
                break;
            }
            cur() = (*curStack_)[static_cast<std::size_t>(std::clamp<int>(depth, 0, 15))];
            return Exec::Reload;
        }
        case 125:
            e.pendingInterrupt = static_cast<std::int16_t>(o.I(0));
            interruptEntry(e, ip);
            return Exec::Reload;
        case 126: {
            const std::int32_t sub = o.I(0), slot = o.I(1);
            if (slot >= 0 && slot < 32) e.interrupts[static_cast<std::size_t>(slot)] = static_cast<std::int16_t>(sub);
            break;
        }
        case 135: {
            const std::int32_t slot = o.I(0);
            if (slot < 0 || slot >= 4) break;
            const bool own = threadCursor_ == slot;
            if (own) {
                eclWarnOnce("a thread replaces its own slot (kept running)");
                break;
            }
            e.threads[static_cast<std::size_t>(slot)].reset();
            const std::int32_t sub = o.I(1);
            if (sub >= 0) {
                auto t = std::make_unique<EclThread>();
                t->sub = sub;
                startSub(t->ctx, o.I(1));
                t->ctx.args = c.args;
                t->ctx.thread = slot + 1;
                e.threads[static_cast<std::size_t>(slot)] = std::move(t);
            }
            break;
        }
        case 136: exRoutine(e, o.I(0), p); break;
        case 137: {
            const std::int32_t n = o.I(0);
            if (n >= 0) {
                c.exRoutine = static_cast<std::int8_t>(o.I(0));
                c.exPos = ip;
            } else {
                c.exRoutine = -1;
            }
            break;
        }
        case 146: c.time.increment(o.I(0)); break;
        case 151: e.noStack = o.B(0) != 0; break;

        // ---- assignment and arithmetic ----
        case 6: o.setIW(0, o.I(1)); break;
        case 7: o.setFW(0, o.F(1)); break;
        case 8: {
            const std::int32_t v = o.I(1);
            o.setIW(0, (rng.u16() & 1) ? v : -v);
            break;
        }
        case 9: {
            const bool plus = (rng.u16() & 1) != 0;
            const float v = o.F(1);
            o.setFW(0, (plus ? 1.0f : -1.0f) * v);
            break;
        }
        case 10: intOp([](std::int32_t a, std::int32_t b) { return a + b; }); break;
        case 11: intOp([](std::int32_t a, std::int32_t b) { return a - b; }); break;
        case 12: intOp([](std::int32_t a, std::int32_t b) { return a * b; }); break;
        case 13: intOp([](std::int32_t a, std::int32_t b) { return b == 0 ? a : a / b; }); break;
        case 14: intOp([](std::int32_t a, std::int32_t b) { return b == 0 ? a : a % b; }); break;
        case 15: floatOp([](float a, float b) { return a + b; }); break;
        case 16: floatOp([](float a, float b) { return a - b; }); break;
        case 17: floatOp([](float a, float b) { return a * b; }); break;
        case 18: floatOp([](float a, float b) { return a / b; }); break;
        case 19: {
            const float b = o.F(1);
            const float a = readFloat(e, o.LF(0));
            o.setFW(0, std::fmod(a, b));
            break;
        }
        case 20: case 21: case 22: case 23: case 24: {
            const std::int32_t a = o.I(1), b = o.I(2);
            std::int32_t r = 0;
            switch (op) {
                case 20: r = a + b; break;
                case 21: r = a - b; break;
                case 22: r = a * b; break;
                case 23: r = b == 0 ? a : a / b; break;
                default: r = b == 0 ? a : a % b; break;
            }
            o.setIW(0, r);
            break;
        }
        case 25: case 26: case 27: case 28: {
            const float a = o.F(1), b = o.F(2);
            o.setFW(0, op == 25 ? a + b : op == 26 ? a - b : op == 27 ? a * b : a / b);
            break;
        }
        case 29: {
            const float b = o.F(2), a = o.F(1);
            o.setFW(0, std::fmod(a, b));
            break;
        }
        case 30: o.setIW(0, o.getIW(0) + 1); break;
        case 31: o.setIW(0, o.getIW(0) - 1); break;
        case 32: o.setFW(0, zsin(o.F(1))); break;
        case 33: o.setFW(0, zcos(o.F(1))); break;
        case 34: {
            const float x2 = o.F(3), x1 = o.F(1), y2 = o.F(4), y1 = o.F(2);
            o.setFW(0, zatan2(y2 - y1, x2 - x1));
            break;
        }
        case 35: {
            const float a = o.F(1), b = o.F(2), t = o.F(3);
            o.setFW(0, (a - b) * t + o.F(2));
            break;
        }
        case 36: {
            const float target = o.LF(0);
            EclInterp* slot = nullptr;
            for (EclInterp& s : c.interps) {
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
            slot->routine = slot->routineIndex >= 0 && slot->routineIndex <= 6 ? EclInterp::Linear
                            : slot->routineIndex == 7                          ? EclInterp::Hermite
                                                                               : EclInterp::None;
            slot->p0 = o.F(4);
            slot->p1 = o.F(5);
            slot->m0 = o.F(6);
            slot->m1 = o.F(7);
            break;
        }
        case 37: o.setFW(0, addNormalizeAngle(o.getFW(0), 0.0f)); break;
        case 38: {
            const float a = addNormalizeAngle(o.F(2), 0.0f);
            const float m = o.F(3);
            o.setFW(0, cosTimes(a, m));
            o.setFW(1, sinTimes(a, m));
            break;
        }
        case 39: {
            const float x1 = o.F(1), x2 = o.F(3), y1 = o.F(2), y2 = o.F(4);
            const float dx = x1 - x2, dy = y1 - y2;
            o.setFW(0, std::sqrt(dx * dx + dy * dy));
            break;
        }
        case 166: {
            const float a = o.F(2), m = o.F(3);
            o.setFW(1, sinTimes(a, m));
            o.setFW(0, cosTimes(o.F(2), o.F(3)));
            break;
        }

        // ---- movement ----
        case 63:
            e.pos = {o.F(0), o.F(1), 0.0f};
            clampToBounds(e);
            break;
        case 64: {
            const float x = o.F(2), y = o.F(3);
            const std::int32_t t = o.I(0), ease = o.I(1);
            e.moveDelta = Vec3f{x, y, 0.0f} - e.abs;
            e.moveOrigin = e.pos;
            e.moveTimer.assign(t);
            e.moveDuration = t;
            e.easing = static_cast<std::uint8_t>(ease & 7);
            e.moveMode = Enemy::Interpolate;
            e.velocity = {};
            if (e.mirror) e.moveDelta.x = -e.moveDelta.x;
            break;
        }
        case 65:
            e.angle = addNormalizeAngle(o.F(0), 0.0f);
            e.speed = o.F(1);
            e.moveMode = Enemy::Polar;
            e.moveDuration = 0;
            e.moveTimer.assign(0);
            break;
        case 66:
        case 67:
        case 178: {
            float a;
            float speed;
            std::int32_t t;
            if (op == 66) {
                t = o.I(0);
                if (t <= 0) {
                    e.angle = addNormalizeAngle(o.F(2), 0.0f);
                    e.speed = o.F(3);
                    e.moveMode = Enemy::Polar;
                    e.moveDuration = 0;
                    e.moveTimer.assign(0);
                    break;
                }
                a = addNormalizeAngle(o.F(2), 0.0f);
                speed = o.F(3);
            } else {
                if (op == 67) {
                    a = exitAngle(e);
                } else {
                    const std::uint32_t r = rng.u32InRange(4);
                    const Vec3f pp = playerPos();
                    if (r != 0) {
                        const bool leftCase = pp.x < e.pos.x;
                        const float d1 = leftCase ? e.pos.x - pp.x : pp.x - e.pos.x;
                        const float d2 = leftCase ? (pp.x + 384.0f) - e.pos.x : e.pos.x - (pp.x - 384.0f);
                        if (leftCase) {
                            a = d2 > d1 ? addNormalizeAngle(rng.f32InRange(kZunPi / 2.0f) + 3.0f * kZunPi / 4.0f, 0.0f)
                                        : addNormalizeAngle(rng.f32InRange(kZunPi / 2.0f) - kZunPi / 4.0f, 0.0f);
                        } else {
                            a = d2 > d1 ? rng.f32InRange(kZunPi / 2.0f) - kZunPi / 4.0f
                                        : addNormalizeAngle(rng.f32InRange(kZunPi / 2.0f) + 3.0f * kZunPi / 4.0f, 0.0f);
                        }
                    } else {
                        a = (static_cast<float>(rng.u32()) / 2147483648.0f - 1.0f) * kZunPi;
                    }
                    if (e.boundsLowY + 48.0f > e.pos.y && a < 0.0f) a = -a;
                    if (e.boundsHighY - 48.0f < e.pos.y && a > 0.0f) a = -a;
                }
                t = o.I(0);
                speed = o.F(2);
                if (t <= 0) {
                    e.angle = a;
                    e.speed = speed;
                    e.moveMode = Enemy::Polar;
                    e.moveDuration = 0;
                    e.moveTimer.assign(0);
                    break;
                }
            }
            e.moveDelta.x = cosTimes(a, speed) * static_cast<float>(t);
            e.moveDelta.y = sinTimes(a, speed) * static_cast<float>(t);
            e.moveDelta.z = 0.0f;
            e.moveOrigin = e.abs;  // A, not P [QUIRK]
            e.moveTimer.assign(t);
            e.moveDuration = t;
            e.easing = static_cast<std::uint8_t>(o.I(1) & 7);
            e.moveMode = Enemy::Interpolate;
            if (op == 66 && e.mirror) e.moveDelta.x = -e.moveDelta.x;
            break;
        }
        case 68:
            e.angle = addNormalizeAngle(o.F(0) + angleToPlayer(e.pos), 0.0f);
            e.speed = o.F(1);
            break;
        case 70:
            e.angularVelocity = o.F(0);
            e.moveMode = Enemy::Polar;
            break;
        case 71:
            e.accel = o.F(0);
            e.moveMode = Enemy::Polar;
            break;
        case 72:
        case 73:
        case 74: {
            const std::int32_t t = o.I(0);
            if (op == 72) {
                e.moveOrigin.x = o.F(1);
                e.moveOrigin.y = o.F(2);
                e.orbitAngle = o.F(3);
                e.orbitAngularVelocity = o.F(4);
                e.orbitRadius = o.F(5);
                e.orbitRadialVelocity = o.F(6);
            } else if (op == 73) {
                e.moveOrigin = e.pos;
                e.orbitRadius = 0.0f;
                e.orbitAngle = o.F(1);
                e.orbitAngularVelocity = o.F(2);
                e.orbitRadialVelocity = o.F(3);
            } else {
                e.orbitAngularVelocity = o.F(1);
                e.orbitRadialVelocity = o.F(2);
            }
            e.moveTimer.assign(t);
            e.moveDuration = t;
            e.moveMode = Enemy::Orbit;
            break;
        }
        case 75:
            e.boundsLowX = o.F(0), e.boundsLowY = o.F(1), e.boundsHighX = o.F(2), e.boundsHighY = o.F(3);
            e.bounds = true;
            break;
        case 76: e.bounds = false; break;
        case 77: e.hitbox = {o.F(0), o.F(1), 24.0f}; break;
        case 78: e.box2 = {o.F(0), o.F(1), e.box2.z}; break;

        // ---- animation ----
        case 54: case 58:
            startScript(e, e.vm, o.I(0), op == 58);
            e.file1 = op == 58;
            break;
        case 55: case 59: {
            const std::int32_t n = o.I(0);
            // n default, n+1 left, n+2 right, n+3 after-left, n+4 after-right, n+5 extra.
            e.pose = {static_cast<std::int16_t>(n), static_cast<std::int16_t>(n + 3), static_cast<std::int16_t>(n + 4),
                      static_cast<std::int16_t>(n + 1), static_cast<std::int16_t>(n + 2), static_cast<std::int16_t>(n + 5)};
            e.poseState = 255;
            e.file1 = op == 59;
            break;
        }
        case 56: case 60: {
            const std::int32_t i5 = o.I(5), i4 = o.I(4), i3 = o.I(3), i2 = o.I(2), i1 = o.I(1), i0 = o.I(0);
            e.pose = {static_cast<std::int16_t>(i0), static_cast<std::int16_t>(i3), static_cast<std::int16_t>(i4),
                      static_cast<std::int16_t>(i1), static_cast<std::int16_t>(i2), static_cast<std::int16_t>(i5)};
            e.poseState = 255;
            e.file1 = op == 60;
            break;
        }
        case 57: case 61: {
            if (op == 61) e.file1 = true;
            const std::int32_t slot = o.I(0), script = o.I(1);
            if (slot >= 0 && slot < 2) {
                if (script >= 0) {
                    startScript(e, e.vms2[static_cast<std::size_t>(slot)], script, e.file1);
                    e.vm2File[static_cast<std::size_t>(slot)] = e.file1 ? 1 : 0;
                } else {
                    e.vm2File[static_cast<std::size_t>(slot)] = -1;
                }
            }
            e.file1 = op == 61;
            break;
        }
        case 62: startScript(e, e.vm, e.pose[5], e.file1); break;
        case 149: e.vm.interrupt(o.I(0)); break;
        case 150: {
            const std::int32_t slot = o.L(0);
            if (slot >= 0 && slot < 2) e.vms2[static_cast<std::size_t>(slot)].interrupt(o.H(1, 0));
            break;
        }
        case 165: e.vm.rotation.z = o.F(0); break;
        case 157: {
            e.trailFlags = o.B(0);
            e.trailCount = static_cast<std::int16_t>(std::clamp(o.I(1), 0, 96));
            e.trailInterval = static_cast<std::int16_t>(o.I(2));
            e.nodeStep = static_cast<std::int16_t>(o.I(3));
            break;
        }

        // ---- flags, life, callbacks ----
        case 79: case 80: case 81: {
            const std::int32_t v = o.I(0);
            auto apply = [&](int k, bool on) {
                switch (k) {
                    case 0: e.hittable = !on; break;
                    case 1: e.contact = !on; break;
                    case 2: e.damageable = !on; break;
                    case 3: e.noCollision = on; break;
                    case 4: e.oobDisabled = on; break;
                    case 5: e.cannotDie = on; break;
                    default: break;
                }
            };
            for (int k = 0; k < 6; ++k) {
                const bool b = (v >> k) & 1;
                if (op == 79) apply(k, b);
                else if (b) apply(k, op == 80);
            }
            break;
        }
        case 83: e.sparkle = (o.I(0) & 1) != 0; break;
        case 129:
            if (!((wd.mode & ModeSpellPractice) && (wd.mode & ModeDissolve))) e.deathType = static_cast<std::uint8_t>(o.B(0) & 7);
            break;
        case 130:
            if (!((wd.mode & ModeSpellPractice) && (wd.mode & ModeDissolve))) e.deathSub = static_cast<std::int16_t>(o.L(0) & 0xFFFF);
            break;
        case 131:
            e.maxLife = o.I(0);
            e.life = o.I(0);
            e.phaseLife = o.I(0);
            break;
        case 132: e.timer.assign(o.I(0)); break;
        case 133: {
            const std::int32_t threshold = o.I(1), index = o.I(0), sub = o.I(2);
            if (index >= 0 && index < 4) {
                e.lifeThresholds[static_cast<std::size_t>(index)] = threshold;
                if (!((wd.mode & ModeSpellPractice) && (wd.mode & ModeDissolve))) e.lifeSubs[static_cast<std::size_t>(index)] = sub;
            }
            break;
        }
        case 134:
            e.timerThreshold = o.I(0);
            if (!((wd.mode & ModeSpellPractice) && (wd.mode & ModeDissolve))) e.timerSub = o.I(1);
            e.timer.assign(0);
            break;
        case 153:
            e.timerSub = e.deathSub;
            e.timer.assign(0);
            break;
        case 155:
            e.survival = (o.B(0) & 1) != 0;
            spell_.displayed = 99999990u;
            break;
        case 156:
            e.projectile = o.B(0) != 0;
            e.drawLayer = 2;
            break;
        case 159: e.drawLayer = static_cast<std::uint8_t>(std::clamp(o.I(0), 0, 3)); break;
        case 160: e.invincible.assign(o.I(0)); break;
        case 173: e.frozenWhileBombing = (o.I(0) & 1) != 0; break;
        case 183: e.intangibleWhileBombing = (o.I(0) & 1) != 0; break;
        case 145: case 182: break;  // draw-only flags
        case 177: e.phaseLife = o.I(0); break;
        case 82: {
            const float r = o.F(0);
            e.noFireRadius2 = r * r;
            break;
        }
        case 175: timelineGate_ = o.I(0); break;
        case 163: break;

        // ---- bosses and spell cards ----
        case 127: {
            const std::int32_t slot = o.I(0);
            if (slot >= 0) {
                if (slot < kBossSlots) bosses_[static_cast<std::size_t>(slot)] = &e;
                if (slot == 0) {
                    bossPresent_ = true;
                    bossBar_ = 1.0f;
                }
                e.boss = true;
                e.bossId = static_cast<std::uint8_t>(slot);
                e.noFireRadius2 = 0.0f;
            } else {
                if (e.bossId < 4) {
                    bossPresent_ = false;
                    bosses_[e.bossId] = nullptr;
                }
                e.boss = false;
                releaseCircles(e);
            }
            break;
        }
        case 128: {
            if (e.circleCount >= static_cast<std::int32_t>(e.circles.size())) {
                log::warn("ECL op 128: more than 24 spell circles; ignored");
                break;
            }
            if (!wd.effects) break;
            Effect& c = wd.effects->spawn(wd, 13, e.pos, 1, 0xFF6060D0u);
            e.circles[static_cast<std::size_t>(e.circleCount)] = &c;
            c.direction = {o.F(1), o.F(2), o.F(3)};
            e.circleRadius = o.F(4);
            ++e.circleCount;
            break;
        }
        case 158: case 148: break;  // the GUI's bar segments and life markers (boss HUD to come)
        case 122: spellCardStart(e, p); break;
        case 123: spellCardEnd(); break;
        case 164: break;
        case 176: {
            // The Last Spell marker (spec 12.7): state 1 in mode bits 7-8 (bombs refused, a hit
            // starts the Dissolve spell), bit 13 in the final stages, and the enemy freezes
            // while the player bombs.
            wd.mode = (wd.mode & ~static_cast<std::uint32_t>(ModeDissolve | ModeBit13)) | (1u << 7);
            const int card = spell_.id;
            const bool sp = (wd.mode & ModeSpellPractice) != 0;
            if ((!sp && (stage_ == 6 || stage_ == 7)) || (sp && ((card >= 143 && card <= 146) || (card >= 171 && card <= 190)))) {
                wd.mode |= ModeBit13;
            }
            e.frozenWhileBombing = true;
            break;
        }
        case 184: spell_.noDrain = (o.I(0) & 1) != 0; break;
        case 86: {
            Enemy* b = bossAt(o.I(2));
            if (b) o.setIW(0, ((o.mask >> 1) & 1u) ? readInt(*b, o.L(1)) : o.L(1));
            break;
        }
        case 87: {
            Enemy* b = bossAt(o.I(2));
            if (b) o.setFW(0, ((o.mask >> 1) & 1u) ? readFloat(*b, o.LF(1)) : o.LF(1));
            break;
        }
        case 88: {
            Enemy* b = bossAt(o.I(0));
            if (b) {
                startSub(b->ctx, o.L(1));  // (the boss's own call semantics, approximated)
            }
            break;
        }
        case 89: {
            Enemy* b = bossAt(o.I(0));
            if (b) b->pendingInterrupt = static_cast<std::int16_t>(o.I(1));
            break;
        }

        // ---- spawning ----
        case 93:
        case 94: {
            const float x = o.F(1), y = o.F(2), z = o.F(3);
            const std::int32_t score = o.I(6), item = o.I(5), life = o.I(4);
            if (e.life <= 0) break;
            Vec3f at{x, y, z};
            if (op == 94) at = at + e.pos;
            spawnWithContext(o.L(0), at, life, item, score, c.args);
            break;
        }
        case 95: killAllSimple(nullptr); break;
        case 90: case 91: case 92: {
            const float x = o.F(1), y = o.F(2);
            const std::int32_t score = o.I(5), item = o.I(4), life = o.I(3);
            spawnFamiliar(e, op, o.L(0), {x, y, 0.0f}, life, item, score);
            break;
        }

        // ---- items, effects, sound, misc ----
        case 124: wd.playSound(o.I(0)); break;
        case 139: {
            const std::int32_t count = o.I(1), type = o.I(0);
            const auto color = static_cast<std::uint32_t>(o.getIW(2));
            if (wd.effects) wd.effects->spawn(wd, type, e.pos, count, color);
            break;
        }
        case 140: {
            const Vec3f custom{o.F(3), o.F(4), o.F(5)};
            const std::int32_t count = o.I(1), type = o.I(0);
            const auto color = static_cast<std::uint32_t>(o.getIW(2));
            if (wd.effects) wd.effects->spawnWithVector(wd, type, e.pos, custom, count, color);
            break;
        }
        case 141:
            if (bm) bm->spawnItem(wd, e.pos, o.I(0), BulletManager::Falling);
            break;
        case 142:
        case 168: {
            const std::int32_t n = o.I(0);
            for (int k = 0; k < n; ++k) {
                Vec3f at = e.pos;
                at.x += rng.f32InRange(128.0f) - 64.0f;
                at.y += rng.f32InRange(128.0f) - 64.0f;
                int type = BulletManager::Point;
                if (op == 142) type = g.power < 128.0f ? (k == 0 ? BulletManager::BigPower : BulletManager::SmallPower) : BulletManager::Point;
                if (bm) bm->spawnItem(wd, at, type, BulletManager::Falling);
            }
            break;
        }
        case 143: e.itemDrop = o.I(0); break;
        case 144:
            e.extraPoints = o.I(0);
            e.extraPower = o.I(1);
            break;
        case 138:
            e.death1 = static_cast<std::int8_t>(o.word(0)[0]);
            e.death2 = o.word(0)[1];
            e.death3 = o.word(0)[2];
            break;
        case 147:
            if (wd.background) wd.background->setResumeLabel(o.I(0));
            break;
        case 152:
            e.rankSpeedLow = o.F(0);
            e.rankSpeedHigh = o.F(1);
            for (int k = 0; k < 4; ++k) e.rankAmounts[static_cast<std::size_t>(k)] = static_cast<std::int16_t>(o.I(2 + k));
            break;
        case 174: break;  // the familiar aura (visual)
        case 179: case 180: break;  // the night clock (GUI)
        case 181:
            if (g.nightSteps < 12) {
                wd.playSound(45);
                ++g.nightSteps;
            }
            break;
        case 112:
            if (bm) bm->clearAll(wd, 1);
            break;
        case 162:
            if (bm) bm->clearAll(wd, 4);
            break;
        case 161: break;

        // ---- bullets ----
        case 96: case 97: case 98: case 99: case 100: case 101: case 102: case 103: case 104:
            if (e.life <= 0) break;
            if (e.deferredBullets) {
                std::memcpy(e.deferred.data(), p, std::min<std::size_t>(size, e.deferred.size()));
                break;
            }
            bulletOp(e, p);
            break;
        case 105:
        case 106: {
            std::int32_t interval = o.I(0);
            if (interval != 0) {
                const std::int32_t r = g.rank;
                interval += r * ((-interval / 5) - (interval / 5)) / 32 + interval / 5;
                e.shootTimer.assign(op == 106 && interval > 0 ? static_cast<std::int32_t>(rng.u32InRange(static_cast<std::uint32_t>(interval))) : 0);
            }
            e.shootInterval = interval;
            break;
        }
        case 107: e.deferredBullets = true; break;
        case 108: e.deferredBullets = false; break;
        case 109:
            e.shooter.pos = e.pos + e.shootOffset;
            if (bm) bm->fire(wd, e.shooter);
            break;
        case 110: e.shootOffset = {o.F(0), o.F(1), 0.0f}; break;
        case 111: {
            const std::int32_t index = o.I(0);
            if (index < 0 || index >= 6) break;
            BulletCommand& cmd = e.shooter.commands[static_cast<std::size_t>(index)];
            cmd.type = static_cast<std::uint32_t>(o.I(1));
            cmd.immediate = o.I(2);
            cmd.duration = o.I(3);
            cmd.loops = o.I(4);
            cmd.a = o.F(5);
            cmd.b = o.F(6);
            break;
        }
        case 113: {
            const std::int32_t s = o.I(0);
            if (s >= 0) {
                e.shooter.sound = o.I(0);
                e.shooter.flags |= 0x200;
            } else {
                e.shooter.flags &= ~0x200u;
            }
            e.shooter.bulletSound = o.I(1);
            break;
        }

        // ---- lasers ----
        case 114:
        case 115: {
            Shooter& s = e.laserShooter;
            s.type = o.H(0, 0);
            s.color = static_cast<std::int16_t>(((o.mask >> 1) & 1u) ? readInt(e, o.H(0, 1)) : o.H(0, 1));
            s.pos = e.abs + e.shootOffset;
            s.angle1 = o.FB(1, 2);
            s.speed1 = o.FB(2, 3);
            const float startOff = o.FB(3, 4), endOff = o.FB(4, 5), startLen = o.FB(5, 6), width = o.FB(6, 7);
            const std::int32_t startTime = o.IB(7, 8), duration = o.IB(8, 9), endTime = o.IB(9, 10);
            const std::int32_t hbStart = o.L(10), hbEnd = o.L(11);
            const auto flags = static_cast<std::uint16_t>(o.L(12));
            if (bm) {
                Laser* l = bm->spawnLaser(wd, s, startOff, endOff, startLen, width, startTime, duration, endTime, hbStart, hbEnd, flags, op == 115);
                if (e.laserIndex >= 0 && e.laserIndex < 32) e.lasers[static_cast<std::size_t>(e.laserIndex)] = static_cast<std::int16_t>(bm->laserIndex(l));
            }
            break;
        }
        case 116: e.laserIndex = o.I(0); break;
        case 117:
            if (Laser* l = laserRef(o.I(0))) l->angle = addNormalizeAngle(l->angle + o.F(1), 0.0f);
            break;
        case 167:
            if (Laser* l = laserRef(o.I(0))) l->angle = o.F(1);
            break;
        case 118:
            if (Laser* l = laserRef(o.I(0))) l->angle = angleToPlayer(l->origin) + o.F(1);
            break;
        case 119:
            if (Laser* l = laserRef(o.I(0))) l->origin = e.abs + Vec3f{o.F(1), o.F(2), o.F(3)};
            break;
        case 120: {
            Laser* l = laserRef(o.I(0));
            c.args.intB[2] = l && l->inUse ? 1 : 0;
            break;
        }
        case 121:
            if (Laser* l = laserRef(o.I(0)); l && bm) bm->stopLaser(*l);
            break;
        case 154: e.lasers.fill(-1); break;

        default: break;  // 0, 3, 84, 85 and unknown opcodes do nothing
    }
    return Exec::Next;
}

// ---- timelines (spec 7) -----------------------------------------------------------------

void EnemyManager::timelineSpawn(const std::uint8_t* rec, int op) {
    ZunRng& rng = *w().rng;
    const std::int32_t sub = eclI32(rec + 8);
    float x = eclF32(rec + 12), y = eclF32(rec + 16);
    std::int32_t life, item, score;
    const bool mirror = op == 1 || op == 4 || op == 5 || op == 12;
    if (op == 2 || op == 4) {
        const float x1 = eclF32(rec + 12), x2 = eclF32(rec + 16);
        y = eclF32(rec + 20);
        x = rng.f32InRange(x2 - x1) + x1;
        life = eclI32(rec + 24), item = eclI32(rec + 28), score = eclI32(rec + 32);
    } else if (op == 3 || op == 5) {
        y = eclF32(rec + 12);
        x = rng.f32InRange(384.0f);
        life = eclI32(rec + 16), item = eclI32(rec + 20), score = eclI32(rec + 24);
    } else {
        life = eclI32(rec + 20), item = eclI32(rec + 24), score = eclI32(rec + 28);
    }
    Enemy& e = spawn(sub, {x, y, 0.0f}, life, op == 11 || op == 12 ? -1 : item, op == 11 || op == 12 ? eclI32(rec + 36) : score, mirror);
    if ((op == 11 || op == 12) && !isSpare(e)) {
        e.extraPoints = eclI32(rec + 28);
        e.extraPower = eclI32(rec + 32);
    }
}

void EnemyManager::runTimelines() {
    for (int i = 0; i < ecl_.timelineCount(); ++i) {
        Timeline& t = timelines_[static_cast<std::size_t>(i)];
        if (!t.started) {
            t.started = true;
            t.pos = ecl_.timelineOffset(i);
        }
        bool stopped = false;
        while (!stopped && static_cast<std::uint64_t>(t.pos) + 8 <= ecl_.size()) {
            const std::uint8_t* rec = ecl_.at(t.pos);
            const std::int32_t time = eclI32(rec);
            if (time < 0) break;
            if (t.clock.current < time) break;
            const std::uint8_t size = rec[6];
            if (t.clock.current == time && (rec[7] & rankMask_) != 0) {
                const std::int16_t op = eclI16(rec + 4);
                const bool gated = bossPresent_ || timelineGate_ != 0;
                switch (op) {
                    case 0: case 1: case 2: case 3: case 4: case 5: case 11: case 12:
                        if (!gated) timelineSpawn(rec, op);
                        break;
                    case 15: timelineSpawn(rec, 0); break;
                    case 6: break;  // dialogue (to come)
                    case 7: break;  // waits for the dialogue (none yet)
                    case 8: {
                        Enemy* b = bossAt(eclI32(rec + 8));
                        if (b) b->pendingInterrupt = eclI16(rec + 12);
                        break;
                    }
                    case 9: break;
                    case 10: {
                        Enemy* b = bossAt(eclI32(rec + 8));
                        if (b && b->active) {
                            t.clock.decrement(1);
                            stopped = true;
                        }
                        break;
                    }
                    case 13: {
                        const std::int32_t n = eclI32(rec + 8);
                        bool found = false;
                        for (std::int32_t& sgn : signals_) {
                            if (sgn == n) sgn = -1, found = true;
                        }
                        if (!found) {
                            t.clock.decrement(1);
                            stopped = true;
                        }
                        break;
                    }
                    case 14: {
                        const std::int32_t n = eclI32(rec + 8);
                        for (std::int32_t& sgn : signals_) {
                            if (sgn < 0) sgn = n;
                        }
                        break;
                    }
                    default: break;
                }
                if (stopped) break;
            }
            if (size == 0) break;
            t.pos += size;
        }
        t.clock.tick();
    }
}

// ---- spawning (spec 8.9) ----------------------------------------------------------------

Enemy& EnemyManager::spawn(int sub, const Vec3f& pos, std::int32_t life, std::int32_t item, std::int32_t score, bool mirror) {
    Enemy* slot = &enemies_[kEnemies];
    for (int i = 0; i < kEnemies; ++i) {
        if (!enemies_[static_cast<std::size_t>(i)].active) {
            slot = &enemies_[static_cast<std::size_t>(i)];
            break;
        }
    }
    spawnFailed_ = isSpare(*slot);
    if (spawnFailed_) return *slot;
    Enemy& e = *slot;
    const int index = static_cast<int>(slot - enemies_.data());
    e = Enemy{};
    e.active = true;
    e.lasers.fill(-1);
    e.slot = index;
    e.mirror = mirror;
    if (life >= 0) e.life = life;
    e.pos = pos;
    e.abs = pos;
    startSub(e.ctx, sub);
    if (!runVm(e)) {
        e.active = false;
        spawnFailed_ = true;
        return e;
    }
    e.color = e.vm.color;
    e.itemDrop = static_cast<std::int8_t>(item);
    if (score >= 0) e.score = score;
    e.maxLife = e.phaseLife = e.life;
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
    spawnFailed_ = isSpare(*slot);
    if (spawnFailed_) return *slot;
    Enemy& e = *slot;
    const int index = static_cast<int>(slot - enemies_.data());
    // The caller's state (the running VM) is saved around the child's first pass.
    EclContext* savedCur = cur_;
    auto* savedStack = curStack_;
    std::int16_t* savedDepth = curDepth_;
    const int savedCursor = threadCursor_;
    Enemy* savedRunning = running_;
    e = Enemy{};
    e.active = true;
    e.lasers.fill(-1);
    e.slot = index;
    if (life >= 0) e.life = life;
    e.pos = pos;
    e.abs = pos;
    e.ctx.args = args;
    startSub(e.ctx, sub);
    const bool ok = runVm(e);
    cur_ = savedCur;
    curStack_ = savedStack;
    curDepth_ = savedDepth;
    threadCursor_ = savedCursor;
    running_ = savedRunning;
    if (!ok) {
        e.active = false;
        spawnFailed_ = true;
        return e;
    }
    if (life >= 0) e.life = life;
    e.color = e.vm.color;
    e.itemDrop = static_cast<std::int8_t>(item);
    if (score >= 0) e.score = score;
    e.maxLife = e.phaseLife = e.life;
    return e;
}

// ---- familiars (spec 9) -----------------------------------------------------------------

void EnemyManager::spawnFamiliar(Enemy& parent, int op, std::int32_t sub, const Vec3f& pos, std::int32_t life, std::int32_t item,
                                 std::int32_t score) {
    const World& wd = w();
    if (parent.life <= 0 || parent.dismissed) {
        spawnFailed_ = true;
        wd.playSound(36);
        return;
    }
    Vec3f at = pos;
    if (op == 91) at = parent.abs + pos;
    EclArgs args = cur_ ? cur_->args : parent.ctx.args;
    Enemy& child = spawnWithContext(sub, at, life, item, score, args);
    if (!spawnFailed_) {
        child.familiar = true;
        child.youkaiState = wd.player && wd.player->youkaiInFront();
        child.drawLayer = child.youkaiState ? 0 : 2;
        child.contact = false;
        if (op == 92) {
            child.offset = parent.pos;
            child.abs = child.pos + child.offset;
            child.attached = true;
        }
        child.parent = &parent;
        Enemy* last = &parent;
        while (last->next) last = last->next;
        last->next = &child;
        child.prev = last;
        ++parent.familiarCount;
    }
    wd.playSound(36);
}

void EnemyManager::unlinkFromParent(Enemy& e) {
    if (e.prev) e.prev->next = e.next;
    if (e.next) e.next->prev = e.prev;
    e.prev = e.next = nullptr;
    e.parent = nullptr;
}

void EnemyManager::familiarPreUpdate(Enemy& e) {
    const World& wd = w();
    const bool youkai = wd.player && wd.player->youkaiInFront();
    const bool wasHuman = !e.youkaiState;
    if (!e.youkaiState && youkai) {
        if (wd.effects) wd.effects->spawn(wd, 31, e.abs, 1, 0x80303080u);
        wd.playSound(40);
        e.drawLayer = 0;
    } else if (e.youkaiState && !youkai) {
        if (wd.effects) wd.effects->spawn(wd, 30, e.abs, 1, 0x80803030u);
        wd.playSound(39);
        e.drawLayer = 2;
    }
    if (wasHuman && e.sparkle && hasTicked(e.timer) && e.timer.current % 2 == 0 && wd.effects) wd.effects->spawn(wd, 38, e.abs, 1, 0xFFFFFFFFu);
    e.youkaiState = youkai;
    e.extraMask = youkai ? 0x40 : 0x20;
}

// Dismissal (spec 9.6): the familiars die on their next update; with a bonus each one gives
// a cancel circle, time orbs (and its drops when no boss is up or a card is active).
void EnemyManager::dismissFamiliars(Enemy& e, bool bonus) {
    const World& wd = w();
    GameState& g = *wd.state;
    Player* player = wd.player;
    const bool bombing = player && player->bombActive();
    auto orb = [&](const Vec3f& at, float magnitude) {
        const float m = wd.rng->f32InRange(magnitude);  // magnitude first, then the angle
        const float a = (static_cast<float>(wd.rng->u32()) / 2147483648.0f - 1.0f) * kZunPi;
        Vec3f off;
        fromAngle(off, a, m);
        if (wd.bullets) wd.bullets->spawnItem(wd, at + off, BulletManager::TimeOrb, BulletManager::OrbRising);
    };
    if (e.parent == nullptr && e.next != nullptr) {
        const int count = familiarCountOf(e);
        int n = count;
        int k = 0;
        for (Enemy* f = e.next; f;) {
            Enemy* nxt = f->next;
            if (f->attached) f->offset = e.pos;
            f->prev = f->next = nullptr;
            f->parent = nullptr;
            f->dismissed = true;
            if (bonus) {
                int c;
                if (isYoukaiSolo(g.character)) c = n >= 10 ? 26 : 2 * n + 6;
                else if (isHumanSolo(g.character)) c = n >= 4 ? 40 : 6 * n + 16;
                else c = n >= 8 ? 26 : 2 * n + 10;
                if (bombing) n /= 3;  // after c: later familiars get fewer [QUIRK]
                f->abs = f->pos + f->offset;
                if (player) player->cancelCircle(f->abs, 32.0f, 2.0f, 8, e.boss ? 7 : 9);
                for (int i = 0; i < c; ++i) orb(f->abs, static_cast<float>(2 * c));
                bool anyBossSlot = false;
                for (Enemy* b : bosses_) anyBossSlot = anyBossSlot || b != nullptr;
                if (!anyBossSlot || wd.spellCardActive) {
                    f->itemDrop = 8;
                    drops(*f, false);
                }
                wd.playSound(2 + (k % 2));
            }
            f->itemDrop = -2;
            f->extraPoints = f->extraPower = 0;
            f = nxt;
            ++k;
        }
        e.next = nullptr;
        e.familiarCount = 0;
        if (bonus) {
            for (int i = 0; i < 2 * count; ++i) orb(e.abs, 128.0f);
            if (player) {
                player->cancelCircle(e.abs, 32.0f, 1.0f, 16, 7);
                player->orbTimer() = 0;
            }
        }
    }
    if (e.familiar && bonus) {
        g.gauge = static_cast<std::int16_t>(g.gauge - g.gauge / 12);
        if (player) {
            player->gaugeAfterFamiliar();
            player->orbTimer() = 50;
        }
        if (wd.bullets) wd.bullets->spawnItem(wd, e.abs, BulletManager::TimeOrb, BulletManager::Homing);
        e.itemDrop = -2;
        e.extraPoints = e.extraPower = 0;
    }
    if (e.parent) unlinkFromParent(e);
}

// ---- the manager's frame (spec 8.2) -----------------------------------------------------

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

void EnemyManager::update(const World& w) {
    w_ = &w;
    if (!loaded_) return;
    if (w.mode & ModeSpellFreeze) return;
    GameState& g = *w.state;
    // Rank (spec 8.2 step 4, corrected by spec_player 14.2): every 2400 - 240 x lives frames.
    const std::int32_t period = 2400 - 240 * g.livesI();
    if (!w.dialogue && hasTicked(managerTimer_) && period > 0 && managerTimer_.current % period == 0) {
        g.increaseSubrank(100);  // (0x43BFC3 is IncreaseSubrank: spec_player 0.8)
    }
    for (auto& l : drawLists_) l.clear();
    runTimelines();
    liveCount_ = 0;
    for (int i = 0; i < kEnemies; ++i) {
        Enemy& e = enemies_[static_cast<std::size_t>(i)];
        if (!e.active) {
            if (w.player && w.player->targetEnemySlot() == i) w.player->targetEnemySlot() = -1;
            continue;
        }
        ++liveCount_;
        updateEnemy(e);
    }
    spellCardUpdate();
    managerTimer_.tick();
}

void EnemyManager::updateEnemy(Enemy& e) {
    const World& wd = w();
    Player* player = wd.player;
    tookDamage_ = false;
    if (e.dismissed) {
        e.abs = e.pos + e.offset;
        e.abs.z = 0.0f;
        death(e);
        return;
    }
    const bool bombing = player && player->bombActive();
    if (e.frozenWhileBombing && (bombing || (player && player->state() != Player::State::Alive))) {
        e.timer.decrement(1);
    } else {
        if (e.familiar) familiarPreUpdate(e);
        for (int rerun = 0; rerun < 16; ++rerun) {
            if (!runVm(e)) {
                e.active = false;
                despawn(e);
                return;
            }
            clampToBounds(e);
            movementStep(e);
            clampToBounds(e);
            if (e.attached && e.parent) e.offset = e.parent->pos;
            e.abs = e.pos + e.offset;
            e.abs.z = 0.0f;
            // Trail history (TH07 §8.2 step 5): while the trail flags are set, over the count.
            if (e.trailFlags != 0) {
                const int count = std::clamp<int>(e.trailCount, 0, 96);
                for (int k = count - 1; k >= 1; --k) e.trail[static_cast<std::size_t>(k)] = e.trail[static_cast<std::size_t>(k - 1)];
                e.trail[0] = {e.abs, e.velocity, e.angle};
            }
            if (!e.vm.sprite) e.noCollision = true;
            const float sw = e.vm.spriteWidth, sh = e.vm.spriteHeight;
            if (!e.noCollision && !e.inBounds && onScreen(e.abs, sw, sh)) {
                e.inBounds = true;
            } else if (e.inBounds && !e.oobDisabled && !onScreen(e.abs, sw, sh)) {
                // With a trail its oldest node must be off screen too.
                const int last = std::clamp<int>(e.trailCount, 0, 96) - 1;
                if (e.trailFlags == 0 || last < 0 || !onScreen(e.trail[static_cast<std::size_t>(last)].pos, sw, sh)) {
                    e.active = false;
                    despawn(e);
                    return;
                }
            }
            if (lifeCallback(e)) continue;
            if (e.timerThreshold >= 0 && timerCallback(e)) continue;
            break;
        }
        // ANM.
        e.vm.color = e.color;
        if (wd.rng) e.vm.tick(*wd.rng);
        e.color = e.vm.color;
        for (int k = 0; k < 2; ++k) {
            if (e.vm2File[static_cast<std::size_t>(k)] >= 0 && wd.rng && e.vms2[static_cast<std::size_t>(k)].tick(*wd.rng)) {
                e.vm2File[static_cast<std::size_t>(k)] = -1;
            }
        }
        // Collision.
        if (!e.noCollision && !e.youkaiState && !(e.intangibleWhileBombing && bombing)) {
            if (e.contact) {
                contactTest(e, e.abs, e.hitbox);
                // Along the trail, every 6th node up to the interval, shrinking with flag 2.
                if (e.trailFlags != 0) {
                    for (int j = 1; j < e.trailInterval && j < 96; j += 6) {
                        Vec3f box = e.hitbox;
                        if (e.trailFlags & 2) box = box - divVec(box * static_cast<float>(j), static_cast<float>(e.trailInterval));
                        contactTest(e, e.trail[static_cast<std::size_t>(j)].pos, box);
                    }
                }
            }
            e.damageThisFrame = 0;
            if (e.hittable) shotDamage(e);
        }
        if (e.deathLatch && e.life > 0) e.deathLatch = false;
        if (e.life <= 0 && !e.deathLatch && !e.cannotDie) {
            death(e);
            if (!e.active) return;
        }
    }
    // Damage tint (visual), boss bar.
    if (e.boss && e.bossId == 0 && !wd.dialogue && e.maxLife > 0) bossBar_ = static_cast<float>(e.life) / static_cast<float>(e.maxLife);
    updateCircles(e);
    if (!wd.timeStop) e.timer.tick();
    if (e.invincible.current > 0) e.invincible.decrement(1);
    if (!e.noCollision && e.active) drawLists_[std::min<std::size_t>(e.drawLayer, 3)].insert(drawLists_[std::min<std::size_t>(e.drawLayer, 3)].begin(), &e);
}

// The contact test (spec 8.3, TH07 §8.5): projectiles graze every 6th ticked frame with
// their box / 0.7; the killbox is the box / 1.5 (reciprocal multiplies); a hit costs a
// killable non-boss, non-projectile enemy 10 life (also while the player is invulnerable).
void EnemyManager::contactTest(Enemy& e, const Vec3f& centre, const Vec3f& box) {
    const World& wd = w();
    Player* player = wd.player;
    if (!player) return;
    if (e.projectile && hasTicked(e.timer) && e.timer.current % 6 == 0) player->bulletGraze(wd, centre, divVec(box, 0.7f), false);
    if (player->enemyContact(wd, centre, divVec(box, 1.5f)) == Player::HitResult::Hit) {
        if (e.damageable && !e.boss && !e.projectile && !e.cannotDie) e.life -= 10;
    }
}

void EnemyManager::shotDamage(Enemy& e) {
    const World& wd = w();
    Player* player = wd.player;
    if (!player) return;
    GameState& g = *wd.state;
    bool bomb = false;
    int d = 0;
    if (!(wd.spellCardActive && e.parent && player->bombActive())) d = player->damageQuery(wd, e.abs, e.hitbox, e.damageWord, bomb);
    if (e.box2.x > 0.0f) {
        bool bomb2 = false;
        const int d2 = player->damageQuery(wd, e.abs, e.box2, e.damageWord, bomb2);
        if (!bomb) {
            const float k = g.character == 3 || g.character == 11 ? 6.5f : 1.7f;
            d = x86ToInt(static_cast<float>(d) + static_cast<float>(d2) / k);
        }
    }
    // Homing bookkeeping (spec 8.5), every frame the enemy is hittable.
    {
        Vec3f& h = player->homingTarget();
        bool& lock = player->targetLock();
        const float px = player->position().x;
        if (e.boss) {
            if (!lock || std::fabs(e.abs.x - px) < std::fabs(h.x - px)) h = e.abs;
            lock = true;
        }
        if (!lock && h.y < e.abs.y) h = e.abs;
        int& target = player->targetEnemySlot();
        if (std::fabs(e.abs.x - px) < 64.0f && !e.familiar && (target < 0 || enemies_[static_cast<std::size_t>(target)].pos.y > e.abs.y)) {
            target = e.slot;
        }
    }
    if (d <= 0) return;
    tookDamage_ = true;
    d = std::min(d, 70);
    g.score += static_cast<std::uint32_t>(d / 5);  // d / 5 x 10 points
    if (e.damageable) {
        if (wd.spellCardActive) {
            if (!bomb) d = d > 7 ? d / 7 : 1;
            else d = spell_.bombed && !e.familiar ? (d > 2 ? x86ToInt(static_cast<float>(d) / 2.5f) : 1) : 0;
        }
    } else {
        d = 0;
    }
    if (e.invincible.current > 0) d = e.boss ? d / 9 : 0;
    e.life -= d;
    e.damageThisFrame = d;
    if (e.familiar && e.parent && !player->bombActive()) {
        Enemy& par = *e.parent;
        int h = d / 2;
        if (par.invincible.current > 0) h = par.boss ? h / 9 : 0;
        std::int32_t floor = 0;
        for (std::int32_t th : par.lifeThresholds) floor = std::max(floor, th);
        par.life -= h;
        if (par.life <= floor) par.life = floor;
    }
}

void EnemyManager::resetForCallback(Enemy& e) {
    freeThreads(e);
    e.rankSpeedLow = -0.5f;
    e.rankSpeedHigh = 0.5f;
    e.rankAmounts.fill(0);
    e.depth = 0;
    e.lowLife = 0;
    e.shooter = template_->shooter;
    e.shootInterval = 0;
}

bool EnemyManager::lifeCallback(Enemy& e) {
    for (int i = 0; i < 4; ++i) {
        const std::int32_t th = e.lifeThresholds[static_cast<std::size_t>(i)];
        if (th < 0 || e.life >= th) continue;
        e.life = e.phaseLife = th;
        startSub(e.ctx, e.lifeSubs[static_cast<std::size_t>(i)]);
        e.lifeThresholds[static_cast<std::size_t>(i)] = -1;
        e.timerThreshold = -1;
        resetForCallback(e);
        dismissFamiliars(e, true);
        for (Enemy& o : enemies_) {
            if (!o.active || o.boss) continue;
            if (&o == &e && e.boss) continue;
            o.life = 0;
            if (o.deathSub >= 0) {
                startSub(o.ctx, o.deathSub);
                o.deathSub = -1;
            }
        }
        if (e.boss && w().player) w().player->makeInvulnerable(70);
        return true;
    }
    return false;
}

bool EnemyManager::timerCallback(Enemy& e) {
    if (e.timer.current < e.timerThreshold) return false;
    std::int32_t best = -1;
    int bestIndex = -1;
    for (int i = 0; i < 4; ++i) {
        if (e.lifeThresholds[static_cast<std::size_t>(i)] > best) best = e.lifeThresholds[static_cast<std::size_t>(i)], bestIndex = i;
    }
    if (bestIndex >= 0 && best > 0) {
        e.life = e.phaseLife = best;
        e.lifeThresholds[static_cast<std::size_t>(bestIndex)] = -1;
    }
    startSub(e.ctx, e.timerSub);
    e.timerThreshold = -1;
    e.timerSub = e.deathSub;
    e.timer.assign(0);
    if (!e.survival) {
        if (spell_.active) {
            spell_.capturable = false;
            spell_.timedOut = true;
            spell_.captureValue = 0;
        }
        if (w().bullets) w().bullets->clearAll(w(), 4);
    }
    if (e.boss && w().player) w().player->makeInvulnerable(70);
    dismissFamiliars(e, false);
    for (Enemy& o : enemies_) {
        if (!o.active || o.boss) continue;
        o.life = 0;
        if (o.deathSub >= 0) {
            startSub(o.ctx, o.deathSub);
            o.deathSub = -1;
        }
    }
    resetForCallback(e);
    return true;
}

bool EnemyManager::anyBoss() const {
    for (Enemy* b : bosses_) {
        if (b) return true;
    }
    return false;
}

void EnemyManager::playerMissed() {
    spell_.capturable = false;
    spell_.captureValue = 0;
}

void EnemyManager::bombUsed() {
    spell_.capturable = false;
    spell_.bombed = spell_.active;
}

void EnemyManager::dissolveStart() {
    for (Enemy* b : bosses_) {
        if (b) b->hittable = false;
    }
}

void EnemyManager::dissolveEnd() {
    for (Enemy* b : bosses_) {
        if (!b) continue;
        dismissFamiliars(*b, false);
        b->life = 0;
        b->frozenWhileBombing = false;
    }
}

// Spell circles (spec_effects 5.2): the enemy drives its circles' centre, radius and angle,
// and a release fades them out over 16 effect passes.
void EnemyManager::releaseCircles(Enemy& e) {
    for (int k = 0; k < e.circleCount; ++k) {
        if (Effect* c = e.circles[static_cast<std::size_t>(k)]) c->fade = 1;
        e.circles[static_cast<std::size_t>(k)] = nullptr;
    }
    e.circleCount = 0;
}

void EnemyManager::updateCircles(Enemy& e) {
    for (int k = 0; k < e.circleCount; ++k) {
        Effect* c = e.circles[static_cast<std::size_t>(k)];
        if (!c) continue;
        c->vm.set(AnmVm::Active, !e.noCollision);
        c->centre = e.pos;
        c->radius = c->radius < e.circleRadius ? c->radius + 0.3f : e.circleRadius;
        c->angle = addNormalizeAngle(c->angle, kZunPi / 100.0f);
    }
}

void EnemyManager::despawn(Enemy& e) {
    dismissFamiliars(e, false);
    if (e.deathType == 0) e.active = false;
    if (e.boss && e.bossId < 4) {
        bossPresent_ = false;
        bosses_[e.bossId] = nullptr;
        e.boss = false;
    }
    releaseCircles(e);
    e.lifeThresholds.fill(-1);
    e.timerThreshold = -1;
    freeThreads(e);
    if (w().player && w().player->targetEnemySlot() == e.slot) w().player->targetEnemySlot() = -1;
}

void EnemyManager::drops(Enemy& e, bool autoCollect) {
    const World& wd = w();
    GameState& g = *wd.state;
    BulletManager* bm = wd.bullets;
    if (!bm) return;
    const int state = autoCollect ? BulletManager::Homing : BulletManager::Falling;
    if (e.itemDrop >= 0) {
        if (wd.effects) wd.effects->spawn(wd, e.death2 + 4, e.abs, 3, 0xFFFFFFFFu);
        bm->spawnItem(wd, e.abs, e.itemDrop, state);
    } else if (e.itemDrop == -1) {
        if (dropCounter_ % 3 == 0) {
            if (wd.effects) wd.effects->spawn(wd, e.death2 + 4, e.abs, 6, 0xFFFFFFFFu);
            const int type = wd.tables ? wd.tables->dropCycle[dropIndex_ % 32] : 0;
            bm->spawnItem(wd, e.abs, type, state);
            dropIndex_ = static_cast<std::uint16_t>((dropIndex_ + 1) % 32);
        }
        ++dropCounter_;
    }
    for (int k = 0; k < e.extraPower; ++k) {
        Vec3f at = e.abs;
        at.x += wd.rng->f32InRange(128.0f) - 64.0f;
        at.y += wd.rng->f32InRange(128.0f) - 64.0f;
        bm->spawnItem(wd, at, g.power < 128.0f ? BulletManager::SmallPower : BulletManager::Point, state);
    }
    for (int k = 0; k < e.extraPoints; ++k) {
        Vec3f at = e.abs;
        at.x += wd.rng->f32InRange(128.0f) - 64.0f;
        at.y += wd.rng->f32InRange(128.0f) - 64.0f;
        bm->spawnItem(wd, at, BulletManager::Point, state);
    }
    e.extraPower = e.extraPoints = 0;
}

void EnemyManager::death(Enemy& e) {
    const World& wd = w();
    GameState& g = *wd.state;
    e.deathLatch = true;
    e.timerThreshold = -1;
    e.lifeThresholds.fill(-1);
    freeThreads(e);
    if (e.familiar && e.parent) --e.parent->familiarCount;
    dismissFamiliars(e, true);
    g.addGauge(wd.player && wd.player->focused() ? 200 : -200, false, wd.player && wd.player->bombActive());
    const bool wasDismissed = e.dismissed;
    const bool bombing = wd.player && wd.player->bombActive();
    switch (e.deathType) {
        case 3:
            e.life = 1;
            e.damageable = false;
            e.deathType = 0;
            bossPresent_ = false;
            if (e.death1 >= 0 && wd.effects) {
                for (int k = 0; k < 3; ++k) wd.effects->spawn(wd, e.death1, e.abs, 1, 0xFFFFFFFFu);
            }
            if (wd.player) wd.player->makeInvulnerable(90);
            e.frozenWhileBombing = e.intangibleWhileBombing = false;
            break;
        case 0:
        case 1:
        case 2:
            if (e.deathType != 2) g.score += static_cast<std::uint32_t>(e.score) / 10;
            if (e.deathType == 0) e.active = false;
            if (e.deathType == 1) e.contact = e.damageable = e.hittable = false;
            if (e.deathType != 2 && e.boss) {  // boss clean-up
                bossPresent_ = false;
                releaseCircles(e);
            }
            drops(e, bombing);
            if (e.boss && !wd.spellCardActive && wd.bullets) {
                std::uint32_t s = wd.bullets->bonusClear(wd, 8000);
                s = killAllScoring(8000, s);
                g.score += s / 10;
            }
            e.life = 0;
            break;
        default: break;
    }
    if (!wasDismissed) {
        wd.playSound(2 + (e.slot % 2));
        if (e.death1 >= 0 && wd.effects) {
            wd.effects->spawn(wd, e.death1, e.abs, 1, 0xFFFFFFFFu);
            wd.effects->spawn(wd, e.death2 + 4, e.abs, 4, 0xFFFFFFFFu);
        }
        if ((g.h80() || g.y80()) && wd.bullets) wd.bullets->spawnItem(wd, e.abs, BulletManager::TimeOrb, BulletManager::Homing);
    }
    if (e.deathSub >= 0) {
        e.rankSpeedLow = -0.5f;
        e.rankSpeedHigh = 0.5f;
        e.rankAmounts.fill(0);
        e.depth = 0;
        e.shooter = template_->shooter;
        e.shootInterval = 0;
        startSub(e.ctx, e.deathSub);
        e.deathSub = -1;
        e.active = true;
    }
}

void EnemyManager::killAllSimple(Enemy* except) {
    for (Enemy& e : enemies_) {
        if (!e.active || e.boss || &e == except || e.cannotDie) continue;
        e.life = 0;
        if (e.familiar) unlinkFromParent(e);
    }
}

std::uint32_t EnemyManager::killAllScoring(std::uint32_t max, std::uint32_t start) {
    std::uint32_t b = 2000, total = start;
    for (Enemy& e : enemies_) {
        if (!e.active || e.boss || e.cannotDie) continue;
        e.life = 0;
        total += b;
        b = std::min(b + 30, max);
        if (e.familiar) unlinkFromParent(e);
    }
    return total;
}

// ---- spell cards (spec 12) --------------------------------------------------------------

void EnemyManager::spellCardStart(Enemy& e, const std::uint8_t* ins) {
    const World& wd = w();
    spell_ = SpellCard{};
    spell_.active = true;
    spell_.capturable = true;
    spell_.id = eclU16(ins + 14);
    spell_.boss = &e;
    spell_.captureValue = spell_.displayed = static_cast<std::uint32_t>(eclI32(ins + 16));
    if (e.survival) spell_.displayed = 99999990u;
    const std::int32_t threshold = std::max(1, e.timerThreshold);
    const std::uint32_t perSecond = static_cast<std::uint32_t>(threshold) / 60u;
    spell_.drain = perSecond ? (spell_.captureValue - spell_.captureValue / 7) / perSecond : 0;
    spell_.timer.assign(threshold);
    spell_.duration = threshold;
    std::string name;
    for (int i = 0; i < 48; ++i) {
        const auto c = static_cast<char>(ins[20 + i] ^ 0xAA);
        if (c == 0) break;
        name.push_back(c);
    }
    spell_.name = name;
    if (wd.bullets) wd.bullets->clearAll(wd, 1);
    if (wd.background && wd.anm && wd.rng) wd.background->spellStart(*wd.anm, *wd.rng);
    e.rankSpeedLow = -0.5f;
    e.rankSpeedHigh = 0.5f;
    e.rankAmounts.fill(0);
    log::info("th08: spell card {}", spell_.id);
}

void EnemyManager::spellCardEnd() {
    const World& wd = w();
    if (!spell_.active) return;
    spell_.active = false;
    if (!spell_.timedOut && wd.bullets) {
        std::uint32_t s = wd.bullets->bonusClear(wd, 8000);
        s = killAllScoring(8000, s);
        wd.state->score += s / 10;
    }
    if (spell_.capturable) wd.state->score += spell_.captureValue / 10;
    if (wd.player) wd.player->makeInvulnerable(70);
    wd.playSound(15);
    if (wd.background) wd.background->spellEnd();
    if (spell_.boss) spell_.boss->survival = false;
    spell_.boss = nullptr;
}

void EnemyManager::spellCardUpdate() {
    const World& wd = w();
    if (wd.timeStop) return;
    if (spell_.active) {
        if (!spell_.boss || !spell_.boss->active) {
            spell_.active = false;
        } else if (spell_.capturable && !spell_.noDrain && !spell_.boss->survival) {
            const auto step = static_cast<std::uint32_t>(x86ToInt(static_cast<float>(spell_.drain / 60u) * g_timing.speed));
            spell_.captureValue = spell_.captureValue > step ? spell_.captureValue - step : 0;
            spell_.captureValue -= spell_.captureValue % 10;
        }
    }
    spell_.timer.decrement(1);
}

// ---- ex routines (spec 11, the ones without stage visuals) -------------------------------

void EnemyManager::exRoutine(Enemy& e, int routine, const std::uint8_t* ins) {
    const World& wd = w();
    const std::int32_t a = eclI32(ins + 16);
    EclContext& c = cur_ ? *cur_ : e.ctx;
    switch (routine) {
        case 2: {
            bool changed = false;
            if (e.pos.x <= 0.0f || e.pos.x >= 384.0f) e.velocity.x = -e.velocity.x, changed = true;
            if (e.velocity.y < c.args.floatA[7]) e.velocity.y += c.args.floatA[6], changed = true;
            if (e.pos.y < -64.0f) e.velocity.y = -e.velocity.y, changed = true;
            else if (e.pos.y >= 480.0f) e.oobDisabled = false;
            if (changed) e.angle = zatan2(e.velocity.y, e.velocity.x);
            break;
        }
        case 18:
            if (a != 0) g_timing.speed = 1.0f / static_cast<float>(a);
            break;
        case 19: c.args.intA[0] = 0; break;  // the practice card (spell practice not ported)
        case 24: c.args.intA[0] = 0; break;  // spell cards captured this game
        case 26:
            wd.timeStop = a != 0;
            break;
        case 28:
            if (a != 0) g_timing.speed = 1.0f / static_cast<float>(a);
            break;
        case 29:
            g_timing.speed = 1.0f;
            break;
        case 31:
            if (wd.bullets) wd.bullets->spawnItem(wd, e.pos, wd.player && wd.player->bombActive() ? 3 : 5, BulletManager::Falling);
            break;
        // Screen effects (spec_bombs_familiars 1.4): the flashes are visual, the shakes draw
        // two u32 per frame from the next frame on.
        case 1:
            if (wd.screen) wd.screen->flash(60, 1, 0xFFFFFFFFu);
            break;
        case 10:
            if (wd.screen) {
                wd.screen->flash(30, 5, 0x41000000u);  // (the colour argument is the float 8.0's bits)
                wd.screen->rampedShake(4.0f, 120, 190, 60);
            }
            break;
        case 15:
            if (wd.screen) wd.screen->rampedShake(16.0f, 20, 20, 20);
            break;
        case 17:
            if (wd.screen) wd.screen->flash(180, 1, 0xFFFFFFFFu);
            break;
        case 0: case 3: case 5: case 6: case 13: case 20: case 22: case 23: case 30:
            break;  // visual or GUI only
        default: eclWarnOnce(std::format("ex routine {} is not ported yet", routine)); break;
    }
}

// ---- drawing ----------------------------------------------------------------------------

namespace {

// Mid(a, b) of the trail spec 3.3, with the original's wrong far-side branch [QUIRK].
float trailMid(float a, float b) {
    const float lo = std::min(a, b), hi = std::max(a, b);
    const float d = hi - lo, w = (lo + kZunTwoPi) - hi;
    return d < w ? d * 0.5f + lo : w * 0.5f + lo;
}

}  // namespace

// The enemy draw (visual): the primary VM and the two secondary ones at the enemy plus their
// offsets; a youkai-state familiar dark at half alpha (spec 8.2 step 16); then the trail.
void EnemyManager::drawEnemy(SpriteSink& sink, Enemy& e, const AnmDrawState& state) {
    const float x = e.abs.x + kPlayfieldX, y = e.abs.y + kPlayfieldY;
    const std::uint32_t altColor = e.vm.altColor;
    const bool useAlt = e.vm.has(AnmVm::UseAltColor);
    if (e.youkaiState) {
        e.vm.altColor = (static_cast<std::uint32_t>((e.vm.color >> 24) / 2) << 24) | 0x002020C0u;
        e.vm.set(AnmVm::UseAltColor, true);
    }
    e.vm.position = {x + e.vm.offset.x, y + e.vm.offset.y, 0.0f};
    if (!(e.trailFlags & 16)) drawVm(sink, e.vm, state);
    e.vm.altColor = altColor;
    e.vm.set(AnmVm::UseAltColor, useAlt);
    for (int k = 0; k < 2; ++k) {
        if (e.vm2File[static_cast<std::size_t>(k)] < 0) continue;
        AnmVm& v = e.vms2[static_cast<std::size_t>(k)];
        v.position = {x + v.offset.x, y + v.offset.y, 0.0f};
        drawVm(sink, v, state);
    }
    if (e.trailFlags != 0) {
        if (e.trailFlags & 8) drawTrailStrip(sink, e);
        else drawTrailCopies(sink, e, state);
    }
}

// The sprite-copy trail (flag 8 clear; TH07 spec_enemy_trails 4): copies of the primary at
// nodes S, 2S, ... (unfilled nodes skipped), shrinking with flag 2 and fading with flag 4.
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
        if (e.trailFlags & 2) vm.scale.x = scale0.x - (static_cast<float>(j) * scale0.x) / static_cast<float>(count);
        if (e.trailFlags & 4) vm.color = (color0 & 0x00FFFFFFu) | (static_cast<std::uint32_t>(a - (a * j) / count) << 24);
        vm.position = {n.pos.x + vm.offset.x + kPlayfieldX, n.pos.y + vm.offset.y + kPlayfieldY, 0.0f};
        drawVm(sink, vm, state);
    }
    e.vm.scale = scale0;
    e.vm.color = color0;
}

// The strip trail (flag 8; TH07 spec_enemy_trails 3): two vertices per node across the
// recorded angle, the sprite stretched along the whole length, drawn as consecutive quads.
void EnemyManager::drawTrailStrip(SpriteSink& sink, Enemy& e) {
    const AnmVm& vm = e.vm;
    if (!vm.sprite || !vm.has(AnmVm::Visible) || !vm.has(AnmVm::Active) || (vm.color >> 24) == 0) return;
    const int count = std::clamp<int>(e.trailCount, 0, 96);
    const int step = std::max<int>(1, e.nodeStep);
    int nodes = 0;
    for (int j = 0; j < count && e.trail[static_cast<std::size_t>(j)].pos.x >= -990.0f; j += step) ++nodes;
    if (nodes < 2) return;
    const AnmSprite& sp = *vm.sprite;
    const float uStep = (sp.uvEndX - sp.uvStartX) / static_cast<float>(nodes - 1);
    float u = sp.uvEndX + vm.uvScroll.x;
    const float vA = sp.uvStartY + vm.uvScroll.y, vB = sp.uvEndY + vm.uvScroll.y;
    const float h0 = vm.scale.y * vm.spriteHeight / 2.0f;
    const std::uint32_t color0 = vm.color;
    const auto a = static_cast<std::int32_t>(color0 >> 24);
    auto toNdc = [](float x, float y, Vertex3D& v) {
        v.x = (x + 0.5f) / 640.0f * 2.0f - 1.0f;
        v.y = 1.0f - (y + 0.5f) / 480.0f * 2.0f;
        v.z = -1.0f;
        v.w = 1.0f;
    };
    Draw3DState ds;
    ds.depthWrite = false;
    ds.clipX = kPlayfieldX, ds.clipY = kPlayfieldY, ds.clipW = kPlayfieldW, ds.clipH = kPlayfieldH;
    Vertex3D prev[2]{};
    bool havePrev = false;
    float prevTheta = 0.0f;
    for (int k = 0, j = 0; k < nodes; ++k, j += step, u -= uStep) {
        const TrailNode& n = e.trail[static_cast<std::size_t>(j)];
        const float theta = j == 0 ? n.angle : trailMid(e.trail[static_cast<std::size_t>(j - 1)].angle, n.angle);
        float h = h0;
        if (e.trailFlags & 2) {
            h *= 1.0f - static_cast<float>(j) / static_cast<float>(count);
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
        toNdc(n.pos.x + (-s * h + kPlayfieldX), n.pos.y + (c * h + kPlayfieldY), cur[0]);
        toNdc(n.pos.x + (s * h + kPlayfieldX), n.pos.y + (-c * h + kPlayfieldY), cur[1]);
        cur[0].u = cur[1].u = u;
        cur[0].v = vA;
        cur[1].v = vB;
        cur[0].color = cur[1].color = argbToRgbaBytes(color);
        if (havePrev) {
            const Vertex3D quad[4] = {prev[0], prev[1], cur[0], cur[1]};
            sink.drawQuad3D(sp.texture, drawBlend(vm), quad, ds);
        }
        prev[0] = cur[0];
        prev[1] = cur[1];
        havePrev = true;
        prevTheta = theta;
    }
}

void EnemyManager::drawLayers(SpriteSink& sink, const AnmDrawState& state, int first, int last) {
    for (int l = first; l <= last && l < 4; ++l) {
        for (Enemy* e : drawLists_[static_cast<std::size_t>(l)]) drawEnemy(sink, *e, state);
    }
}

}  // namespace touhou::th08
