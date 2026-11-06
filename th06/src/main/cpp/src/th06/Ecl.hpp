#pragma once

// ECL ("enemy control language") files of the 2002 game: data/ecldata1.ecl .. 7.
//
// There is no magic number. Layout (little-endian):
//
//   EclRawHeader     i16 subCount, i16 mainCount, u32 timelineOffsets[3],
//                    u32 subOffsets[subCount]                  (offsets from file start)
//   timeline         TimelineInstr records, each `size` bytes, until time < 0
//                    (only timeline 0 is used by the game)
//   subs             EclRawInstr records chained by offsetToNext
//
// A stage's timeline spawns enemies at fixed frames; each enemy then runs a sub as its
// own little program (see EnemyManager). Field meanings follow the th06 decompilation.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace touhou::th06 {

#pragma pack(push, 1)
struct EclRawHeader {
    std::int16_t subCount;
    std::int16_t mainCount;
    std::uint32_t timelineOffsets[3];
    // followed by std::uint32_t subOffsets[subCount]
};
static_assert(sizeof(EclRawHeader) == 0x10);

struct TimelineInstrArgs {
    std::uint32_t uintVar1;  // floats x, y, z for the spawn opcodes
    std::uint32_t uintVar2;
    std::uint32_t uintVar3;
    std::uint16_t ushortVar1;  // life
    std::uint16_t ushortVar2;  // item drop
    std::uint32_t uintVar4;    // score
};
static_assert(sizeof(TimelineInstrArgs) == 0x14);

struct TimelineInstr {
    std::int16_t time;
    std::int16_t arg0;  // sub id for the spawn opcodes
    std::int16_t opCode;
    std::int16_t size;  // total record size in bytes
    // followed by TimelineInstrArgs (present when size >= 8 + 20)
};
static_assert(sizeof(TimelineInstr) == 8);

struct EclRawInstr {
    std::int32_t time;
    std::int16_t opCode;
    std::int16_t offsetToNext;
    std::uint8_t unknown0;
    std::uint8_t difficultyMask;  // bit d set = runs on difficulty d (0 Easy .. 4 Extra)
    std::uint16_t unknown1;
    // followed by arguments
};
static_assert(sizeof(EclRawInstr) == 12);

struct EclRawInstrBulletArgs {
    std::int16_t sprite;
    std::int16_t color;
    std::int32_t count1;  // value or variable id
    std::int32_t count2;
    float speed1;
    float speed2;
    float angle1;
    float angle2;
    std::int32_t flags;
};
static_assert(sizeof(EclRawInstrBulletArgs) == 0x20);

struct EclRawInstrLaserArgs {
    std::int16_t sprite;
    std::int16_t color;
    float angle;
    float speed;
    float startOffset;
    float endOffset;
    float startLength;
    float width;
    std::int32_t startTime;
    std::int32_t duration;
    std::int32_t despawnDuration;
    std::int32_t hitboxStartTime;
    std::int32_t hitboxEndDelay;
    std::int32_t flags;
};
static_assert(sizeof(EclRawInstrLaserArgs) == 0x34);

struct EclRawInstrEnemyCreateArgs {
    std::int32_t subId;
    float x, y, z;
    std::int16_t life;
    std::int16_t itemDrop;
    std::int32_t score;
};
static_assert(sizeof(EclRawInstrEnemyCreateArgs) == 0x18);
#pragma pack(pop)

namespace ecl_op {
enum : std::int16_t {
    Nop = 0, EnemyDelete, Jump, Loop, SetInt, SetFloat, SetIntRand, SetIntRandMin, SetFloatRand,
    SetFloatRandMin, SetVarSelfX, SetVarSelfY, SetVarSelfZ, MathIntAdd, MathIntSub, MathIntMul,
    MathIntDiv, MathIntMod, MathInc, MathDec, MathFloatAdd, MathFloatSub, MathFloatMul, MathFloatDiv,
    MathFloatMod, MathLineAngle, MathReduceAngle, CmpInt, CmpFloat, JumpLss, JumpLeq, JumpEqu, JumpGre,
    JumpGeq, JumpNeq, Call, Ret, CallLss, CallLeq, CallEqu, CallGre, CallGeq, CallNeq, MovePosition,
    MoveAxisSpeed, MoveVelocity, MoveAngularVelocity, MoveSpeed, MoveAcceleration, MoveRand,
    MoveRandInBounds, MoveTowardsPlayer, MoveVelocityInterpDecelerate, MoveVelocityInterpDecelerateFast,
    MoveVelocityInterpAccelerate, MoveVelocityInterpAccelerateFast, MovePositionInterpLinear,
    MovePositionInterpDecelerate, MovePositionInterpDecelerateFast, MovePositionInterpAccelerate,
    MovePositionInterpAccelerateFast, MoveAsInterpDecelerate, MoveAsInterpDecelerateFast,
    MoveAsInterpAccelerate, MoveAsInterpAccelerateFast, MoveBoundsSet, MoveBoundsDisable, BulletFanAimed,
    BulletFan, BulletCircleAimed, BulletCircle, BulletOffsetCircleAimed, BulletOffsetCircle,
    BulletRandomAngle, BulletRandomSpeed, BulletRandom, ShootInterval, ShootIntervalDelayed, ShootDisable,
    ShootEnable, ShootNow, ShootOffset, BulletEffects, BulletCancel, BulletSound, LaserCreate,
    LaserCreateAimed, LaserIndex, LaserRotate, LaserRotateFromPlayer, LaserOffset, LaserTest, LaserCancel,
    SpellcardStart, SpellcardEnd, EnemyCreate, EnemyKillAll, AnmSetMain, AnmSetPoses, AnmSetSlot,
    AnmDeathEffects, BossSet, SpellcardEffect, EnemySetHitbox, EnemyFlagCollision, EnemyFlagCanTakeDamage,
    EffectSound, EnemyFlagsDeath, DeathCallbackSub, EnemyInterruptSet, EnemyInterrupt, EnemyLifeSet,
    PhaseTimerSet, LifeCallbackThreshold, LifeCallbackSub, TimerCallbackThreshold, TimerCallbackSub,
    EnemyFlagInteractable, EffectParticle, DropItems, AnmFlagRotation, ExInsCall, ExInsRepeat, EclTimeAdd,
    DropItemId, StdUnpause, BossSetLifeCount, DebugWatch, AnmInterruptMain, AnmInterruptSlot,
    EnemyFlagDisableCallstack, BulletRankInfluence, EnemyFlagInvisible, PhaseAdvance, LaserClearAll,
    SpellcardFlagTimeout,
};
}  // namespace ecl_op

namespace timeline_op {
enum : std::int16_t {
    EnemyCreate = 0, DummyCreate, EnemyCreateMirror, DummyCreateMirror, EnemyCreateRandom,
    DummyCreateRandom, EnemyCreateMirrorRandom, DummyCreateMirrorRandom, MsgRead, MsgWait, BossInterrupt,
    PlayerPower, BossWait,
};
}  // namespace timeline_op

// A loaded, bounds-checked ECL file. Instruction pointers are offsets into bytes().
class EclFile {
  public:
    bool load(std::vector<std::uint8_t> bytes, std::string* error);

    std::span<const std::uint8_t> bytes() const { return bytes_; }
    // Puts back bytes saved earlier (scripts write into themselves); same size only, in place,
    // so pointers into the script stay valid.
    bool restoreBytes(std::span<const std::uint8_t> saved) {
        if (saved.size() != bytes_.size()) return false;
        std::copy(saved.begin(), saved.end(), bytes_.begin());
        return true;
    }
    int subCount() const { return static_cast<int>(subOffsets_.size()); }
    // Offset of a sub's first instruction, or 0 if the id is invalid.
    std::uint32_t subOffset(int subId) const;
    std::uint32_t timelineOffset() const { return timelineOffset_; }
    // Where the usable timeline ends (its terminator, or where a damaged chain stops).
    std::uint32_t timelineEnd() const { return timelineEnd_; }
    int timelineRecords() const { return timelineRecords_; }

    // Bounds-checked reads; return false when the record would run past the file.
    bool instrAt(std::uint32_t offset, EclRawInstr& out) const;
    bool timelineAt(std::uint32_t offset, TimelineInstr& out, TimelineInstrArgs& args) const;

    template <class T>
    T argAt(std::uint32_t instrOffset, std::size_t byteOffset) const {
        T v{};
        const std::size_t at = static_cast<std::size_t>(instrOffset) + sizeof(EclRawInstr) + byteOffset;
        if (at + sizeof(T) <= bytes_.size()) std::memcpy(&v, bytes_.data() + at, sizeof(T));
        return v;
    }

    // MATH_INC / MATH_DEC on a literal argument modify the loaded script in place in the
    // original executable; this reproduces that.
    template <class T>
    void writeArg(std::uint32_t instrOffset, std::size_t byteOffset, T value) {
        const std::size_t at = static_cast<std::size_t>(instrOffset) + sizeof(EclRawInstr) + byteOffset;
        if (at + sizeof(T) <= bytes_.size()) std::memcpy(bytes_.data() + at, &value, sizeof(T));
    }

  private:
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint32_t> subOffsets_;
    std::uint32_t timelineOffset_ = 0;
    std::uint32_t timelineEnd_ = 0;
    int timelineRecords_ = 0;
};

}  // namespace touhou::th06

