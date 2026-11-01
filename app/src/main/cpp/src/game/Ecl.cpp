#include "game/Ecl.hpp"

#include <algorithm>
#include <cstring>

#include "core/Log.hpp"

namespace th06 {

bool EclFile::load(std::vector<std::uint8_t> bytes, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = "ECL: " + why;
        return false;
    };
    if (bytes.size() < sizeof(EclRawHeader)) return fail("file too small");
    EclRawHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.subCount <= 0 || header.subCount > 4096) return fail("implausible sub count");
    const std::size_t tableEnd = sizeof(EclRawHeader) + 4u * static_cast<std::size_t>(header.subCount);
    if (tableEnd > bytes.size()) return fail("sub table runs past end of file");
    if (header.timelineOffsets[0] < tableEnd || header.timelineOffsets[0] + sizeof(TimelineInstr) > bytes.size()) {
        return fail("timeline offset out of range");
    }

    std::vector<std::uint32_t> subs(static_cast<std::size_t>(header.subCount));
    for (std::size_t i = 0; i < subs.size(); ++i) {
        std::memcpy(&subs[i], bytes.data() + sizeof(EclRawHeader) + 4 * i, 4);
        if (subs[i] < tableEnd || subs[i] + sizeof(EclRawInstr) > bytes.size()) {
            return fail("sub " + std::to_string(i) + " offset out of range");
        }
    }

    // The timeline must end (time < 0) inside the file with positive record sizes.
    std::uint32_t at = header.timelineOffsets[0];
    for (int n = 0;; ++n) {
        if (at + sizeof(TimelineInstr) > bytes.size() || n > 100000) return fail("timeline is not terminated");
        TimelineInstr t;
        std::memcpy(&t, bytes.data() + at, sizeof(t));
        if (t.time < 0) break;
        if (t.size < static_cast<std::int16_t>(sizeof(TimelineInstr))) return fail("timeline record with bad size");
        at += static_cast<std::uint32_t>(t.size);
    }

    bytes_ = std::move(bytes);
    subOffsets_ = std::move(subs);
    timelineOffset_ = header.timelineOffsets[0];
    log::info("ECL: {} subs, timeline at 0x{:x}", subOffsets_.size(), timelineOffset_);
    return true;
}

std::uint32_t EclFile::subOffset(int subId) const {
    if (subId < 0 || subId >= subCount()) return 0;
    return subOffsets_[static_cast<std::size_t>(subId)];
}

bool EclFile::instrAt(std::uint32_t offset, EclRawInstr& out) const {
    if (offset < sizeof(EclRawHeader) || static_cast<std::size_t>(offset) + sizeof(EclRawInstr) > bytes_.size()) {
        return false;
    }
    std::memcpy(&out, bytes_.data() + offset, sizeof(out));
    return out.offsetToNext >= static_cast<std::int16_t>(sizeof(EclRawInstr)) &&
           static_cast<std::size_t>(offset) + static_cast<std::size_t>(out.offsetToNext) <= bytes_.size();
}

bool EclFile::timelineAt(std::uint32_t offset, TimelineInstr& out, TimelineInstrArgs& args) const {
    if (static_cast<std::size_t>(offset) + sizeof(TimelineInstr) > bytes_.size()) return false;
    std::memcpy(&out, bytes_.data() + offset, sizeof(out));
    args = {};
    const std::size_t argBytes = out.size > 8 ? static_cast<std::size_t>(out.size) - 8 : 0;
    const std::size_t n = std::min(argBytes, sizeof(TimelineInstrArgs));
    if (static_cast<std::size_t>(offset) + 8 + n <= bytes_.size()) std::memcpy(&args, bytes_.data() + offset + 8, n);
    return true;
}

}  // namespace th06
