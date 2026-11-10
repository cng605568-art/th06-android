#include "th08/Ending.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bgm.hpp"
#include "th08/ScoreFile.hpp"

namespace touhou::th08 {

namespace {
constexpr int kTextScript = 18;       // text.anm scripts 18-32 (sprites 21-35)
constexpr std::size_t kLineMax = 66;  // the original's 68-byte buffer
bool isSpace(std::uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
}  // namespace

Ending::~Ending() { release(); }

void Ending::release() {
    if (staffLoaded_ && ctx_.anm) ctx_.anm->release(kSlotStaff);
    staffLoaded_ = false;
    background_.release();
}

bool Ending::create(const Run& run, ScoreData* records, std::string* error) {
    AnmLibrary& lib = *ctx_.anm;
    const int team = run.character < 4 ? std::clamp(run.character, 0, 3) : (run.character - 4) / 2;
    const auto d = static_cast<std::size_t>(std::clamp(run.difficulty, 0, 4));
    // The records (spec 7.1 steps 2-3).
    if (records) {
        ClearRecord& mine = records->clears[static_cast<std::size_t>(std::clamp(run.character, 0, 11))];
        if (run.finished) {
            const std::uint16_t bit = run.stage == 7 ? 0x4000 : 0x8000;
            records->plstAdd(static_cast<int>(d), ScoreData::Clears);
            records->plstAdd(6, ScoreData::Clears);
            // "Unlock already granted", before this run's bits: the staff roll's fast-forward.
            unlockGranted_ = run.stage == 7 ? records->extraUnlocked() : records->spellPracticeUnlocked();
            const auto& words = run.continued ? mine.reached : mine.noContinue;
            seen_ = false;
            for (int k = 0; k < 4; ++k) seen_ = seen_ || (words[static_cast<std::size_t>(k)] & bit) != 0;
            records->markClear(run.character, run.difficulty, bit, !run.continued);
            records->musicHeard[18] = records->musicHeard[19] = 1;
        } else {
            seen_ = mine.pending != 0;  // (nothing ever sets it: bad endings are never skippable [QUIRK])
            mine.pending = 0;
            records->musicHeard[18] = 0x12;
        }
    }
    std::string err;
    staffLoaded_ = ctx_.vfs && ctx_.textures && lib.load(*ctx_.vfs, *ctx_.textures, "staff01.anm", kSlotStaff, &err);
    if (!staffLoaded_) log::warn("Ending: {}", err);
    for (int i = 0; i < 15; ++i) {
        AnmVm& v = vm_[static_cast<std::size_t>(i)];
        v = AnmVm{};
        v.startFresh(lib, *ctx_.rng, kSlotText, kTextScript + i);
        v.position = {64.0f, 400.0f + 16.0f * static_cast<float>(i), 0.0f};
    }
    vm_[15] = AnmVm{};
    const char kind = !run.finished ? 'a' : run.stage == 7 ? 'c' : 'b';
    const std::string path = std::format("end0{}{}.end", team, kind);
    if (!loadScript(path)) {
        // (The original quits the program; the port goes on to the result screen.)
        if (error) *error = path + " could not be read";
        finish();
        return false;
    }
    log::info("th08: ending {} ({})", path, seen_ ? "seen before" : "first time");
    return true;
}

bool Ending::startScript(std::vector<std::uint8_t> bytes, const std::string& path) {
    data_ = std::move(bytes);
    path_ = path;
    pos_ = 0;
    normalDelay_ = 8;
    wait_ = 0;
    steps_ = 0;
    done_ = false;
    return true;
}

bool Ending::loadScript(const std::string& path) {
    const auto bytes = ctx_.vfs ? ctx_.vfs->read(path) : std::nullopt;
    if (!bytes) return false;
    return startScript(*bytes, path);
}

void Ending::finish() {
    if (done_) return;
    done_ = true;
    release();
    if (ctx_.request) ctx_.request(Scene::ResultAfterGame);
}

// A decimal number as the C library reads it, then everything up to the next NUL and every
// NUL after it (arguments are NUL-separated in the shipped files).
std::int32_t Ending::readNumber() {
    std::size_t p = pos_;
    while (p < data_.size() && isSpace(data_[p])) ++p;
    bool negative = false;
    if (p < data_.size() && (data_[p] == '-' || data_[p] == '+')) negative = data_[p++] == '-';
    std::int64_t v = 0;
    while (p < data_.size() && data_[p] >= '0' && data_[p] <= '9') {
        v = std::min<std::int64_t>(v * 10 + (data_[p] - '0'), 0x7FFFFFFF);
        ++p;
    }
    while (p < data_.size() && data_[p] != 0) ++p;
    while (p < data_.size() && data_[p] == 0) ++p;
    pos_ = p;
    return static_cast<std::int32_t>(negative ? -v : v);
}

std::string Ending::readString() {
    std::string s;
    while (pos_ < data_.size() && data_[pos_] != 0) s.push_back(static_cast<char>(data_[pos_++]));
    if (pos_ < data_.size()) ++pos_;
    return s;
}

void Ending::skipLine() {
    while (pos_ < data_.size() && data_[pos_] != '\r' && data_[pos_] != '\n') ++pos_;
    while (pos_ < data_.size() && (data_[pos_] == '\r' || data_[pos_] == '\n')) ++pos_;
}

// One command at pos_ ('@' and a letter). False: the ending is over.
bool Ending::command(bool& endStep) {
    AnmLibrary& lib = *ctx_.anm;
    const std::uint8_t letter = pos_ + 1 < data_.size() ? data_[pos_ + 1] : 'z';
    pos_ += 2;
    switch (letter) {
        case 'b': {
            const std::string path = readString();
            if (!ctx_.vfs || !ctx_.textures || !background_.load(*ctx_.vfs, *ctx_.textures, path)) {
                log::warn("Ending: picture {} missing", path);
                return false;
            }
            break;
        }
        case 'a': {  // staff01.anm script and sprite, file-local
            const int vm = readNumber(), script = readNumber(), sprite = readNumber();
            if (vm >= 0 && vm < kVms && staffLoaded_) {
                AnmVm& v = vm_[static_cast<std::size_t>(vm)];
                v.startFresh(lib, *ctx_.rng, kSlotStaff, script);
                v.selectSprite(sprite);
            }
            break;
        }
        case 'V': {
            const int distance = readNumber(), duration = readNumber();
            scroll_ = duration != 0 ? static_cast<float>(distance) / static_cast<float>(duration)
                                    : (distance > 0 ? 1e30f : distance < 0 ? -1e30f : 0.0f);
            break;
        }
        case 'v': top_ = static_cast<float>(readNumber()); break;
        case 'F': {
            const std::string path = readString();
            if (!loadScript(path)) {
                log::warn("Ending: staff roll {} missing", path);
                return false;
            }
            seen_ = unlockGranted_;  // (TH08: the unlock this ending grants was already there)
            for (AnmVm& v : vm_) v.scriptIndex = 0;  // @R
            break;  // the line skip below happens in the new file [QUIRK]
        }
        case 'R':
            for (AnmVm& v : vm_) v.scriptIndex = 0;
            break;
        case 'm': {
            const std::string path = readString();
            if (ctx_.music) {
                ctx_.music->load(0, path);
                ctx_.music->playLoaded(0);
            }
            ctx_.notePlayed(path);
            break;
        }
        case 'M': {
            const int seconds = readNumber();
            if (ctx_.music) ctx_.music->fadeOut(static_cast<float>(seconds));
            break;
        }
        case 's':
            normalDelay_ = readNumber();
            heldDelay_ = readNumber();
            break;
        case 'c': color_ = static_cast<std::uint32_t>(readNumber()); break;
        case 'r':
            clear_ = readNumber();
            clearMin_ = readNumber();
            wait_ = waitMin_ = 0;  // (explicit in TH08)
            endStep = true;
            break;
        case 'w':
            wait_ = readNumber();
            waitMin_ = readNumber();
            endStep = true;
            break;
        case '0':
        case '1':
        case '2':
        case '3':
            fadeN_ = readNumber();
            fadeType_ = letter - '0';
            fadeT_ = 0;
            break;
        case 'z': return false;
        default: break;
    }
    skipLine();
    return true;
}

bool Ending::step(const MenuInput& in) {
    const bool advance = in.pressed(BtnShoot | BtnEnter);
    const bool skip = seen_ && in.held(BtnSkip);
    bool parse = true;
    if (clear_ > 0) {
        --clear_;
        if (clearMin_ != 0) --clearMin_;
        else if (advance || skip) clear_ = 0;
        if (clear_ <= 0) {
            for (int i = 0; i < 15; ++i) vm_[static_cast<std::size_t>(i)].interrupt(2);
            lineSlot_ = 0;
        } else {
            parse = false;
        }
    }
    if (parse && wait_ > 0) {
        --wait_;
        if (waitMin_ != 0) --waitMin_;
        else if (advance || skip) wait_ = 0;
        parse = false;  // parsing resumes next step, even at 0 [QUIRK]
    }
    while (parse) {
        if (pos_ >= data_.size()) return false;
        if (data_[pos_] == '@') {
            bool endStep = false;
            if (!command(endStep)) return false;
            if (endStep) break;
            continue;
        }
        // A text line, two bytes (one Shift-JIS character) at a time.
        std::string line;
        while (pos_ < data_.size() && data_[pos_] != 0 && data_[pos_] != '\r' && data_[pos_] != '\n') {
            for (int k = 0; k < 2 && pos_ < data_.size(); ++k, ++pos_) {
                if (line.size() < kLineMax) line.push_back(static_cast<char>(data_[pos_]));
            }
        }
        if (!line.empty() && lineSlot_ < kVms) {
            AnmVm& v = vm_[static_cast<std::size_t>(lineSlot_)];
            renderSpriteText(ctx_.textures, ctx_.text, v, line, color_, TextAlign::Left);
            v.interrupt(1);
            lastLine_ = line;
        }
        while (pos_ < data_.size() && (data_[pos_] == 0 || data_[pos_] == '\r' || data_[pos_] == '\n')) ++pos_;
        // The pause: the held delay while Shoot or Enter is held, else the normal one; a press
        // can cut it once the held delay has passed (TH08).
        wait_ = in.held(BtnShoot | BtnEnter) ? heldDelay_ : normalDelay_;
        waitMin_ = heldDelay_;
        if (lineSlot_ < kVms) ++lineSlot_;
        break;
    }
    ++steps_;
    top_ -= scroll_;
    if (top_ <= 0.0f) {
        top_ = 0.0f;
        scroll_ = 0.0f;
    }
    return true;
}

void Ending::update(const MenuInput& in) {
    if (done_) return;
    for (int k = 0; k < 9; ++k) {  // (up to 8 extra steps while skipping; TH07 4)
        if (!step(in)) {
            finish();
            return;
        }
        for (int i = 0; i < 15; ++i) vm_[static_cast<std::size_t>(i)].tick(*ctx_.rng);
        if (!(seen_ && in.held(BtnSkip))) break;
    }
    // The fade (the original advances it while drawing).
    if (fadeType_ >= 0) {
        const bool white = fadeType_ >= 2, toColour = (fadeType_ & 1) != 0;
        int a;
        if (fadeT_ < fadeN_) {
            a = toColour ? fadeT_ * 255 / fadeN_ : 255 - fadeT_ * 255 / fadeN_;
            ++fadeT_;
        } else {
            a = toColour ? 255 : 0;
        }
        overlay_ = (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24) | (white ? 0xFFFFFFu : 0u);
    }
}

void Ending::draw(SpriteSink& sink) {
    if (done_) return;
    if (background_.loaded()) background_.blitRect(sink, 0.0f, 0.0f, 0.0f, std::trunc(top_), 640.0f, 480.0f);
    const AnmDrawState none;
    for (int i = 0; i < 15; ++i) drawVm(sink, vm_[static_cast<std::size_t>(i)], none);
    fillScreen(sink, ctx_.ui, overlay_);
}

}  // namespace touhou::th08
