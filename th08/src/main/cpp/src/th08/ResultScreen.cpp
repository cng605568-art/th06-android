#include "th08/ResultScreen.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/ExeTables.hpp"
#include "th08/GameConfig.hpp"

namespace touhou::th08 {

// The name grid (spec 3.4: the exe's 96-character table, in this order).
const char ResultScreen::kKeyTable[kKeys + 1] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ.,:;_@"
    "abcdefghijklmnopqrstuvwxyz+-/*=%"
    "0123456789#!?'\"$(){}[]<>&\\|~^   ";

namespace {


std::string todayDate() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02d}/{:02d}", tm.tm_mon + 1, tm.tm_mday);
}

void put32(HighScoreRecord& r, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) r[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
}
std::uint32_t get32(const HighScoreRecord& r, std::size_t at) {
    return static_cast<std::uint32_t>(r[at]) | static_cast<std::uint32_t>(r[at + 1]) << 8 | static_cast<std::uint32_t>(r[at + 2]) << 16 |
           static_cast<std::uint32_t>(r[at + 3]) << 24;
}

// printf with a Shift-JIS format from the exe (its trail bytes are never '%').
template <typename... A>
std::string sjisFormat(const std::string& fmt, A... args) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof buf, fmt.c_str(), args...);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(std::min(n, 255))) : std::string();
}

std::string cString(const std::uint8_t* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

// The stage number a row shows (table 0x4C7F80: Extra shows 1 [QUIRK]).
constexpr int kStageShown[9] = {1, 2, 3, 4, 4, 5, 6, 6, 1};
constexpr const char* kDifficultyNames[5] = {"      Easy", "    Normal", "      Hard", "   Lunatic", "     Extra"};

}  // namespace

ResultScreen::~ResultScreen() {
    if (loaded_ && ctx_.anm) {
        ctx_.anm->release(kSlotResult);
        ctx_.anm->release(kSlotResultText);
    }
}

void ResultScreen::setState(State s) {
    state_ = s;
    pass_ = 0;
    sub_ = subPass_ = 0;
    leaving_ = false;
}

void ResultScreen::sendAll(int interrupt) {
    for (AnmVm& v : vm_) v.interrupt(interrupt);
}

HighScoreRecord* ResultScreen::runRecord() {
    for (HighScoreRecord& r : records_.highScores) {
        if (r[9] == 1) return &r;
    }
    return nullptr;
}

bool ResultScreen::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures || !ctx_.rng) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx_.anm;
    background_.load(*ctx_.vfs, *ctx_.textures, "result.jpg");
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "result00.anm", kSlotResult, error)) return false;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "resulttext.anm", kSlotResultText, error)) return false;
    loaded_ = true;
    for (int i = 0; i < kVms; ++i) {
        AnmVm& v = vm_[static_cast<std::size_t>(i)];
        v = AnmVm{};
        v.startFresh(lib, *ctx_.rng, kSlotResult, i);
    }
    // The text VMs: 0-13 text.anm sprites 21-34, 14-27 resulttext.anm sprites 2-15, anchored
    // top-left, font 15 x 15; no scripts. The spell list's bar: result00 sprite 32.
    for (int i = 0; i < kTextVms; ++i) {
        AnmVm& t = text_[static_cast<std::size_t>(i)];
        t = AnmVm{};
        if (i < 14) t.initialiseWithSprite(lib, kSlotText, 21 + i);
        else if (i < 28) t.initialiseWithSprite(lib, kSlotResultText, 2 + (i - 14));
        t.flags |= 0x1800u;
        t.fontWidth = t.fontHeight = 15;
    }
    bar_ = AnmVm{};
    bar_.initialiseWithSprite(lib, kSlotResult, 32);
    // The captured-card counts per category and column (step 9).
    const ExeTables* tables = ctx_.tables;
    for (std::size_t j = 0; j < 6; ++j) {
        counts_[j].fill(0);
        if (!tables) continue;
        for (const std::int32_t c : tables->cardCategories[j]) {
            if (c < 0 || c >= kCardCount) continue;
            const CardRecord& r = records_.cards[static_cast<std::size_t>(c)];
            if (!r.present) continue;
            for (std::size_t k = 0; k < 13; ++k) counts_[j][k] += (r.captures[k] != 0 || r.practiceCaptures[k] != 0) ? 1 : 0;
        }
    }
    // The PLST totals (step 10): row 6's clears and continues are the sums of rows 0-4.
    for (const int field : {static_cast<int>(ScoreData::Clears), static_cast<int>(ScoreData::Continues)}) {
        std::uint32_t sum = 0;
        for (int row = 0; row < 5; ++row) sum += records_.plstValue(row, field);
        records_.plstAdd(6, field, sum - records_.plstValue(6, field));
    }
    difficulty_ = std::clamp(game_.difficulty, 0, 4);
    character_ = std::clamp(game_.character, 0, 11);
    shownCharacter_ = character_;
    if (mode_ == Mode::Viewer) {
        difficulty_ = 1;  // (remembered: Normal, the first character)
        shownCharacter_ = character_ = 0;
        setState(State::Menu);
        return true;
    }
    // The LSNM name (8 spaces when the file has none).
    if (records_.hasLastName) std::memcpy(name_.data(), records_.lastName.data(), 8);
    else std::memset(name_.data(), ' ', 8);
    name_[8] = 0;
    for (int k = 0; k < 8; ++k) name_[static_cast<std::size_t>(k)] = name_[static_cast<std::size_t>(k)] == 0 ? ' ' : name_[static_cast<std::size_t>(k)];
    if (mode_ == Mode::PracticeEnd) {
        // The practice best (spec 1.3 step 7), then the replay prompt: not ported, so leave.
        const auto slot = static_cast<std::size_t>(game_.stage * 5 + difficulty_);
        PracticeRecord& p = records_.practice[static_cast<std::size_t>(character_)];
        if (slot < p.best.size() && p.best[slot] < game_.score) {
            p.best[slot] = game_.score;
            p.hasData = true;
        }
        setState(State::Leave);
    } else if (mode_ == Mode::SpellPracticeEnd) {
        setState(State::Leave);  // (no PSCR update; the cards were counted in play)
    } else {
        setState(State::NameEntry);
    }
    return true;
}

// ---- after a game (spec 3) ----------------------------------------------------------------------

void ResultScreen::nameEntry(const MenuInput& in) {
    if (pass_ == 0) {
        // The gate: slow mode makes no entry (spec 3.1).
        if (ctx_.config && ctx_.config->slowMode != 0) {
            setState(State::Statistics);
            return;
        }
        sendAll(difficulty_ + 3);
        // The entry (spec 3.2-3.3).
        HighScoreRecord r{};
        std::memcpy(r.data(), "HSCR", 4);
        r[4] = r[6] = 0x68;
        r[5] = r[7] = 0x01;
        r[8] = 4;
        r[9] = 1;  // this run
        put32(r, 0x0C, game_.score);
        r[0x14] = static_cast<std::uint8_t>(character_);
        r[0x15] = static_cast<std::uint8_t>(difficulty_);
        r[0x16] = static_cast<std::uint8_t>(game_.finished ? 99 : std::clamp(game_.stage, 0, 8));
        std::memcpy(r.data() + 0x17, name_.data(), 8);
        const std::string date = todayDate();
        std::memcpy(r.data() + 0x20, date.data(), std::min<std::size_t>(date.size(), 5));
        r[0x26] = static_cast<std::uint8_t>(std::min(game_.continues, 127));
        if (ctx_.config) {
            const std::vector<std::uint8_t> cfg = ctx_.config->save();
            std::memcpy(r.data() + 0x28, cfg.data(), std::min<std::size_t>(cfg.size(), 60));
        }
        put32(r, 0x64, game_.runFrames);
        put32(r, 0x68, game_.runPointItems);
        put32(r, 0x70, static_cast<std::uint32_t>(game_.misses));
        put32(r, 0x74, static_cast<std::uint32_t>(game_.bombsUsedGame));
        put32(r, 0x78, static_cast<std::uint32_t>(game_.lastSpellsUsed));
        put32(r, 0x7C, game_.pauses);
        put32(r, 0x80, static_cast<std::uint32_t>(game_.timeOrbsNet));
        const std::int32_t rate = game_.countedFrames != 0
                                      ? static_cast<std::int32_t>(static_cast<float>(game_.humanFrames) / static_cast<float>(game_.countedFrames) * 10000.0f)
                                      : static_cast<std::int32_t>(0x80000000u);  // (integer-indefinite [QUIRK])
        put32(r, 0x84, static_cast<std::uint32_t>(rate));
        std::memcpy(r.data() + 0x88, game_.runCaptures.data(), game_.runCaptures.size());
        rank_ = records_.rankOf(difficulty_, character_, game_.score);
        records_.addHighScore(r);
        if (rank_ >= 10) {  // not qualified: straight to the statistics
            rank_ = -1;
            finishName();
            return;
        }
        nameCursor_ = 0;
        keyCursor_ = records_.hasLastName ? kKeyEnd : 0;
        return;
    }
    if (pass_ < 10) return;
    auto step = [&](int delta, bool inRow) {
        for (int guard = 0; guard < kKeys; ++guard) {
            if (inRow) {
                const int row = keyCursor_ / 16, col = ((keyCursor_ % 16 + delta) % 16 + 16) % 16;
                keyCursor_ = row * 16 + col;
            } else {
                keyCursor_ = ((keyCursor_ + delta) % kKeys + kKeys) % kKeys;
            }
            if (keyCursor_ != kKeySpace) break;
        }
        ctx_.playSound(kSndMove);
    };
    if (in.pressedOrRepeat(BtnUp)) step(-16, false);
    else if (in.pressedOrRepeat(BtnDown)) step(16, false);
    else if (in.pressedOrRepeat(BtnLeft)) step(-1, true);
    else if (in.pressedOrRepeat(BtnRight)) step(1, true);
    if (in.pressedOrRepeat(kSelect)) {
        const int at = std::min(nameCursor_, 7);
        if (keyCursor_ == kKeyEnd) {
            ctx_.playSound(kSndBack);
            finishName();
            return;
        }
        name_[static_cast<std::size_t>(at)] = keyCursor_ == kKeySp ? ' ' : kKeyTable[keyCursor_];
        if (nameCursor_ < 8 && ++nameCursor_ == 8) keyCursor_ = kKeyEnd;
        ctx_.playSound(kSndSelect);
    }
    if (in.pressedOrRepeat(kCancel) && nameCursor_ > 0) {
        name_[static_cast<std::size_t>(std::min(nameCursor_, 7))] = ' ';
        --nameCursor_;
        name_[static_cast<std::size_t>(nameCursor_)] = ' ';
        ctx_.playSound(kSndBack);
    }
    if (HighScoreRecord* r = runRecord()) std::memcpy(r->data() + 0x17, name_.data(), 8);
    if (in.pressed(BtnMenu)) {
        ctx_.playSound(kSndBack);
        finishName();  // Esc backspaces, then finishes [QUIRK]
    }
}

void ResultScreen::finishName() {
    sendAll(2);
    if (HighScoreRecord* r = runRecord()) {
        std::memcpy(r->data() + 0x17, name_.data(), 8);
        (*r)[9] = 0;
    }
    // The name becomes the remembered one (LSNM), saved when the screen leaves.
    std::memcpy(records_.lastName.data(), name_.data(), 8);
    records_.lastName[8] = 0;
    records_.hasLastName = true;
    setState(State::Statistics);
}

void ResultScreen::update(const MenuInput& in) {
    if (done_) return;
    switch (state_) {
        case State::Menu: menu(in); break;
        case State::Difficulty:
        case State::Character:
        case State::SpellCategory:
        case State::SpellColumn: rowsPage(in); break;
        case State::Table: table(in); break;
        case State::SpellList: spellList(in); break;
        case State::DataIn:
        case State::DataShown:
        case State::DataOut: playData(in); break;
        case State::NameEntry: nameEntry(in); break;
        case State::Statistics:
            if (pass_ <= 30) vm_[71].interrupt(18);
            if (pass_ >= 90 && in.pressed(kSelect)) {
                vm_[71].interrupt(2);
                setState(State::StatisticsOut);
            }
            break;
        case State::StatisticsOut:
            if (pass_ >= 30) setState(State::Leave);  // (the replay prompt: not ported)
            break;
        case State::Leave:
            if (pass_ == 1) sendAll(2);
            if (pass_ >= 20) {
                done_ = true;
                if (ctx_.saveScores) ctx_.saveScores(true);
                if (ctx_.request) ctx_.request(Scene::MainMenu);
                return;
            }
            break;
    }
    for (AnmVm& v : vm_) v.tick(*ctx_.rng);
    ++pass_;
}

// ---- the viewer (spec 2) -------------------------------------------------------------------------

// A page's rows on entry: 22 and one VM step at once, then 20 for the cursor row, 21 for the rest.
void ResultScreen::enterRows(int first, int count, int cursor) {
    for (int i = 0; i < count; ++i) {
        AnmVm& v = vm_[static_cast<std::size_t>(first + i)];
        v.interrupt(22);
        v.tick(*ctx_.rng);
        v.interrupt(i == cursor ? 20 : 21);
    }
}

void ResultScreen::highlightRows(int first, int count, int cursor) {
    for (int i = 0; i < count; ++i) vm_[static_cast<std::size_t>(first + i)].interrupt(i == cursor ? 20 : 21);
}

void ResultScreen::chooseRow(int first, int count, int chosen) {
    for (int i = 0; i < count; ++i) vm_[static_cast<std::size_t>(first + i)].interrupt(i == chosen ? 23 : 1);
}

bool ResultScreen::moveRows(const MenuInput& in, int count, int& cursor) {
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d == 0) return false;
    cursor = (cursor + d + count) % count;
    ctx_.playSound(kSndMove);
    return true;
}

// State 1: score ranking, spell cards, play statistics, exit.
void ResultScreen::menu(const MenuInput& in) {
    if (sub_ == 0) {
        if (subPass_ == 0) {
            sendAll(1);
            enterRows(0, 4, menuCursor_);
        }
        if (++subPass_ >= 20) sub_ = 1, subPass_ = 0;
        return;
    }
    if (moveRows(in, 4, menuCursor_)) highlightRows(0, 4, menuCursor_);
    if (in.pressed(kCancel)) {
        ctx_.playSound(kSndBack);
        if (menuCursor_ != 3) {
            menuCursor_ = 3;
            highlightRows(0, 4, menuCursor_);
        } else {
            setState(State::Leave);
        }
        return;
    }
    if (!in.pressed(kSelect)) return;
    if (menuCursor_ == 3) {
        ctx_.playSound(kSndBack);
        setState(State::Leave);
        return;
    }
    chooseRow(0, 4, menuCursor_);
    ctx_.playSound(kSndSelect);
    setState(menuCursor_ == 0 ? State::Difficulty : menuCursor_ == 1 ? State::SpellCategory : State::DataIn);
}

// States 3, 4, 6 and 7: a column of rows; input after 6 passes.
void ResultScreen::rowsPage(const MenuInput& in) {
    int first = 4, count = 5;
    int* remembered = &difficulty_;
    switch (state_) {
        case State::Character: first = 15, count = 12, remembered = &shownCharacter_; break;
        case State::SpellCategory: first = 9, count = 6, remembered = &category_; break;
        case State::SpellColumn: first = 27, count = 13, remembered = &column_; break;
        default: break;
    }
    if (state_ == State::Difficulty) cheatCode(in);
    if (sub_ == 0) {
        if (subPass_ == 0) {
            cursor_ = std::clamp(*remembered, 0, count - 1);
            enterRows(first, count, cursor_);
        }
        if (++subPass_ >= 6) sub_ = 1, subPass_ = 0;
        return;
    }
    if (moveRows(in, count, cursor_)) highlightRows(first, count, cursor_);
    if (in.pressed(kCancel)) {
        ctx_.playSound(kSndBack);
        *remembered = cursor_;
        switch (state_) {
            case State::Difficulty:
                menuCursor_ = 0;
                setState(State::Menu);
                break;
            case State::Character:
                for (int i = 0; i < 12; ++i) vm_[static_cast<std::size_t>(15 + i)].interrupt(1);
                setState(State::Difficulty);
                break;
            case State::SpellCategory:
                menuCursor_ = 1;
                setState(State::Menu);
                break;
            default:
                for (int i = 0; i < 13; ++i) vm_[static_cast<std::size_t>(27 + i)].interrupt(1);
                setState(State::SpellCategory);
                break;
        }
        return;
    }
    if (!in.pressed(kSelect)) return;
    chooseRow(first, count, cursor_);
    ctx_.playSound(kSndSelect);
    switch (state_) {
        case State::Difficulty:
            difficulty_ = cursor_;
            cursor_ = 0;
            setState(State::Character);
            break;
        case State::Character:
            vm_[40].interrupt(3);  // (always 3, whatever the difficulty [QUIRK])
            shownCharacter_ = -1;
            setState(State::Table);
            break;
        case State::SpellCategory:
            category_ = cursor_;
            setState(State::SpellColumn);
            break;
        default:
            column_ = cursor_;
            vm_[40].interrupt(3);
            cursor_ = 0;  // the page
            shownPage_ = -1;
            setState(State::SpellList);
            break;
    }
}

// State 5: the table of list [difficulty][character]; Left/Right change the character.
void ResultScreen::table(const MenuInput& in) {
    if (pass_ == 10 && shownCharacter_ != cursor_) shownCharacter_ = cursor_;
    if (pass_ >= 6) {
        int c = cursor_;
        if (in.pressedOrRepeat(BtnLeft)) c = (c + 11) % 12;
        else if (in.pressedOrRepeat(BtnRight)) c = (c + 1) % 12;
        if (c != cursor_) {
            ctx_.playSound(kSndMove);
            pass_ = 0;
            vm_[40].interrupt(difficulty_ + 3);
            vm_[static_cast<std::size_t>(15 + cursor_)].interrupt(24);
            vm_[static_cast<std::size_t>(15 + c)].interrupt(25);
            cursor_ = c;
        }
    }
    if (in.pressed(kCancel)) {
        shownCharacter_ = cursor_;
        setState(State::Character);
        vm_[40].interrupt(1);
        ctx_.playSound(kSndBack);
    }
}

void ResultScreen::renderText(int i, const std::string& sjis) {
    AnmVm& t = text_[static_cast<std::size_t>(i)];
    renderSpriteText(ctx_.textures, ctx_.text, t, sjis, 0xFFFFFFu, TextAlign::Left);
    textLines_[static_cast<std::size_t>(i)] = sjis;
}

// The spell list's names (or "？？？？？" for a card never tried in the main game) and header.
void ResultScreen::renderSpellPage() {
    shownPage_ = cursor_;
    shownColumn_ = column_;
    const ExeTables* tables = ctx_.tables;
    if (!tables) return;
    const auto& ids = tables->cardCategories[static_cast<std::size_t>(std::clamp(category_, 0, 5))];
    const int n = static_cast<int>(ids.size());
    for (int i = 0; i < 10; ++i) {
        const int index = shownPage_ * 10 + i;
        if (index >= n) break;
        const int c = ids[static_cast<std::size_t>(index)];
        if (c < 0 || c >= kCardCount) continue;
        const CardRecord& r = records_.cards[static_cast<std::size_t>(c)];
        renderText(i, r.attempts[12] == 0 ? tables->dataText.unknownCard : cString(r.name.data(), r.name.size()));
        text_[static_cast<std::size_t>(i)].color |= 0xFF000000u;
    }
    renderText(10, sjisFormat(tables->dataText.spellHeader, capturedCount(category_, column_), n));
    text_[10].color |= 0xFF000000u;
}

// State 8: ten cards a page; Left/Right turn the page, Up/Down change the character column.
void ResultScreen::spellList(const MenuInput& in) {
    if (leaving_ && pass_ >= 10) {
        setState(State::SpellColumn);
        return;
    }
    if (pass_ == 10 && (shownPage_ != cursor_ || shownColumn_ != column_)) renderSpellPage();
    const int n = ctx_.tables ? static_cast<int>(ctx_.tables->cardCategories[static_cast<std::size_t>(std::clamp(category_, 0, 5))].size()) : 0;
    if (pass_ >= 6) {
        const int pages = std::max(1, (n + 9) / 10);
        int p = cursor_;
        if (in.pressedOrRepeat(BtnLeft)) p = (p + pages - 1) % pages;
        else if (in.pressedOrRepeat(BtnRight)) p = (p + 1) % pages;
        if (p != cursor_) {
            ctx_.playSound(kSndMove);
            cursor_ = p;
            pass_ = 0;
            vm_[40].interrupt(10);
        } else {
            const int old = column_;
            if (moveRows(in, 13, column_)) {
                pass_ = 0;
                animating_ = true;
                vm_[static_cast<std::size_t>(27 + old)].interrupt(24);
                vm_[static_cast<std::size_t>(27 + column_)].interrupt(25);
            }
        }
    }
    if (in.pressed(kCancel)) {
        leaving_ = true;
        pass_ = 0;
        ctx_.playSound(kSndBack);
        vm_[40].interrupt(1);
    }
    if (animating_ && pass_ >= 20) animating_ = false;
}

// The play statistics page's 19 lines (spec 2.7).
void ResultScreen::renderPlayData() {
    const ExeTables* tables = ctx_.tables;
    if (!tables) return;
    const ExeTables::DataText& d = tables->dataText;
    const auto run = records_.plstTime(ScoreData::kRunningTime), play = records_.plstTime(ScoreData::kPlayTime);
    auto row6 = [&](const std::string& fmt, int field, int lastRow) {
        auto v = [&](int row) { return static_cast<int>(records_.plstValue(row, field)); };
        return sjisFormat(fmt, v(0), v(1), v(2), v(3), v(4), v(lastRow));
    };
    std::array<std::string, 19> lines;
    lines[0] = sjisFormat(d.running, static_cast<int>(run[0]), static_cast<int>(run[1]), static_cast<int>(run[2]));
    lines[1] = sjisFormat(d.playTime, static_cast<int>(play[0]), static_cast<int>(play[1]), static_cast<int>(play[2]));
    lines[2] = d.playsHeader;
    for (int c = 0; c < 13; ++c) {
        const int field = c < 12 ? ScoreData::PerCharacter + c : ScoreData::Plays;
        auto v = [&](int row) { return static_cast<int>(records_.plstValue(row, field)); };
        lines[static_cast<std::size_t>(3 + c)] =
            sjisFormat(d.playsRow, tables->fullNames[static_cast<std::size_t>(c)].c_str(), v(0), v(1), v(2), v(3), v(4), v(6));
    }
    lines[16] = row6(d.clears, ScoreData::Clears, 6);
    lines[17] = row6(d.continues, ScoreData::Continues, 5);  // (row 5's cell as the total [QUIRK])
    lines[18] = row6(d.practice, ScoreData::PracticePlays, 6);
    for (int i = 0; i < 19; ++i) {
        renderText(i, lines[static_cast<std::size_t>(i)]);
        AnmVm& t = text_[static_cast<std::size_t>(i)];
        t.position = {56.0f, 64.0f + 17.0f * static_cast<float>(i) + (i >= 16 ? 17.0f : 0.0f), 0.0f};
        t.color &= 0x00FFFFFFu;
    }
}

// States 19-21: fade in over 40 passes, shown until a key, fade out over 20, back to the menu.
void ResultScreen::playData(const MenuInput& in) {
    auto alpha = [&](int a) {
        for (int i = 0; i < 20; ++i) {
            AnmVm& t = text_[static_cast<std::size_t>(i)];
            t.color = (t.color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24);
        }
    };
    if (state_ == State::DataIn) {
        if (pass_ == 1) renderPlayData();
        if (pass_ >= 1 && pass_ < 40) alpha(pass_ * 255 / 40);
        if (pass_ >= 40) {
            alpha(255);
            setState(State::DataShown);
        }
        return;
    }
    if (state_ == State::DataShown) {
        if (pass_ % 60 == 59 && ctx_.tables) {
            const auto run = records_.plstTime(ScoreData::kRunningTime);
            const std::string line = sjisFormat(ctx_.tables->dataText.running, static_cast<int>(run[0]), static_cast<int>(run[1]),
                                                static_cast<int>(run[2]));
            if (line != textLines_[0]) renderText(0, line);
        }
        if (in.pressed(BtnShoot | BtnBomb | BtnMenu | BtnEnter)) setState(State::DataOut);
        return;
    }
    if (pass_ < 20) {
        alpha(255 - pass_ * 255 / 20);
        return;
    }
    alpha(0);
    setState(State::Menu);
    menu(in);  // (state 0's handler runs in the same pass)
}

// The cheat code on the difficulty page (spec 2.10): while Focus or Skip is held, Right x 4,
// Left, D x 2, Q x 3; on the pass after the tenth key everything unlocks.
void ResultScreen::cheatCode(const MenuInput& in) {
    if (!in.held(BtnFocus | BtnSkip)) {
        cheatStep_ = 0;
        return;
    }
    if (cheatStep_ >= 10) {
        for (ClearRecord& r : records_.clears) {
            for (std::uint16_t& w : r.noContinue) w = static_cast<std::uint16_t>(w | 0xFFFF);
            for (std::uint16_t& w : r.reached) w = static_cast<std::uint16_t>(w | 0xFFFF);
        }
        for (CardRecord& c : records_.cards) {
            for (std::uint32_t& a : c.attempts) a += 1;
        }
        cheatStep_ = 0;
        ctx_.playSound(28);
        return;
    }
    static constexpr std::uint16_t kSequence[10] = {BtnRight, BtnRight, BtnRight, BtnRight, BtnLeft, BtnD, BtnD, BtnQ, BtnQ, BtnQ};
    if (in.pressed(kSequence[cheatStep_])) ++cheatStep_;
    else if (in.pressed(0x160B)) cheatStep_ = 0;
}

// ---- drawing ------------------------------------------------------------------------------------

void ResultScreen::drawTable(int difficulty, int character) {
    const AnmVm& panel = vm_[40];
    if (!(panel.position.x < 640.0f)) return;
    const float x = panel.position.x, y = panel.position.y;
    ascii_.add(x + 24.0f, y + 18.0f, "No  Name       Score(Stage)   Date   Slow", 0xFFE0E0EFu);
    const auto rows = records_.listRows(difficulty, character);
    for (int r = 0; r < 10; ++r) {
        const ScoreData::ListRow& row = rows[static_cast<std::size_t>(r)];
        const float ry = y + 36.0f + 18.0f * static_cast<float>(r);
        const bool mine = row.record && (*row.record)[9] == 1;
        std::uint32_t color = 0xFFFFC0C0u;
        if (state_ == State::NameEntry) color = mine ? 0xFFF0F0FFu : 0xC0FFC0C0u;
        ascii_.add(x + 24.0f, ry, std::format("{:2d}", r + 1), color);
        std::string name = "--------", date = "--/--", line;
        std::uint32_t score = row.placeholderScore;
        int continues = 0, stage = 0;
        float slow = 0.0f;
        if (row.record) {
            const HighScoreRecord& h = *row.record;
            name.assign(reinterpret_cast<const char*>(h.data() + 0x17), 8);
            date.assign(reinterpret_cast<const char*>(h.data() + 0x20), 5);
            score = get32(h, 0x0C);
            continues = std::min<int>(h[0x26], 9);
            stage = h[0x16];
            std::memcpy(&slow, h.data() + 0x10, 4);
        }
        for (char& c : name) c = c == 0 ? ' ' : c;
        if (stage == 99) line = std::format("{:>8} {:9d}{:1d}(C)", name, score, continues);
        else line = std::format("{:>8} {:9d}{:1d}({})", name, score, continues, kStageShown[std::clamp(stage, 0, 8)]);
        ascii_.add(x + 72.0f, ry, line, color);
        ascii_.add(x + 392.0f, ry, std::format(" {:>5}   {:3.2f}", date, slow), color);
        if (mine && state_ == State::NameEntry) {
            std::string marks(8, ' ');
            marks[static_cast<std::size_t>(std::min(nameCursor_, 7))] = '_';
            ascii_.add(x + 72.0f, ry, marks, color);
        }
    }
}

void ResultScreen::drawGrid() {
    for (int i = 0; i < kKeys; ++i) {
        if (i == kKeySpace) continue;
        const float x = 160.0f + 20.0f * static_cast<float>(i % 16), y = 356.0f + 18.0f * static_cast<float>(i / 16);
        std::string glyph(1, kKeyTable[i]);
        if (i == kKeySp) glyph = std::string(1, static_cast<char>(0x80));
        if (i == kKeyEnd) glyph = std::string(1, static_cast<char>(0x81));
        if (i == keyCursor_) {
            const int t = pass_ % 64, u = pass_ % 32;
            const float s = t < 32 ? 1.2f + static_cast<float>(u) * 0.8f / 32.0f : 2.0f - static_cast<float>(u) * 0.8f / 32.0f;
            ascii_.add(x - (s - 1.0f) * 8.0f, y - (s - 1.0f) * 8.0f, glyph, 0xFFFFFFC0u, s);
        } else {
            ascii_.add(x, y, glyph, 0xC0C0C0C0u);
        }
    }
}

// The final statistics (spec 3.5), from the panel VM's position, in its colour.
void ResultScreen::drawStatistics() {
    const AnmVm& p = vm_[71];
    const std::uint32_t c = p.color;
    float x = p.position.x + 210.0f, y = p.position.y + 32.0f;
    ascii_.add(x, y, std::format("{:9d}", game_.guiScore), c);
    ascii_.add(x + 117.0f, y, std::format("{:1d}", std::min(game_.continues, 9)), c);
    y += 22.0f;
    ascii_.add(x, y, kDifficultyNames[difficulty_], c);
    x += 13.0f;
    y += 22.0f;
    if (game_.finished) {
        ascii_.add(x, y, "      100%", c);
    } else {
        float f = static_cast<float>(game_.playTime) / (difficulty_ < 4 ? 195559.0f : 80000.0f);
        if (f >= 1.0f) f = 0.99f;
        ascii_.add(x, y, std::format("    {:3.2f}%", f * 100.0f), c);
    }
    y += 22.0f;
    ascii_.add(x, y, std::format("{:9d}", game_.continues), c);
    y += 22.0f;
    ascii_.add(x, y, std::format("{:9d}", static_cast<int>(game_.misses)), c);
    y += 22.0f;
    ascii_.add(x, y, std::format("{:9d}", static_cast<int>(game_.bombsUsedGame)), c);
    y += 22.0f;
    ascii_.add(x, y, std::format("{:9d}", game_.cardsCaptured), c);
    y += 22.0f;
    ascii_.add(x, y, std::format("    {:3.2f}%", 0.0f), c);  // (the slowdown: the port runs at full speed)
}

// The spell list (spec 2.6), from VM 40's position.
void ResultScreen::drawSpellList(SpriteSink& sink) {
    const AnmVm& panel = vm_[40];
    if (!(panel.position.x < 640.0f) || !ctx_.tables || shownPage_ < 0) return;
    const auto& ids = ctx_.tables->cardCategories[static_cast<std::size_t>(std::clamp(category_, 0, 5))];
    const int n = static_cast<int>(ids.size());
    const AnmDrawState none;
    float x = panel.position.x, y = panel.position.y;
    text_[10].position = {x + 320.0f, y - 16.0f, 0.0f};
    drawVm(sink, text_[10], none);
    y += 16.0f;
    const auto k = static_cast<std::size_t>(std::clamp(shownColumn_, 0, 12));
    static constexpr const char* kLevels[5] = {"E", "N", "H", "L", "-"};
    for (int i = 0; i < 10; ++i) {
        const int index = shownPage_ * 10 + i;
        if (index >= n) break;
        const int c = ids[static_cast<std::size_t>(index)];
        if (c < 0 || c >= kCardCount) continue;
        const CardRecord& r = records_.cards[static_cast<std::size_t>(c)];
        bar_.position = {x + 320.0f, y + 16.0f, 0.0f};
        bar_.scale.x = 2.375f;
        drawVm(sink, bar_, none);
        std::uint32_t colour = 0xFFF0F0FFu - static_cast<std::uint32_t>(i) * 0x00080800u;
        if (r.attempts[k] == 0) colour = 0xC0C0C0FFu;
        else if (r.captures[k] == 0) colour = 0xFFC0A0A0u;
        ascii_.add(x, y, std::format("No.{:02d}", c + 1), colour);
        AnmVm& name = text_[static_cast<std::size_t>(i)];
        name.position = {x + 78.0f, y, 0.0f};
        drawVm(sink, name, none);
        const bool lastWord = c >= 205;
        const std::uint32_t tries = lastWord ? r.practiceAttempts[k] : r.attempts[k];
        const std::uint32_t got = lastWord ? r.practiceCaptures[k] : r.captures[k];
        ascii_.add(x + 446.0f, y, tries == 0 ? std::string("---/---(-)") : std::format("{:3d}/{:3d}({})", got, tries, kLevels[std::min<int>(r.level, 4)]),
                   colour);
        if (r.captures[k] != 0) ascii_.add(x + 424.0f, y - 13.0f, std::format("MaxBonus {:8d}", r.best[k]), 0xFFA08090u, 0.8f);
        int step = 33;
        if (animating_) step = pass_ < 10 ? (10 - pass_) * 33 / 10 : (pass_ - 10) * 33 / 10;
        y += static_cast<float>(step);
    }
}

void ResultScreen::draw(SpriteSink& sink) {
    if (done_) return;
    background_.blit(sink, 0.0f, 0.0f);
    const AnmDrawState none;
    for (const AnmVm& v : vm_) {
        AnmVm d = v;
        d.position = {v.position.x + v.offset.x, v.position.y + v.offset.y, v.position.z + v.offset.z};
        drawVm(sink, d, none);
    }
    if (state_ == State::SpellList) drawSpellList(sink);
    else if (shownCharacter_ >= 0) drawTable(difficulty_, shownCharacter_);
    if (state_ == State::NameEntry && rank_ >= 0) drawGrid();
    if (state_ == State::Statistics || state_ == State::StatisticsOut) drawStatistics();
    if (state_ == State::DataIn || state_ == State::DataShown || state_ == State::DataOut) {
        for (int i = 0; i < 20; ++i) drawVm(sink, text_[static_cast<std::size_t>(i)], none);
    }
    if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);
    ascii_.clear();
}

}  // namespace touhou::th08
