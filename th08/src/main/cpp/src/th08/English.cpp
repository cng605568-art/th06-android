#include "th08/English.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>

#include "core/Log.hpp"
#include "gfx/Image.hpp"

// Parts of this file are adapted from thcrap (Touhou Community Reliant Automatic Patcher),
// which is public domain (the Unlicense): the dialogue and ending patchers from
// thcrap_tsa/src/th06_msg.cpp, the PNG-over-THTX blitting from thcrap_tsa/src/anm.cpp and the
// layout markup from thcrap_tsa/src/layout.cpp. (Taken over from the TH07 port's English mode.)

namespace touhou::th08 {

namespace fs = std::filesystem;

namespace {

constexpr const char* kBuild = "v1.00d";
// A dialogue text command's argument room: the length byte's 255 minus the NUL (and, for
// the hard lines, the speaker and line fields); the UTF-8 marker counts.
constexpr std::size_t kMaxAutoLineBytes = 254, kMaxHardLineBytes = 250;
constexpr std::uint8_t kMsgMask = 0x77;  // thcrap's msg_crypt_th08: every text byte xor 0x77

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) { return le32(b.data() + at); }
std::int32_t i32(const std::uint8_t* p) { return static_cast<std::int32_t>(le32(p)); }
std::int16_t i16(const std::uint8_t* p) { return static_cast<std::int16_t>(p[0] | p[1] << 8); }
float f32(const std::uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
void put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void set32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
}

std::optional<std::vector<std::uint8_t>> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// thcrap's build-specific variant: ".<build>" before the first dot of the file name.
std::string buildVariant(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t base = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.find('.', base);
    if (dot == std::string::npos) return path + "." + kBuild;
    return path.substr(0, dot) + "." + kBuild + path.substr(dot);
}

// The translated lines of a box: {"lines": [...]} or a bare array.
const Json* linesOf(const Json* box) {
    if (!box) return nullptr;
    if (box->isObject()) box = box->find("lines");
    return box && box->isArray() ? box : nullptr;
}

// A line as thcrap's msg patcher takes it: a string, and when it starts with '|' (ruby) it
// must have two commas after it (thcrap: the games would crash on anything else).
std::optional<std::string> msgLine(const Json& l) {
    if (!l.isString()) return std::nullopt;
    const std::string& s = l.string();
    if (!s.empty() && s[0] == '|') {
        const std::size_t c1 = s.find(',', 1);
        if (c1 == std::string::npos || s.find(',', c1 + 1) == std::string::npos) return std::nullopt;
    }
    return s;
}

std::string_view dropNote(std::string_view s) {
    const std::size_t at = s.find('\x14');
    return at == std::string_view::npos ? s : s.substr(0, at);
}


}  // namespace

std::string English::gameText(std::string_view utf8) {
    std::string s;
    s.reserve(utf8.size() + 1);
    s.push_back(static_cast<char>(kUtf8TextMarker));
    s.append(utf8);
    return s;
}

// ---- the stack ----------------------------------------------------------------------------------

std::vector<std::string> English::discoverStack(const std::string& thcrapRoot) {
    std::vector<std::string> out;
    if (thcrapRoot.empty()) return out;
    std::error_code ec;
    const fs::path repos = fs::path(thcrapRoot) / "repos";
    if (!fs::is_directory(repos, ec)) return out;
    std::vector<std::string> repoNames;
    for (const auto& r : fs::directory_iterator(repos, ec)) {
        if (r.is_directory(ec)) repoNames.push_back(r.path().filename().string());
    }
    std::sort(repoNames.begin(), repoNames.end());
    auto has = [&](const std::string& repo, const std::string& id) { return fs::is_directory(repos / repo / id, ec); };
    std::string langRepo;
    if (has("thpatch", "lang_en")) langRepo = "thpatch";
    for (const std::string& r : repoNames) {
        if (langRepo.empty() && has(r, "lang_en")) langRepo = r;
    }
    if (langRepo.empty()) return out;

    // thcrap's order: a patch's dependencies (patch.js "dependencies": "repo/patch", or a
    // bare patch id looked up in the same repository first), depth-first, then the patch.
    std::set<std::string> seen;
    std::function<void(const std::string&, const std::string&)> add = [&](const std::string& repo, const std::string& id) {
        if (!seen.insert(repo + "/" + id).second) return;
        const fs::path dir = repos / repo / id;
        if (const auto text = readFile((dir / "patch.js").string())) {
            std::string why;
            const auto j = Json::parse(std::string_view(reinterpret_cast<const char*>(text->data()), text->size()), &why);
            const Json* deps = j ? j->find("dependencies") : nullptr;
            if (deps && deps->isArray()) {
                for (const Json& d : deps->array()) {
                    if (!d.isString()) continue;
                    const std::string& dep = d.string();
                    const std::size_t slash = dep.find('/');
                    if (slash != std::string::npos) {
                        add(dep.substr(0, slash), dep.substr(slash + 1));
                        continue;
                    }
                    std::string where = repo;
                    if (!has(repo, dep)) {
                        for (const std::string& r : repoNames) {
                            if (has(r, dep)) {
                                where = r;
                                break;
                            }
                        }
                    }
                    add(where, dep);
                }
            }
        }
        if (fs::is_directory(dir, ec)) out.push_back(dir.string() + "/");
        else log::info("English text: patch {}/{} is not in the imported files (skipped)", repo, id);
    };
    add(langRepo, "lang_en");
    return out;
}

English::English(std::vector<std::string> stack) : patches_(kBuild) {
    patches_.setStack(std::move(stack));
    // Which of the game's files each patch has (its th08 folder), by lower-cased path.
    for (const std::string& folder : patches_.folders()) {
        std::error_code ec;
        const fs::path game = fs::path(folder) / "th08";
        if (!fs::is_directory(game, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(game, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = lower(fs::relative(it->path(), fs::path(folder), ec).generic_string());
            files_.emplace(folder + "|" + rel, it->path().string());
        }
    }
    log::info("English text: {} patch folder(s), {} game file(s)", patches_.folders().size(), files_.size());
}

std::string English::resolve(const std::string& rel) const {
    const auto& folders = patches_.folders();
    const std::string key = lower(rel), variant = lower(buildVariant(rel));
    for (auto f = folders.rbegin(); f != folders.rend(); ++f) {
        for (const std::string& k : {variant, key}) {
            if (const auto it = files_.find(*f + "|" + k); it != files_.end()) return it->second;
        }
    }
    return {};
}

std::vector<std::string> English::resolveAll(const std::string& rel) const {
    std::vector<std::string> out;
    const std::string key = lower(rel), variant = lower(buildVariant(rel));
    for (const std::string& f : patches_.folders()) {
        for (const std::string& k : {key, variant}) {
            if (const auto it = files_.find(f + "|" + k); it != files_.end()) out.push_back(it->second);
        }
    }
    return out;
}

const Json* English::json(const std::string& path) const { return patches_.json(path); }

std::string English::keepNote(std::string_view line) const {
    const std::size_t at = line.find('\x14');
    if (at == std::string_view::npos) return std::string(line);
    const std::string note(line.substr(at + 1));
    auto it = noteIndex_.find(note);
    if (it == noteIndex_.end()) {
        it = noteIndex_.emplace(note, static_cast<int>(notes_.size())).first;
        notes_.push_back(note);
    }
    return std::string(line.substr(0, at)) + "\x14" + std::to_string(it->second);
}

std::string English::note(int index) const {
    std::lock_guard lock(mutex_);
    return index >= 0 && static_cast<std::size_t>(index) < notes_.size() ? notes_[static_cast<std::size_t>(index)] : std::string();
}

// ---- the file filter ----------------------------------------------------------------------------

std::optional<std::vector<std::uint8_t>> English::filterFile(const std::string& name, std::optional<std::vector<std::uint8_t>> bytes) const {
    std::lock_guard lock(mutex_);
    // A replacement: thcrap looks the game's files up by the name the game asks for (its
    // base name) in each patch's game folder, highest priority first.
    if (const std::string rep = resolve("th08/" + name); !rep.empty()) {
        if (auto b = readFile(rep)) {
            if (logged_.insert(name).second) log::info("English text: {} from {}", name, rep);
            bytes = std::move(b);
        }
    }
    if (!bytes) return bytes;
    const auto ends = [&](std::string_view s) { return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0; };
    if (ends(".anm")) {
        for (const auto& [n, b] : anmCache_) {
            if (n == name) return b;
        }
        if (patchAnm(*bytes, name)) {
            if (anmCache_.size() >= 3) anmCache_.erase(anmCache_.begin());
            anmCache_.emplace_back(name, *bytes);
        }
    } else if (name.starts_with("msg") && ends(".dat")) {
        if (const Json* d = json("th08/" + name + ".jdiff")) {
            if (patchMsg(*bytes, *d)) log::info("English text: {} patched", name);
        }
    } else if (ends(".end")) {
        if (const Json* d = json("th08/" + name + ".jdiff")) {
            if (patchEnd(*bytes, *d)) log::info("English text: {} patched", name);
        }
    }
    return bytes;
}

// ---- dialogue (msg08) ---------------------------------------------------------------------------

// Adapted from thcrap's patch_msg() with the MSG_TH08 format: opcode 3 is a "hard line" (a
// speaker, a line number, the text), 16, 19 and 20 are "auto lines" (the text only), 4 and 15
// end an auto-line box; every text is masked with 0x77. A box's key is "<time>_<i>", i counting
// the boxes that start at the same time (a line whose box has no translation counts again).
// A box's translated lines replace its lines in order (hard lines: by line number); original
// lines beyond the translation are dropped and extra translated lines are inserted after the
// box's last line, as copies of it. Script offsets follow their commands.
bool English::patchMsg(std::vector<std::uint8_t>& msg, const Json& diff) const {
    std::lock_guard lock(mutex_);
    if (!diff.isObject() || msg.size() < 4) return false;
    const std::uint32_t count = le32(msg, 0);
    const std::size_t table = 4 + 4 * static_cast<std::size_t>(count);
    if (count > 4096 || table > msg.size()) return false;
    std::vector<std::uint32_t> offsets(count);
    for (std::uint32_t i = 0; i < count; ++i) offsets[i] = le32(msg, 4 + 4 * static_cast<std::size_t>(i));

    enum class Op { None, Hard, Auto, AutoEnd };
    auto opOf = [](std::uint8_t type) {
        switch (type) {
            case 3: return Op::Hard;
            case 16: case 19: case 20: return Op::Auto;
            case 4: case 15: return Op::AutoEnd;
            default: return Op::None;
        }
    };
    struct Cmd {
        std::uint16_t time;
        std::uint8_t type;
        std::vector<std::uint8_t> data;
    };
    std::vector<Cmd> out;
    std::map<std::size_t, std::size_t> firstOut;  // original position -> output index
    // thcrap's REP_HARD_LINE / REP_AUTO_LINE: the text (marker + UTF-8, the note's index kept,
    // cut to the last whole character that fits) and its NUL, masked.
    auto setText = [&](Cmd& c, bool hard, int lineIndex, const std::string& line) {
        std::vector<std::uint8_t> text;
        if (hard) {
            const std::uint8_t s0 = !c.data.empty() ? c.data[0] : 0, s1 = c.data.size() > 1 ? c.data[1] : 0;
            text = {s0, s1, static_cast<std::uint8_t>(lineIndex), static_cast<std::uint8_t>(lineIndex >> 8)};
        }
        const std::size_t room = hard ? kMaxHardLineBytes : kMaxAutoLineBytes;
        std::vector<std::uint8_t> body;
        const std::string kept = keepNote(line);
        if (!kept.empty()) {
            const std::size_t note = kept.find('\x14');
            const std::string suffix = note == std::string::npos ? std::string() : kept.substr(note);
            const std::string_view main = std::string_view(kept).substr(0, note == std::string::npos ? kept.size() : note);
            const std::size_t fit = suffix.size() + 1 < room ? room - 1 - suffix.size() : 0;
            const std::string_view cut = utf8Prefix(main, fit);
            body.push_back(kUtf8TextMarker);
            body.insert(body.end(), cut.begin(), cut.end());
            if (suffix.size() + body.size() <= room) body.insert(body.end(), suffix.begin(), suffix.end());
        }
        body.push_back(0);
        for (std::uint8_t& b : body) b ^= kMsgMask;
        text.insert(text.end(), body.begin(), body.end());
        c.data = std::move(text);
    };

    const Json* entry = nullptr;
    const Json* lines = nullptr;
    int time = -1, ind = -1;
    std::size_t cur = 0;
    Op lastLineOp = Op::None;  // thcrap's last_line_op (None: no line replaced yet)
    std::size_t lastLineCmd = 0;
    bool entryNew = true, changed = false;
    auto lineAt = [&](std::size_t i) -> std::optional<std::string> {
        if (!lines || i >= lines->size()) return std::nullopt;
        return msgLine(lines->array()[i]);
    };
    // thcrap's box_end(): extra translated lines after the box's last line, then a new box.
    auto boxEnd = [&] {
        if (lastLineOp == Op::None) return;
        if (lines) {
            while (cur < lines->size()) {
                if (const auto t = lineAt(cur)) {
                    Cmd c = out[lastLineCmd];
                    setText(c, lastLineOp == Op::Hard, static_cast<int>(cur), *t);
                    const std::size_t at = lastLineCmd + 1;
                    out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), std::move(c));
                    for (auto& [pos, idx] : firstOut) {
                        if (idx >= at) ++idx;
                    }
                    lastLineCmd = at;
                    changed = true;
                }
                ++cur;
            }
        }
        cur = 0;
        lines = nullptr;
    };
    // thcrap's replace_next_diff_line(): false drops the command.
    auto replaceNext = [&](Op op) {
        if (!lines) {
            ++ind;
            lines = linesOf(entry->find(std::format("{}_{}", time, ind)));
        }
        if (!lines) return true;
        const auto t = lineAt(cur);
        if (t) {
            setText(out.back(), op == Op::Hard, static_cast<int>(cur), *t);
            lastLineOp = op;
            lastLineCmd = out.size() - 1;
        }
        ++cur;
        changed = true;
        return t.has_value();
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
        if (c.time == 0 && c.type == 0) {  // an entry's terminator: close any open box
            entryNew = true;
            boxEnd();
        }
        if (entryNew && c.type != 0) {
            int idx = -1;
            for (std::uint32_t i = 0; i < count && idx < 0; ++i) {
                if (offsets[i] == here) idx = static_cast<int>(i);
            }
            entry = idx >= 0 ? diff.find(std::to_string(idx)) : nullptr;
            if (entry && !entry->isObject()) entry = nullptr;
            entryNew = false;
            time = -1;
        }
        if (c.time != time) {
            time = c.time;
            ind = -1;
        }
        out.push_back(c);
        const Op op = opOf(c.type);
        if (op == Op::None || !entry) continue;
        bool keep = true;
        switch (op) {
            case Op::AutoEnd:  // thcrap's op_auto_end()
                if (lastLineOp == Op::None || lastLineOp == Op::Auto) boxEnd();
                break;
            case Op::Auto:
                if (lastLineOp == Op::Hard) boxEnd();
                keep = replaceNext(Op::Auto);
                break;
            case Op::Hard: {
                if (c.data.size() < 4) break;
                const std::size_t lineIndex = static_cast<std::size_t>(c.data[2] | c.data[3] << 8);
                if (lineIndex == 0) boxEnd();  // line 0 ends the box before
                cur = lineIndex;
                keep = replaceNext(Op::Hard);
                break;
            }
            case Op::None: break;
        }
        if (!keep) out.pop_back();  // an original line beyond the translation's lines
    }
    if (!changed) return false;

    // Rebuild: the table, then the commands; script offsets follow their commands.
    std::vector<std::size_t> outPos(out.size() + 1);
    std::size_t at = table;
    for (std::size_t i = 0; i < out.size(); ++i) {
        outPos[i] = at;
        at += 4 + std::min<std::size_t>(out[i].data.size(), 255);
    }
    outPos[out.size()] = at;
    std::vector<std::uint8_t> result(table, 0);
    std::copy(msg.begin(), msg.begin() + 4, result.begin());
    for (std::uint32_t i = 0; i < count; ++i) {
        auto f = firstOut.find(offsets[i]);
        set32(result, 4 + 4 * static_cast<std::size_t>(i), static_cast<std::uint32_t>(f != firstOut.end() ? outPos[f->second] : offsets[i]));
    }
    for (const Cmd& c : out) {
        const std::size_t n = std::min<std::size_t>(c.data.size(), 255);
        put16(result, c.time);
        result.push_back(c.type);
        result.push_back(static_cast<std::uint8_t>(n));
        result.insert(result.end(), c.data.begin(), c.data.begin() + static_cast<std::ptrdiff_t>(n));
    }
    result.insert(result.end(), msg.begin() + static_cast<std::ptrdiff_t>(pos), msg.end());  // anything after the commands
    msg = std::move(result);
    return true;
}

// ---- endings ------------------------------------------------------------------------------------

// Adapted from thcrap's patch_end_th06(): lines are numbered from 0 ('@' lines are
// commands); a run of text lines starting at line L is replaced by jdiff["L"].lines (each
// written NUL- and line-terminated); runs without a translation stay as they are.
bool English::patchEnd(std::vector<std::uint8_t>& end, const Json& diff) {
    if (!diff.isObject()) return false;
    std::vector<std::string_view> fileLines;
    const std::string_view all(reinterpret_cast<const char*>(end.data()), end.size());
    for (std::size_t start = 0; start < all.size();) {
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
        const Json* lines = linesOf(diff.find(std::to_string(i)));
        if (lines) {
            for (const Json& l : lines->array()) {
                if (!l.isString()) continue;  // (thcrap writes string lines only)
                const std::string_view text = dropNote(l.string());
                if (!text.empty()) out += gameText(text);
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

// ---- pictures over ANM textures -----------------------------------------------------------------

namespace {

int bytesPerPixel(int format) { return format == 1 ? 4 : (format == 3 || format == 5) ? 2 : 0; }

// thcrap's format_from_bgra(): RGBA8 pixels to the THTX format, no rounding.
std::vector<std::uint8_t> toFormat(const Image& img, int format) {
    const std::size_t n = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    std::vector<std::uint8_t> out(n * static_cast<std::size_t>(bytesPerPixel(format)));
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t r = img.rgba[i * 4], g = img.rgba[i * 4 + 1], b = img.rgba[i * 4 + 2], a = img.rgba[i * 4 + 3];
        if (format == 1) {
            out[i * 4] = b, out[i * 4 + 1] = g, out[i * 4 + 2] = r, out[i * 4 + 3] = a;
        } else if (format == 5) {
            out[i * 2 + 1] = static_cast<std::uint8_t>((a >> 4) << 4 | (r >> 4));
            out[i * 2] = static_cast<std::uint8_t>((g >> 4) << 4 | (b >> 4));
        } else {
            const unsigned v = static_cast<unsigned>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            out[i * 2] = static_cast<std::uint8_t>(v), out[i * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
        }
    }
    return out;
}

enum class Alpha { Empty, Opaque, Full };

// thcrap's sprite_alpha_analyze().
Alpha analyzeAlpha(const std::uint8_t* p, int format, std::size_t stride, int w, int h) {
    const unsigned alphaMax = format == 1 ? 0xFF : format == 5 ? 0xF : 0;
    const std::size_t opaqueSum = static_cast<std::size_t>(alphaMax) * static_cast<std::size_t>(w);
    if (!opaqueSum) return Alpha::Opaque;
    Alpha ret = Alpha::Full;
    for (int row = 0; row < h; ++row, p += stride) {
        std::size_t sum = 0;
        for (int x = 0; x < w; ++x) sum += format == 1 ? p[x * 4 + 3] : (p[x * 2 + 1] & 0xF0) >> 4;
        if (sum == 0 && ret != Alpha::Opaque) ret = Alpha::Empty;
        else if (sum == opaqueSum && ret != Alpha::Empty) ret = Alpha::Opaque;
        else return Alpha::Full;
    }
    return ret;
}

// thcrap's blit_blend() (alpha added and clamped) and blit_overwrite(), one row.
void blendRow(std::uint8_t* dst, const std::uint8_t* rep, int pixels, int format) {
    if (format == 1) {
        for (int i = 0; i < pixels; ++i, dst += 4, rep += 4) {
            const int newAlpha = dst[3] + rep[3];
            const int dstAlpha = 0xFF - rep[3];
            for (int k = 0; k < 3; ++k) dst[k] = static_cast<std::uint8_t>((dst[k] * dstAlpha + rep[k] * rep[3]) >> 8);
            dst[3] = static_cast<std::uint8_t>(std::min(newAlpha, 0xFF));
        }
    } else if (format == 5) {
        for (int i = 0; i < pixels; ++i, dst += 2, rep += 2) {
            const int ra = (rep[1] & 0xF0) >> 4, rr = rep[1] & 0x0F, rg = (rep[0] & 0xF0) >> 4, rb = rep[0] & 0x0F;
            const int da = (dst[1] & 0xF0) >> 4, dr = dst[1] & 0x0F, dg = (dst[0] & 0xF0) >> 4, db = dst[0] & 0x0F;
            const int newAlpha = da + ra, dstAlpha = 0xF - ra;
            dst[1] = static_cast<std::uint8_t>(std::min(newAlpha, 0xF) << 4 | ((dr * dstAlpha + rr * ra) >> 4));
            dst[0] = static_cast<std::uint8_t>(((dg * dstAlpha + rg * ra) & 0xF0) | ((db * dstAlpha + rb * ra) >> 4));
        }
    } else {
        std::memcpy(dst, rep, static_cast<std::size_t>(pixels) * static_cast<std::size_t>(bytesPerPixel(format)));
    }
}

}  // namespace

bool English::blitPng(std::uint8_t* thtx, int format, int w, int h, const std::vector<Rect>& sprites, const Image& png, int offsetX,
                      int offsetY) {
    const int bpp = bytesPerPixel(format);
    if (bpp == 0 || w <= 0 || h <= 0 || png.empty()) return false;
    const std::vector<std::uint8_t> rep = toFormat(png, format);
    // thcrap's anm_entry_t::transform_and_add_sprite(): sprite rectangles wrapped into the
    // texture; one sprite or none stands for the whole texture.
    struct Local {
        int x, y, w, h;
    };
    std::vector<Local> locals;
    if (sprites.size() <= 1) {
        locals.push_back({0, 0, w, h});
    } else {
        for (const Rect& s : sprites) {
            if (s.w == 0.0f || s.h == 0.0f) continue;
            const auto ux = static_cast<std::uint32_t>(std::max(0.0f, s.x)), uy = static_cast<std::uint32_t>(std::max(0.0f, s.y));
            Local lr{static_cast<int>(ux % static_cast<std::uint32_t>(w)), static_cast<int>(uy % static_cast<std::uint32_t>(h)),
                     std::min(static_cast<int>(s.w), w), std::min(static_cast<int>(s.h), h)};
            const int splitW = lr.x + lr.w - w, splitH = lr.y + lr.h - h;
            if (splitW > 0) lr.w = w - lr.x;
            if (splitH > 0) lr.h = h - lr.y;
            if (splitW > 0 && splitH > 0) locals.push_back({0, 0, splitW, splitH});
            if (splitW > 0) locals.push_back({0, lr.y, splitW, lr.h});
            if (splitH > 0) locals.push_back({lr.x, 0, lr.w, splitH});
            locals.push_back(lr);
        }
    }
    const std::size_t dstStride = static_cast<std::size_t>(w) * static_cast<std::size_t>(bpp);
    const std::size_t repStride = static_cast<std::size_t>(png.width) * static_cast<std::size_t>(bpp);
    bool any = false;
    for (const Local& s : locals) {
        // thcrap's sprite_patch_set() and sprite_patch() in the "auto" mode: nothing for an
        // empty replacement, a blend over an opaque original, otherwise an overwrite.
        const int repX = offsetX + s.x, repY = offsetY + s.y;
        if (s.w <= 0 || s.h <= 0 || repX < 0 || repY < 0 || repX >= png.width || repY >= png.height) continue;
        const int copyW = std::min(s.w, png.width - repX), copyH = std::min(s.h, png.height - repY);
        std::uint8_t* dst = thtx + static_cast<std::size_t>(s.y) * dstStride + static_cast<std::size_t>(s.x) * static_cast<std::size_t>(bpp);
        const std::uint8_t* src = rep.data() + static_cast<std::size_t>(repY) * repStride + static_cast<std::size_t>(repX) * static_cast<std::size_t>(bpp);
        if (analyzeAlpha(src, format, repStride, copyW, copyH) == Alpha::Empty) continue;
        const bool blend = analyzeAlpha(dst, format, dstStride, copyW, copyH) == Alpha::Opaque;
        for (int row = 0; row < copyH; ++row) {
            std::uint8_t* d = dst + static_cast<std::size_t>(row) * dstStride;
            const std::uint8_t* r = src + static_cast<std::size_t>(row) * repStride;
            if (blend) blendRow(d, r, copyW, format);
            else std::memcpy(d, r, static_cast<std::size_t>(copyW) * static_cast<std::size_t>(bpp));
        }
        any = true;
    }
    return any;
}

// Adapted from thcrap's patch_anm() and stack_game_png_apply(): for each entry with a THTX
// texture, the PNG named like the entry's texture ("data/title/select01.png", or thtk's
// "data/title/select01@title01@2.png" first) from every patch of the stack in order.
bool English::patchAnm(std::vector<std::uint8_t>& anm, const std::string& anmName) const {
    std::lock_guard lock(mutex_);
    bool changed = false;
    std::size_t at = 0;
    const std::string anmStem = anmName.substr(0, anmName.rfind('.'));
    for (int entry = 0; at + 0x40 <= anm.size(); ++entry) {
        std::uint8_t* h = anm.data() + at;
        const int spriteCount = i32(h), nominalW = i32(h + 0x0C), nominalH = i32(h + 0x10);
        const int nameOff = i32(h + 0x1C), next = i32(h + 0x38);
        const int entryX = i32(h + 0x20), entryY = i32(h + 0x24);
        std::string texName;
        if (nameOff > 0) {
            for (std::size_t i = at + static_cast<std::size_t>(nameOff); i < anm.size() && anm[i]; ++i) texName.push_back(static_cast<char>(anm[i]));
        }
        const std::size_t thtx = at + static_cast<std::size_t>(std::max(0, i32(h + 0x30)));
        if (h[0x34] != 0 && !texName.empty() && texName[0] != '@' && thtx + 0x10 <= anm.size() && spriteCount >= 0 &&
            at + 0x40 + 4ull * static_cast<unsigned>(spriteCount) <= anm.size()) {
            const int format = i16(anm.data() + thtx + 6), w = i16(anm.data() + thtx + 8), hh = i16(anm.data() + thtx + 10);
            const int bpp = bytesPerPixel(format);
            const std::size_t need = static_cast<std::size_t>(std::max(0, w)) * static_cast<std::size_t>(std::max(0, hh)) * static_cast<std::size_t>(bpp);
            if (bpp != 0 && w == nominalW && hh == nominalH && thtx + 0x10 + need <= anm.size()) {
                std::vector<Rect> sprites;
                for (int i = 0; i < spriteCount; ++i) {
                    const std::size_t rec = at + static_cast<std::size_t>(i32(h + 0x40 + 4 * i));
                    if (rec + 20 > anm.size()) continue;
                    sprites.push_back({f32(anm.data() + rec + 4), f32(anm.data() + rec + 8), f32(anm.data() + rec + 12), f32(anm.data() + rec + 16)});
                }
                const std::string lowerName = lower(texName);
                const std::size_t slash = lowerName.rfind('/');
                const std::size_t dot = lowerName.rfind('.');
                std::string thtk;
                if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
                    thtk = std::format("{}@{}@{}{}", lowerName.substr(0, dot), lower(anmStem), entry, lowerName.substr(dot));
                }
                for (const std::string& candidate : {thtk, lowerName}) {
                    if (candidate.empty()) continue;
                    bool found = false;
                    for (const std::string& file : resolveAll("th08/" + candidate)) {
                        const auto bytes = readFile(file);
                        std::string why;
                        const auto png = bytes ? decodeImage(*bytes, &why) : std::nullopt;
                        if (!png) {
                            log::warn("English text: {}: {}", file, why);
                            continue;
                        }
                        blitPng(anm.data() + thtx + 0x10, format, w, hh, sprites, *png, entryX, entryY);
                        if (logged_.insert(file).second) log::info("English text: {} over {} ({})", file, texName, anmName);
                        found = changed = true;
                    }
                    if (found) break;
                }
            }
        }
        if (next <= 0) break;
        at += static_cast<std::size_t>(next);
    }
    return changed;
}

// ---- lookups ---------------------------------------------------------------------------------------

std::optional<std::string> English::spellName(int card) const {
    std::lock_guard lock(mutex_);
    const Json* j = json("th08/spells.js");
    const Json* v = j ? j->find(std::to_string(card)) : nullptr;
    if (!v || !v->isString()) return std::nullopt;
    return std::string(dropNote(v->string()));
}

std::optional<English::SpellComment> English::spellComment(int card) const {
    std::lock_guard lock(mutex_);
    const Json* j = json("th08/spellcomments.js");
    const Json* v = j ? j->find(std::to_string(card)) : nullptr;
    if (!v || !v->isObject()) return std::nullopt;
    SpellComment out;
    if (const Json* o = v->find("owner"); o && o->isString()) out.owner = std::string(dropNote(o->string()));
    if (const Json* l = v->find("comment_1"); l && l->isArray()) {
        for (const Json& line : l->array()) {
            if (line.isString()) out.lines.emplace_back(dropNote(line.string()));
        }
    }
    return out;
}

std::optional<std::string> English::musicTitle(int track) const {
    std::lock_guard lock(mutex_);
    const Json* j = json("themes.js");
    const Json* v = j ? j->find(std::format("th08_{:02d}", track)) : nullptr;
    if (!v || !v->isString()) return std::nullopt;
    return std::string(dropNote(v->string()));
}

std::optional<std::string> English::stringDef(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const Json* j = json("stringdefs.js");
    const Json* v = j ? j->find(id) : nullptr;
    if (!v || !v->isString()) return std::nullopt;
    return v->string();
}

std::optional<std::vector<std::optional<std::string>>> English::musicComment(int track) const {
    std::lock_guard lock(mutex_);
    const Json* j = json("th08/musiccmt.js");
    const Json* v = j ? j->find(std::to_string(track)) : nullptr;
    if (!v || !v->isArray()) return std::nullopt;
    std::vector<std::optional<std::string>> lines;
    for (const Json& l : v->array()) {
        if (!l.isString()) {
            lines.emplace_back(std::string());
        } else if (l.string() == "@") {
            // thcrap's music_cmt: "@" is the numbered title, when the title has a translation.
            const auto title = musicTitle(track);
            const std::string format = stringDef("Music Room Numbered Title").value_or("No.%2d %s");
            if (title) lines.emplace_back(formatPatchString(format, track, *title));
            else lines.emplace_back(std::nullopt);
        } else {
            lines.emplace_back(std::string(dropNote(l.string())));
        }
    }
    return lines;
}

std::optional<std::string> English::exeString(std::uint32_t address) const {
    std::lock_guard lock(mutex_);
    const Json* locs = json("th08/stringlocs.js");
    if (!locs || !locs->isObject() || address < 0x400000) return std::nullopt;
    // Keys are "Rx" and the address relative to the image base, in either case.
    const std::string want = std::format("rx{:x}", address - 0x400000);
    for (const auto& [key, id] : locs->object()) {
        if (lower(key) == want && id.isString()) {
            const auto def = stringDef(id.string());
            if (def) return std::string(dropNote(*def));
        }
    }
    return std::nullopt;
}

// ---- layout ----------------------------------------------------------------------------------------

namespace {

struct Token {
    bool command = false;
    std::vector<std::string> params;  // command: {letters, text, measure...}; text: {text}
};

// thcrap's layout_match() and layout_tokenize().
std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::string_view cur = s.substr(i);
        std::size_t curLen = cur.size();
        std::vector<std::string> match;
        if (cur[0] == '<') {
            std::size_t start = 1;
            int nest = 0;
            std::size_t p = 1;
            for (; p < cur.size() && nest >= 0; ++p) {
                nest += cur[p] == '<';
                nest -= cur[p] == '>';
                if ((nest == 0 && cur[p] == '$') || (nest == -1 && cur[p] == '>')) {
                    match.emplace_back(cur.substr(start, p - start));
                    start = p + 1;
                }
            }
            curLen = start;
        }
        if (match.size() > 1) {
            out.push_back({true, std::move(match)});
        } else {
            const std::size_t next = cur.find('<', 1);
            if (next != std::string_view::npos && next < curLen) curLen = next;
            if (curLen == 0) curLen = cur.size();
            out.push_back({false, {std::string(cur.substr(0, curLen))}});
        }
        i += curLen;
    }
    return out;
}

}  // namespace

std::vector<TextRun> layoutMarkup(std::string_view line, const std::function<float(std::string_view, bool)>& measure, float bandWidth,
                                  float* total) {
    std::vector<TextRun> runs;
    const std::vector<Token> tokens = tokenize(line);
    std::vector<float> tabs;
    std::size_t curTab = 0;
    float curX = 0.0f, extent = 0.0f;
    for (std::size_t t = 0; t < tokens.size(); ++t) {
        const Token& tok = tokens[t];
        bool draw = true;
        bool italic = false;
        std::string text;
        float curW = 0.0f;
        if (tok.command) {
            const std::string& cmd = tok.params[0];
            text = tok.params[1];
            italic = cmd.find('i') != std::string::npos;
            curW = measure(text, italic);
            // layout_parse_tabs().
            float tabEnd;
            if (tok.params.size() > 2) {
                tabEnd = tok.params[2].empty() ? bandWidth : curX + measure(tok.params[2], italic);
            } else if (curTab < tabs.size()) {
                tabEnd = tabs[curTab];
            } else if (!tabs.empty() && t + 1 == tokens.size()) {
                tabEnd = bandWidth;
            } else {
                tabEnd = curX + curW;
            }
            int placed = static_cast<int>(cmd.size());
            for (char c : cmd) {
                switch (c) {
                    case 's':  // measured, not drawn
                        tabEnd = curX;
                        draw = false;
                        break;
                    case 't': {  // a tab stop as wide as the widest of the parameters
                        float w = curW;
                        for (std::size_t k = 2; k < tok.params.size(); ++k) w = std::max(w, measure(tok.params[k], italic));
                        tabEnd = curX + w;
                        if (tabs.size() <= curTab) tabs.resize(curTab + 1, 0.0f);
                        tabs[curTab] = tabEnd;
                        break;
                    }
                    case 'l': break;
                    case 'c': curX += (tabEnd - curX) / 2.0f - curW / 2.0f; break;
                    case 'r': curX = tabEnd - curW; break;
                    default: --placed; break;  // b, i, u: style only
                }
            }
            if (placed > 0) {
                ++curTab;
                if (draw && !text.empty()) runs.push_back({text, curX, measure(text, italic), italic});
                curW = tabEnd - curX;
            } else if (draw && !text.empty()) {
                runs.push_back({text, curX, curW, italic});
            }
        } else {
            text = tok.params[0];
            curW = measure(text, false);
            if (!text.empty()) runs.push_back({text, curX, curW, false});
        }
        curX += curW;
        extent = std::max(extent, curX);
    }
    for (const TextRun& r : runs) extent = std::max(extent, r.x + r.width);
    if (total) *total = extent;
    return runs;
}

float renderTranslated(TextRenderer& renderer, std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                       Image& band) {
    if (utf8.empty() || band.empty() || width <= 0.0f) return 0.0f;
    if (utf8.find('<') == std::string_view::npos) return renderer.renderUtf8(utf8, style, left, width, align, band);
    float total = 0.0f;
    const auto runs = layoutMarkup(
        utf8,
        [&](std::string_view s, bool italic) {
            TextStyle st = style;
            st.italic = italic;
            return renderer.measureUtf8(s, st);
        },
        width, &total);
    if (runs.empty() || total <= 0.0f) return 0.0f;
    if (runs.size() == 1 && runs[0].x <= 0.0f && !runs[0].italic) return renderer.renderUtf8(runs[0].text, style, left, width, align, band);
    const float k = total > width ? width / total : 1.0f;  // the whole line narrowed to fit
    const float drawn = total * k;
    const float x0 = align == TextAlign::Right ? left + width - drawn : align == TextAlign::Centre ? left + (width - drawn) / 2.0f : left;
    Image piece(band.width, band.height);
    for (const TextRun& r : runs) {
        std::fill(piece.rgba.begin(), piece.rgba.end(), 0);
        TextStyle st = style;
        st.italic = r.italic;
        const float room = k < 1.0f ? r.width * k : r.width + 0.5f;
        renderer.renderUtf8(r.text, st, x0 + r.x * k, std::max(room, 1.0f), TextAlign::Left, piece);
        for (std::size_t i = 0; i + 3 < band.rgba.size(); i += 4) {
            if (piece.rgba[i + 3] > band.rgba[i + 3]) std::memcpy(&band.rgba[i], &piece.rgba[i], 4);
        }
    }
    return drawn;
}

std::vector<std::string> wrapText(std::string_view text, float width, const std::function<float(std::string_view)>& measure) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::string_view para = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        std::string line;
        std::size_t i = 0;
        while (i < para.size()) {
            const std::size_t sp = para.find(' ', i);
            const std::string_view word = para.substr(i, sp == std::string_view::npos ? std::string_view::npos : sp - i);
            i = sp == std::string_view::npos ? para.size() : sp + 1;
            if (word.empty()) continue;
            const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
            if (!line.empty() && measure(candidate) > width) {
                out.push_back(line);
                line = std::string(word);
            } else {
                line = candidate;
            }
        }
        if (!line.empty()) out.push_back(line);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string formatPatchString(std::string_view format, int number, std::string_view text) {
    std::string out;
    bool usedNumber = false, usedText = false;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out.push_back(format[i]);
            continue;
        }
        std::size_t j = i + 1;
        if (j < format.size() && format[j] == '%') {
            out.push_back('%');
            i = j;
            continue;
        }
        bool leftAlign = false, zero = false;
        while (j < format.size() && (format[j] == '-' || format[j] == '0')) {
            leftAlign |= format[j] == '-';
            zero |= format[j] == '0';
            ++j;
        }
        int widthField = 0, precision = -1;
        while (j < format.size() && format[j] >= '0' && format[j] <= '9') widthField = widthField * 10 + (format[j++] - '0');
        if (j < format.size() && format[j] == '.') {
            precision = 0;
            ++j;
            while (j < format.size() && format[j] >= '0' && format[j] <= '9') precision = precision * 10 + (format[j++] - '0');
        }
        if (j >= format.size()) break;
        std::string piece;
        const char conv = format[j];
        if (conv == 'd' || conv == 'i' || conv == 'u') {
            piece = std::to_string(usedNumber ? 0 : number);
            usedNumber = true;
            if (precision > 0 && static_cast<int>(piece.size()) < precision) piece.insert(0, static_cast<std::size_t>(precision) - piece.size(), '0');
        } else if (conv == 's') {
            piece = usedText ? std::string() : std::string(text);
            usedText = true;
        }
        if (static_cast<int>(piece.size()) < widthField) {
            const std::size_t pad = static_cast<std::size_t>(widthField) - piece.size();
            if (leftAlign) piece.append(pad, ' ');
            else piece.insert(0, pad, zero && conv != 's' ? '0' : ' ');
        }
        out += piece;
        i = j;
    }
    return out;
}

}  // namespace touhou::th08
