#pragma once

// Imperishable Night's ECL scripts (claude/th08-specs/spec_files.md §3.2-3.3,
// spec_ecl_enemies.md): the file (magic 0x800, subs, up to 16 timelines), the argument
// encoding (TH07's per-instruction variable mask), the call contexts and threads the VM keeps
// per enemy, and the helpers that reproduce the original's x87 results. The VM and the
// instruction set run inside the enemy manager (Enemies.hpp).
//
// Scripts write into their own argument words (a write to anything that is not a writable
// variable lands in the loaded script), so the file is a private mutable copy per stage.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/ZunMath.hpp"
#include "th08/Anm.hpp"

namespace touhou::th08 {

// ---- x87-faithful arithmetic ---------------------------------------------------------------

// Float to i32 by truncation; NaN and out-of-range values give 0x80000000 (x86).
inline std::int32_t x86ToInt(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return static_cast<std::int32_t>(0x80000000u);
    return static_cast<std::int32_t>(f);
}
inline float zsin(float a) { return static_cast<float>(std::sin(static_cast<double>(a))); }
inline float zcos(float a) { return static_cast<float>(std::cos(static_cast<double>(a))); }
inline float zatan2(float y, float x) { return static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x))); }
inline float cosTimes(float a, float m) { return static_cast<float>(std::cos(static_cast<double>(a)) * static_cast<double>(m)); }
inline float sinTimes(float a, float m) { return static_cast<float>(std::sin(static_cast<double>(a)) * static_cast<double>(m)); }
inline void fromAngle(Vec3f& out, float a, float m) {
    out.x = cosTimes(a, m);
    out.y = sinTimes(a, m);
}
inline Timer zeroTimer() { return Timer{0, 0.0f, 0}; }
inline bool hasTicked(const Timer& t) { return t.current != t.previous; }

float eclEase(int mode, float t);
void eclWarnOnce(const std::string& message);

// ---- the file -------------------------------------------------------------------------------

class EclFile {
  public:
    static constexpr int kTimelines = 16;
    static constexpr std::uint32_t kHeaderSize = 0x48;
    static constexpr std::size_t kPad = 256;

    bool load(std::span<const std::uint8_t> bytes, std::string* error);
    void clear();
    bool loaded() const { return size_ != 0; }

    int subCount() const { return subs_; }
    int timelineCount() const { return timelines_; }
    std::uint32_t subOffset(int i) const { return subOffsets_[static_cast<std::size_t>(i)]; }
    std::uint32_t timelineOffset(int i) const { return timelineOffsets_[static_cast<std::size_t>(i)]; }
    bool validSub(int i) const { return i >= 0 && i < subs_; }
    std::uint32_t size() const { return size_; }
    bool validInstruction(std::uint32_t off) const;
    std::uint8_t* at(std::uint32_t off) { return bytes_.data() + off; }
    const std::uint8_t* at(std::uint32_t off) const { return bytes_.data() + off; }

  private:
    std::vector<std::uint8_t> bytes_;
    std::uint32_t size_ = 0;
    int subs_ = 0, timelines_ = 0;
    std::vector<std::uint32_t> subOffsets_;
    std::array<std::uint32_t, kTimelines> timelineOffsets_{};
};

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

// A context's args block (0x78 bytes, spec 2.1), with the copy G' of the true globals that
// every call refreshes.
struct EclArgs {
    std::array<std::int32_t, 8> intA{};
    std::array<float, 8> floatA{};
    std::array<std::int32_t, 4> intB{};
    std::array<float, 2> floatB{};
    std::array<std::int32_t, 4> intG{};
    std::array<float, 4> floatG{};
};

// The true globals (ids 10061-10068), zeroed at every stage start in TH08.
struct EclGlobals {
    std::array<std::int32_t, 4> ints{};
    std::array<float, 4> floats{};
};

// One interpolator slot (opcode 36).
struct EclInterp {
    enum Routine : std::uint8_t { None, Linear, Hermite };
    Routine routine = None;
    Timer timer = zeroTimer();
    std::int32_t duration = 0, routineIndex = 0, easing = 0;
    float p0 = 0, p1 = 0, m0 = 0, m1 = 0;
    float target = 0;  // the float-encoded variable id
};

// A call context (0x228 bytes, spec 4.1). Calls save it whole and returns restore it whole,
// interpolators and the per-frame routine included.
struct EclContext {
    std::uint32_t pos = 0;
    Timer time = zeroTimer();
    std::int8_t exRoutine = -1;
    std::uint32_t exPos = 0;
    EclArgs args;
    Timer wait = zeroTimer();
    std::array<EclInterp, 8> interps{};
    std::int32_t thread = 0;  // thread number + 1 (0: the main context)
    std::int16_t sub = 0;
};

// A thread (spec 4.3): its sub, working depth, context and its own 16-entry stack.
struct EclThread {
    std::int32_t sub = 0;
    std::int16_t depth = 0;
    EclContext ctx;
    std::array<EclContext, 16> stack{};
};

}  // namespace touhou::th08
