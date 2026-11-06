#include "th06/Translation.hpp"

#include <format>

#include "core/Log.hpp"
#include "gfx/TextRenderer.hpp"

namespace touhou::th06 {
namespace {

constexpr std::size_t kMaxLineBytes = 249;  // the MSG length byte: 4 header bytes, the marker, the NUL

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}

// The translated lines of a box: {"lines": [...]} or a bare array.
const Json* linesOf(const Json* box) {
    if (!box) return nullptr;
    if (box->isObject()) box = box->find("lines");
    return box && box->isArray() ? box : nullptr;
}

}  // namespace

std::optional<std::string> Translation::spellName(int spellId) const {
    const Json* j = json("th06/spells.js");
    const Json* v = j ? j->find(std::to_string(spellId)) : nullptr;
    if (!v || !v->isString()) return std::nullopt;
    return clean(v->string(), true);
}

std::optional<std::string> Translation::stageTitle(int stage) const {
    const Json* j = json("th06/stages.js");
    const Json* v = j ? j->find(std::to_string(stage)) : nullptr;
    if (v && v->isArray() && !v->array().empty()) v = &v->array().front();
    if (!v || !v->isString()) return std::nullopt;
    return clean(v->string(), true);
}

std::optional<std::string> Translation::musicTitle(int track) const {
    const Json* j = json("themes.js");
    const Json* v = j ? j->find(std::format("th06_{:02d}", track)) : nullptr;
    if (!v || !v->isString()) return std::nullopt;
    return clean(v->string(), false);
}

std::optional<std::string> Translation::stringDef(const std::string& id) const {
    const Json* j = json("stringdefs.js");
    const Json* v = j ? j->find(id) : nullptr;
    if (!v || !v->isString()) return std::nullopt;
    return clean(v->string(), false);
}

std::string Translation::musicCaption(const std::string& title) const {
    std::string format = stringDef("th06 BGM In-game format").value_or("\xE2\x99\xAA%s");  // "♪%s"
    const std::size_t at = format.find("%s");
    if (at == std::string::npos) return format + title;
    return format.substr(0, at) + title + format.substr(at + 2);
}

std::optional<std::vector<std::string>> Translation::musicComment(int track) const {
    const Json* j = json("th06/musiccmt.js");
    const Json* v = j ? j->find(std::to_string(track)) : nullptr;
    if (!v || !v->isArray()) return std::nullopt;
    std::vector<std::string> lines;
    for (const Json& l : v->array()) lines.push_back(l.isString() ? clean(l.string(), true) : std::string());
    return lines;
}

// thcrap's rules for numbering dialogue boxes: a box is keyed "<time>_<i>" (op 3) or
// "<time>_h1_<i>" (op 8, boss title cards), i counting boxes that start at the same time;
// translated lines replace the box's lines (extra original lines are dropped, extra
// translated lines inserted after the last one).
bool Translation::patchMsg(std::vector<std::uint8_t>& msg, int stage) const {
    const Json* diff = json(std::format("th06/msg{}.dat.jdiff", stage));
    if (!diff || !diff->isObject() || msg.size() < 4) return false;
    const std::uint32_t count = le32(msg, 0);
    const std::size_t table = 4 + 4 * static_cast<std::size_t>(count);
    if (count > 4096 || table > msg.size()) return false;
    std::vector<std::uint32_t> offsets(count);
    for (std::uint32_t i = 0; i < count; ++i) offsets[i] = le32(msg, 4 + 4 * static_cast<std::size_t>(i));

    struct Cmd {
        std::uint16_t time;
        std::uint8_t type;
        std::vector<std::uint8_t> data;
    };
    std::vector<Cmd> out;
    std::map<std::size_t, std::size_t> firstOut;  // original position -> output index
    auto setText = [](Cmd& c, int lineIndex, const std::string& text) {
        const std::uint8_t s0 = c.data.size() > 0 ? c.data[0] : 0, s1 = c.data.size() > 1 ? c.data[1] : 0;
        c.data = {s0, s1, static_cast<std::uint8_t>(lineIndex), static_cast<std::uint8_t>(lineIndex >> 8)};
        if (!text.empty()) {
            c.data.push_back(kUtf8TextMarker);
            const std::string_view t = utf8Prefix(text, kMaxLineBytes);
            c.data.insert(c.data.end(), t.begin(), t.end());
        }
        c.data.push_back(0);
    };
    auto validLine = [](const Json& l) { return l.isString() && (l.string().empty() || l.string()[0] != '|'); };

    const Json* entry = nullptr;
    const Json* lines = nullptr;
    int time = -1, ind = -1, cur = 0;
    int lastLineOp = -1;
    std::size_t lastLineCmd = 0;
    bool entryNew = true, changed = false;
    auto boxEnd = [&] {
        if (lastLineOp < 0) return;
        if (lines) {
            std::size_t at = lastLineCmd + 1;
            while (cur < static_cast<int>(lines->size())) {
                const Json& l = lines->array()[static_cast<std::size_t>(cur)];
                Cmd c = out[lastLineCmd];
                setText(c, cur, validLine(l) ? clean(l.string(), true) : std::string());
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), std::move(c));
                for (auto& [pos, idx] : firstOut) {
                    if (idx >= at) ++idx;
                }
                ++at;
                ++cur;
                changed = true;
            }
        }
        cur = 0;
        lines = nullptr;
    };

    std::size_t pos = table;
    while (pos + 4 <= msg.size()) {
        Cmd c{le16(msg, pos), msg[pos + 2], {}};
        const std::size_t len = msg[pos + 3];
        if (pos + 4 + len > msg.size()) break;
        c.data.assign(msg.begin() + static_cast<std::ptrdiff_t>(pos + 4), msg.begin() + static_cast<std::ptrdiff_t>(pos + 4 + len));
        const std::size_t here = pos;
        pos += 4 + len;
        firstOut.emplace(here, out.size());
        if (c.time == 0 && c.type == 0) {
            boxEnd();
            entryNew = true;
        }
        if (entryNew && c.type != 0) {
            int idx = -1;
            for (std::uint32_t i = 0; i < count && idx < 0; ++i) {
                if (offsets[i] == here) idx = static_cast<int>(i);
            }
            entry = idx >= 0 ? diff->find(std::to_string(idx)) : nullptr;
            if (entry && !entry->isObject()) entry = nullptr;
            time = -1;
            entryNew = false;
        }
        if (c.time != time) {
            time = c.time;
            ind = -1;
        }
        out.push_back(c);
        if ((c.type == 3 || c.type == 8) && entry && c.data.size() >= 4) {
            const bool h1 = c.type == 8;
            const int lineIndex = c.data[2] | c.data[3] << 8;
            if (lineIndex == 0) {
                boxEnd();
                if (lastLineOp >= 0) {
                    const bool lastH1 = lastLineOp == 8;
                    if (lastH1 != h1 || (lastH1 && h1)) ind = -1;
                }
            }
            cur = lineIndex;
            if (!lines) {
                ++ind;
                const std::string key = h1 ? std::format("{}_h1_{}", time, ind) : std::format("{}_{}", time, ind);
                lines = linesOf(entry->find(key));
            }
            if (lines) {
                if (cur < static_cast<int>(lines->size()) && validLine(lines->array()[static_cast<std::size_t>(cur)])) {
                    setText(out.back(), cur, clean(lines->array()[static_cast<std::size_t>(cur)].string(), true));
                    lastLineOp = c.type;
                    lastLineCmd = out.size() - 1;
                } else {
                    out.pop_back();  // an original line beyond the translation's lines
                }
                ++cur;
                changed = true;
            }
        }
    }
    if (!changed) return false;

    // Rebuild: the table, then the commands; offsets follow their commands.
    std::vector<std::size_t> outPos(out.size() + 1);
    std::size_t at = table;
    for (std::size_t i = 0; i < out.size(); ++i) {
        outPos[i] = at;
        at += 4 + out[i].data.size();
    }
    outPos[out.size()] = at;
    std::vector<std::uint8_t> result(table, 0);
    for (int k = 0; k < 4; ++k) result[static_cast<std::size_t>(k)] = msg[static_cast<std::size_t>(k)];
    for (std::uint32_t i = 0; i < count; ++i) {
        auto f = firstOut.find(offsets[i]);
        const std::size_t p = f != firstOut.end() ? outPos[f->second] : offsets[i];
        for (int k = 0; k < 4; ++k) result[4 + 4 * i + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(p >> (8 * k));
    }
    for (const Cmd& c : out) {
        result.push_back(static_cast<std::uint8_t>(c.time));
        result.push_back(static_cast<std::uint8_t>(c.time >> 8));
        result.push_back(c.type);
        result.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(c.data.size(), 255)));
        result.insert(result.end(), c.data.begin(), c.data.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(c.data.size(), 255)));
    }
    result.insert(result.end(), msg.begin() + static_cast<std::ptrdiff_t>(pos), msg.end());  // anything after the commands
    msg = std::move(result);
    return true;
}

// Ending files: every line numbered from 0 (commands start with '@'); a run of text lines
// starting at line L is replaced by jdiff["L"].lines.
bool Translation::patchEnd(std::vector<std::uint8_t>& end, std::string_view fileName) const {
    const Json* diff = json("th06/" + std::string(fileName) + ".jdiff");
    if (!diff || !diff->isObject()) return false;
    std::vector<std::string_view> fileLines;
    const std::string_view all(reinterpret_cast<const char*>(end.data()), end.size());
    std::size_t start = 0;
    while (start < all.size()) {
        const std::size_t nl = all.find('\n', start);
        if (nl == std::string_view::npos) {
            fileLines.push_back(all.substr(start));
            break;
        }
        fileLines.push_back(all.substr(start, nl + 1 - start));  // (with its '\n')
        start = nl + 1;
    }
    auto isCommand = [](std::string_view l) { return !l.empty() && l[0] == '@'; };
    std::string out;
    bool changed = false;
    for (std::size_t i = 0; i < fileLines.size();) {
        if (isCommand(fileLines[i])) {
            out += fileLines[i++];
            continue;
        }
        std::size_t runEnd = i;
        while (runEnd < fileLines.size() && !isCommand(fileLines[runEnd])) ++runEnd;
        const Json* lines = linesOf(diff->find(std::to_string(i)));
        if (lines) {
            for (const Json& l : lines->array()) {
                const std::string text = l.isString() ? clean(l.string(), false) : std::string();
                if (!text.empty()) {
                    out.push_back(static_cast<char>(kUtf8TextMarker));
                    out += text;
                }
                out.push_back('\0');
                out.push_back('\n');
            }
            changed = true;
        } else {
            for (std::size_t k = i; k < runEnd; ++k) out += fileLines[k];
        }
        i = runEnd;
    }
    if (!changed) return false;
    end.assign(out.begin(), out.end());
    return true;
}

}  // namespace touhou::th06
