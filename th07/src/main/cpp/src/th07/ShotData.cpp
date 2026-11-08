#include "th07/ShotData.hpp"

#include <cstring>
#include <format>
#include <map>

#include "core/Log.hpp"

namespace touhou::th07 {

namespace {

template <typename T>
T rd(std::span<const std::uint8_t> b, std::size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

constexpr std::size_t kHeader = 0x34;
constexpr std::size_t kEntry = 0x34;

}  // namespace

bool ShotFile::parse(std::span<const std::uint8_t> b, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (b.size() < kHeader) return fail("shot file too short");
    const int levelCount = rd<std::uint16_t>(b, 0x02);
    initialBombs = rd<float>(b, 0x04);
    deathbombWindow = rd<std::int32_t>(b, 0x08);
    hitbox = rd<float>(b, 0x0C);
    grazeBox = rd<float>(b, 0x10);
    autoCollectSpeed = rd<float>(b, 0x14);
    itemBox = rd<float>(b, 0x18);
    cherryPenalty = rd<float>(b, 0x1C);
    pocLine = rd<float>(b, 0x20);
    speed = rd<float>(b, 0x24);
    focusSpeed = rd<float>(b, 0x28);
    diagonalSpeed = rd<float>(b, 0x2C);
    focusDiagonalSpeed = rd<float>(b, 0x30);
    if (levelCount <= 0 || kHeader + 8ull * static_cast<unsigned>(levelCount) > b.size()) return fail("bad level table");
    levels.clear();
    std::map<std::uint32_t, std::shared_ptr<const std::vector<ShotEntry>>> lists;
    for (int l = 0; l < levelCount; ++l) {
        const std::size_t rec = kHeader + 8 * static_cast<std::size_t>(l);
        const auto offset = rd<std::uint32_t>(b, rec);
        Level level;
        level.threshold = rd<std::int32_t>(b, rec + 4);
        auto found = lists.find(offset);
        if (found == lists.end()) {
            auto list = std::make_shared<std::vector<ShotEntry>>();
            for (std::size_t at = offset;; at += kEntry) {
                if (at + 2 > b.size()) return fail("entry list runs off the file");
                if (rd<std::int16_t>(b, at) < 0) break;
                if (at + kEntry > b.size()) return fail("entry list runs off the file");
                ShotEntry e;
                e.interval = rd<std::int16_t>(b, at);
                e.phase = rd<std::int16_t>(b, at + 0x02);
                e.offsetX = rd<float>(b, at + 0x04);
                e.offsetY = rd<float>(b, at + 0x08);
                e.boxW = rd<float>(b, at + 0x0C);
                e.boxH = rd<float>(b, at + 0x10);
                e.angle = rd<float>(b, at + 0x14);
                e.speed = rd<float>(b, at + 0x18);
                e.damage = rd<std::int16_t>(b, at + 0x1C);
                e.origin = rd<std::int8_t>(b, at + 0x1E);
                e.behaviour = rd<std::int8_t>(b, at + 0x1F);
                e.script = rd<std::int16_t>(b, at + 0x20);
                e.sound = rd<std::int16_t>(b, at + 0x22);
                e.fireKind = rd<std::uint32_t>(b, at + 0x24);
                e.updateKind = rd<std::uint32_t>(b, at + 0x28);
                e.drawKind = rd<std::uint32_t>(b, at + 0x2C);
                e.hitKind = rd<std::uint32_t>(b, at + 0x30);
                if (e.hitKind == 3) {
                    log::warn("shot entry with hit kind 3 (invalid in the original): no hit behaviour");
                    e.hitKind = 0;
                }
                list->push_back(e);
            }
            found = lists.emplace(offset, std::move(list)).first;
        }
        level.entries = found->second;
        levels.push_back(std::move(level));
    }
    return true;
}

const std::vector<ShotEntry>& ShotFile::entriesFor(int power) const {
    for (const Level& l : levels) {
        if (l.threshold > power) return *l.entries;
    }
    return *levels.back().entries;  // no sentinel: the original would read past the table
}

std::string shotFileName(int shotIndex, bool focused) {
    return std::format("data/ply0{}{}{}.sht", shotIndex / 2, shotIndex % 2 == 0 ? 'a' : 'b', focused ? "s" : "");
}

std::string playerAnmName(int shotIndex) { return std::format("data/player0{}.anm", shotIndex / 2); }

}  // namespace touhou::th07
