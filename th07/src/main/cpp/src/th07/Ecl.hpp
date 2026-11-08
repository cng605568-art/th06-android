#pragma once

// Perfect Cherry Blossom's ECL scripts (data/ecldataN.ecl): the file (subs, up to 16 stage
// timelines), the argument encoding (a per-instruction variable mask; float-side variable
// ids are written as floats), the call context the VM keeps per enemy, and the numeric
// helpers that reproduce the original's x87 results. The VM itself and the instruction set
// run inside the enemy manager (Enemies.hpp), which owns the enemies the scripts drive.
//
// Scripts write into their own argument words (a write to anything that is not a writable
// variable lands in the loaded script), so the file is a private mutable copy per stage.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "th07/Anm.hpp"

namespace touhou::th07 {

// ---- x87-faithful arithmetic (spec 0.8) -----------------------------------------------------

// Float to i32 by truncation; NaN and out-of-range values give the x86 "integer indefinite"
// value 0x80000000 (ARM would saturate).
inline std::int32_t x86ToInt(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return static_cast<std::int32_t>(0x80000000u);
    return static_cast<std::int32_t>(f);
}
// Float to u32: through a 64-bit integer, keeping the low 32 bits (0 when out of i64 range).
inline std::uint32_t x86ToU32(float f) {
    if (!(f >= -9223372036854775808.0f && f < 9223372036854775808.0f)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(f)));
}
// Transcendentals: the x87 instructions ignore the 24-bit precision control; their result is
// rounded once, when stored.
inline float zsin(float a) { return static_cast<float>(std::sin(static_cast<double>(a))); }
inline float zcos(float a) { return static_cast<float>(std::cos(static_cast<double>(a))); }
inline float zatan2(float y, float x) { return static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x))); }
// The combined sine/cosine times a magnitude, one rounding per product (spec 0.9).
inline float cosTimes(float a, float m) { return static_cast<float>(std::cos(static_cast<double>(a)) * static_cast<double>(m)); }
inline float sinTimes(float a, float m) { return static_cast<float>(std::sin(static_cast<double>(a)) * static_cast<double>(m)); }
inline void fromAngle(Vec3f& out, float a, float m) {
    out.x = cosTimes(a, m);
    out.y = sinTimes(a, m);
}
// A vector divided by a scalar: the original multiplies by the single-precision reciprocal.
inline Vec3f divVec(const Vec3f& v, float s) {
    const float r = 1.0f / s;
    return {v.x * r, v.y * r, v.z * r};
}
// A frame timer as the original zero-fills it (previous count 0: "not ticked yet").
inline Timer zeroTimer() { return Timer{0, 0.0f, 0}; }
inline bool hasTicked(const Timer& t) { return t.current != t.previous; }

// The six easings of interpolators and interpolated movement (anything else is linear).
float eclEase(int mode, float t);

// Logs an unexpected data condition once per distinct message.
void eclWarnOnce(const std::string& message);

// ---- the file -------------------------------------------------------------------------------

class EclFile {
  public:
    static constexpr int kTimelines = 16;
    static constexpr std::uint32_t kHeaderSize = 0x44;
    // Reads past an instruction's end (the original reads the spawn records' unused fields,
    // and a few instructions read more words than they were given) land in this zero tail.
    static constexpr std::size_t kPad = 128;

    // Validates the header and every sub and timeline offset; the bytes become this stage's
    // mutable copy.
    bool load(std::span<const std::uint8_t> bytes, std::string* error);
    void clear();
    bool loaded() const { return size_ != 0; }

    int subCount() const { return subs_; }
    int timelineCount() const { return timelines_; }
    std::uint32_t subOffset(int i) const { return subOffsets_[static_cast<std::size_t>(i)]; }
    std::uint32_t timelineOffset(int i) const { return timelineOffsets_[static_cast<std::size_t>(i)]; }
    bool validSub(int i) const { return i >= 0 && i < subs_; }
    std::uint32_t size() const { return size_; }

    // An instruction header (12 bytes) lies inside the file, with a size of at least 12 that
    // does not run past the end.
    bool validInstruction(std::uint32_t off) const;
    std::uint8_t* at(std::uint32_t off) { return bytes_.data() + off; }
    const std::uint8_t* at(std::uint32_t off) const { return bytes_.data() + off; }
    std::vector<std::uint8_t>& bytes() { return bytes_; }

  private:
    void audit();

    std::vector<std::uint8_t> bytes_;
    std::uint32_t size_ = 0;
    int subs_ = 0, timelines_ = 0;
    std::vector<std::uint32_t> subOffsets_;
    std::array<std::uint32_t, kTimelines> timelineOffsets_{};
};

// Little-endian access to script bytes.
inline std::int32_t eclI32(const std::uint8_t* p) {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline float eclF32(const std::uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
inline std::int16_t eclI16(const std::uint8_t* p) {
    std::int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline std::uint16_t eclU16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline void eclPutI32(std::uint8_t* p, std::int32_t v) { std::memcpy(p, &v, 4); }
inline void eclPutF32(std::uint8_t* p, float v) { std::memcpy(p, &v, 4); }

// ---- the VM's state -------------------------------------------------------------------------

// The context's local variables (0x68 bytes), with the copy of the globals that every call
// refreshes.
struct EclArgs {
    std::array<std::int32_t, 4> intA{};
    std::array<float, 8> floatA{};
    std::array<std::int32_t, 4> intB{};
    std::array<float, 2> floatB{};
    std::array<std::int32_t, 4> intG{};
    std::array<float, 4> floatG{};
};

// The process-wide globals: zero at program start and never reset, not even between stages
// or games (spec 2.1).
struct EclGlobals {
    std::array<std::int32_t, 4> ints{};
    std::array<float, 4> floats{};
};
inline EclGlobals g_eclGlobals;

// One interpolator slot (opcode 27).
struct EclInterp {
    enum Routine : std::uint8_t { None, Linear, Hermite };
    Routine routine = None;
    Timer timer = zeroTimer();
    std::int32_t duration = 0, routineIndex = 0, easing = 0;
    float p0 = 0, p1 = 0, m0 = 0, m1 = 0;
    float target = 0;  // the float-encoded variable id
};

// A call context. Calls copy it whole to the stack and returns restore it whole,
// interpolators and the per-frame routine included.
struct EclContext {
    std::uint32_t pos = 0;  // the next instruction to consider (file offset)
    Timer time = zeroTimer();
    std::int8_t exRoutine = -1;  // per-frame routine, -1 none
    std::uint32_t exPos = 0;     // the instruction that installed it (its arguments are re-read)
    EclArgs args;
    Timer wait = zeroTimer();
    std::array<EclInterp, 8> interps{};
    std::int32_t laserTest = 0;  // opcode 88's result; nothing reads it
    bool periodic = false;
    std::int16_t sub = 0;
};

}  // namespace touhou::th07
