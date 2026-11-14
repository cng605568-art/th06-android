#include "th08/ShotData.hpp"

#include <cstring>
#include <format>
#include <map>

namespace touhou::th08 {

namespace {

template <typename T>
T rd(std::span<const std::uint8_t> b, std::size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

constexpr std::size_t kHeader = 0x38;
constexpr std::size_t kEntry = 0x38;

}  // namespace

bool ShotFile::parse(std::span<const std::uint8_t> b, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (b.size() < kHeader) return fail("shot file too short");
    const int levelCount = rd<std::uint16_t>(b, 0x02);
    bombsPerLife = rd<float>(b, 0x04);
    deathbombCap = rd<std::int32_t>(b, 0x08);
    hitbox = rd<float>(b, 0x0C);
    grazeBox = rd<float>(b, 0x10);
    itemHomingSpeed = rd<float>(b, 0x14);
    itemBox = rd<float>(b, 0x18);
    pocLine = rd<float>(b, 0x1C);
    speed = rd<float>(b, 0x24);
    focusSpeed = rd<float>(b, 0x28);
    diagonalSpeed = rd<float>(b, 0x2C);
    focusDiagonalSpeed = rd<float>(b, 0x30);
    itemFactor = rd<float>(b, 0x34);
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
                e.side = rd<std::int16_t>(b, at + 0x1E);
                e.origin = rd<std::int16_t>(b, at + 0x20);
                e.shotClass = rd<std::int16_t>(b, at + 0x22);
                e.sprite = rd<std::int16_t>(b, at + 0x24);
                e.sound = rd<std::int16_t>(b, at + 0x26);
                e.fireKind = rd<std::uint32_t>(b, at + 0x28);
                e.updateKind = rd<std::uint32_t>(b, at + 0x2C);
                e.drawKind = rd<std::uint32_t>(b, at + 0x30);
                e.hitKind = rd<std::uint32_t>(b, at + 0x34);
                list->push_back(e);
            }
            found = lists.emplace(offset, std::move(list)).first;
        }
        level.entries = found->second;
        levels.push_back(std::move(level));
    }
    return true;
}

int ShotFile::levelFor(int power) const {
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i].threshold > power) return static_cast<int>(i);
    }
    return static_cast<int>(levels.size()) - 1;  // no sentinel: the original would read past the table
}

const std::vector<ShotEntry>& ShotFile::entries(int level) const {
    static const std::vector<ShotEntry> kNone;
    if (levels.empty()) return kNone;
    if (level < 0 || level >= static_cast<int>(levels.size())) level = static_cast<int>(levels.size()) - 1;
    return *levels[static_cast<std::size_t>(level)].entries;
}

std::string shotFileName(int character, bool focused) {
    const int team = teamOf(character);
    // Teams: the human's file unfocused, the youkai's focused; solos their own for both.
    const bool youkai = isSolo(character) ? isYoukaiSolo(character) : focused;
    return std::format("ply0{}a{}.sht", team, youkai ? "s" : "");
}

std::string playerAnmName(int character) { return std::format("player0{}.anm", teamOf(character)); }

}  // namespace touhou::th08
