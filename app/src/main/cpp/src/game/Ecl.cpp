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
    if (header.timelineOffsets[0] < tableEnd || header.timelineOffsets[0] + 2 > bytes.size()) {
        return fail("timeline offset out of range");
    }

    std::vector<std::uint32_t> subs(static_cast<std::size_t>(header.subCount));
    for (std::size_t i = 0; i < subs.size(); ++i) {
        std::memcpy(&subs[i], bytes.data() + sizeof(EclRawHeader) + 4 * i, 4);
        if (subs[i] < tableEnd || subs[i] + sizeof(EclRawInstr) > bytes.size()) {
            return fail("sub " + std::to_string(i) + " offset out of range");
        }
    }

    // Walk the timeline to find where it ends. The game files end it with a short record
    // whose time is negative; often only its first 4 bytes exist, at the very end of the file,
    // since the game reads nothing but `time` there. A damaged chain is tolerated the same way
    // the game tolerates it: play simply stops where the records stop.
    std::uint32_t at = header.timelineOffsets[0];
    std::string problem;
    int records = 0;
    for (;; ++records) {
        if (at + 2 > bytes.size()) {
            problem = "runs to the end of the file";
            break;
        }
        std::int16_t time;
        std::memcpy(&time, bytes.data() + at, 2);
        if (time < 0) break;  // terminator
        if (at + sizeof(TimelineInstr) > bytes.size()) {
            problem = "last record is cut off";
            break;
        }
        TimelineInstr t;
        std::memcpy(&t, bytes.data() + at, sizeof(t));
        if (t.size < static_cast<std::int16_t>(sizeof(TimelineInstr)) || at + static_cast<std::uint32_t>(t.size) > bytes.size()) {
            problem = "record with size " + std::to_string(t.size);
            break;
        }
        at += static_cast<std::uint32_t>(t.size);
    }
    if (!problem.empty()) {
        log::warn("ECL timeline: {} after {} records (offset 0x{:x}); it ends there", problem, records, at);
    }
    timelineEnd_ = at;
    timelineRecords_ = records;

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
    out = {};
    args = {};
    if (static_cast<std::size_t>(offset) + 2 > bytes_.size()) return false;
    const std::size_t head = std::min(sizeof(TimelineInstr), bytes_.size() - offset);
    std::memcpy(&out, bytes_.data() + offset, head);  // a short terminator only has its time
    if (head < sizeof(TimelineInstr)) return true;
    const std::size_t argBytes = out.size > 8 ? static_cast<std::size_t>(out.size) - 8 : 0;
    const std::size_t available = bytes_.size() - offset - sizeof(TimelineInstr);
    std::memcpy(&args, bytes_.data() + offset + sizeof(TimelineInstr),
                std::min({argBytes, sizeof(TimelineInstrArgs), available}));
    return true;
}

}  // namespace th06
