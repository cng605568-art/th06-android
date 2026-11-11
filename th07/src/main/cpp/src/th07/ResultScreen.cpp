#include "th07/ResultScreen.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "th07/English.hpp"
#include "th07/ExeTables.hpp"
#include "th07/GameConfig.hpp"
#include "th07/Player.hpp"
#include "th07/Replay.hpp"

namespace touhou::th07 {

// The port's own grid (the original's is a 96-character table in th07.exe).
const char ResultScreen::kKeyTable[kKeys + 1] =
    "ABCDEFGHIJKLMNOP"
    "QRSTUVWXYZ.,:;_@"
    "abcdefghijklmnop"
    "qrstuvwxyz+-/*=%"
    "0123456789#!?'\"$"
    "()[]<>{}&~^|\\   ";

namespace {

constexpr int kResultScripts = 0x900;
constexpr int kTextSprite = 0x715;
constexpr int kBarSprite = 0x910;
constexpr std::uint16_t kSelect = BtnShoot | BtnEnter, kReturn = BtnBomb | BtnMenu;

// The port's own shot names (the original's are Shift-JIS strings in th07.exe).
constexpr const char* kShotNames[7] = {"Reimu Hakurei  (Spirit Sign)",  "Reimu Hakurei  (Dream Sign)",
                                       "Marisa Kirisame (Magic Sign)",  "Marisa Kirisame (Love Sign)",
                                       "Sakuya Izayoi  (Illusion Sign)", "Sakuya Izayoi  (Time Sign)",
                                       "All characters"};

int wrap(int v, int n) { return n > 0 ? ((v % n) + n) % n : 0; }


}  // namespace

ResultScreen::~ResultScreen() {
    if (loaded_) ctx_.anm->release(42);
    loaded_ = false;
}

void ResultScreen::setState(State s) {
    state_ = s;
    timer_ = 0;
    entered_ = false;
}

void ResultScreen::sendAll(int interrupt) {
    for (int i = 0; i < kInstances; ++i) {
        if (has_[static_cast<std::size_t>(i)]) vm_[static_cast<std::size_t>(i)].interrupt(interrupt);
    }
}

bool ResultScreen::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures || !ctx_.scores) {
        if (error) *error = "no game files";
        return false;
    }
    ScoreData& sd = *ctx_.scores;
    AnmLibrary& lib = *ctx_.anm;
    sd.phantasmCheck();
    ScoreFile::defaultLists(lists_);
    background_.load(*ctx_.vfs, *ctx_.textures, "data/result/result.jpg");
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "data/result00.anm", 42, kResultScripts, error)) return false;
    loaded_ = true;
    for (int i = 0; i < kInstances; ++i) {
        const auto k = static_cast<std::size_t>(i);
        vm_[k] = AnmVm{};
        has_[k] = lib.script(kResultScripts + i) != nullptr;
        if (has_[k]) vm_[k].startFresh(lib, *ctx_.rng, kResultScripts + i);
    }
    bar_ = AnmVm{};
    bar_.selectSprite(lib, kBarSprite);
    bar_.flags = AnmVm::Visible | AnmVm::Active;
    for (int i = 0; i < kTexts; ++i) {
        AnmVm& t = text_[static_cast<std::size_t>(i)];
        t = AnmVm{};
        t.selectSprite(lib, kTextSprite + i);
        t.flags = AnmVm::Active | AnmVm::AnchorLeft | AnmVm::AnchorTop;
        t.fontWidth = t.fontHeight = 15;
        textShown_[static_cast<std::size_t>(i)] = false;
    }
    const auto img = ctx_.readScores ? ctx_.readScores() : std::nullopt;
    if (img) {
        ScoreFile::readHighScores(*img, lists_);
        header_ = img->header;
        haveHeader_ = true;
    }
    lastNameFound_ = ScoreFile::readLastName(img ? &*img : nullptr, lastName_);
    if (mode_ == Mode::Viewer) {
        if (img) ScoreFile::readSpells(*img, sd.spells);
        ScoreFile::readClears(img ? &*img : nullptr, sd.clears);
        sd.phantasmCheck();
        ScoreFile::readPractice(img ? &*img : nullptr, sd);
    }
    const GameState& g = ctx_.lastGame;
    if (mode_ == Mode::PracticeEnd) {
        if (g.difficulty < 4 && g.stage >= 1 && g.stage <= 6 && !ctx_.recordsOff) {
            PracticeRecord& r = sd.practiceAt(g.shotIndex, g.stage - 1, g.difficulty);
            if (static_cast<std::int64_t>(g.score) > r.score) r.score = static_cast<std::int32_t>(g.score);
        }
        std::memcpy(name_.data(), lastName_.data(), 8);
        setState(State::Prompt);
    } else if (mode_ == Mode::AfterGame) {
        setState(State::NameEntry);
    } else {
        setState(State::Menu);
    }
    for (int c = 0; c < 7; ++c) {
        int n = 0;
        for (const SpellRecord& s : sd.spells) n += s.captures[static_cast<std::size_t>(c)] != 0 ? 1 : 0;
        capturedCount_[static_cast<std::size_t>(c)] = n;
    }
    spellColumn_ = 6;
    return true;
}

void ResultScreen::finish() {
    if (finished_) return;
    finished_ = true;
    if (ctx_.writeScores) ctx_.writeScores(lists_, lastName_, haveHeader_ ? &header_ : nullptr);
    if (loaded_) ctx_.anm->release(42);
    loaded_ = false;
    background_.release();
}

void ResultScreen::renderText(int index, const std::string& sjis, std::uint32_t colorRef) {
    AnmVm& t = text_[static_cast<std::size_t>(index)];
    renderSpriteText(ctx_.textures, ctx_.text, t, sjis, colorRef, TextAlign::Left);
    textShown_[static_cast<std::size_t>(index)] = true;
}

// ---- the pass -------------------------------------------------------------------------------

void ResultScreen::update(const MenuInput& in) {
    if (done_) return;
    switch (state_) {
        case State::Menu: menu(in); break;
        case State::ScorePage: scorePage(in); break;
        case State::SpellList: spellList(in); break;
        case State::PlayerData:
        case State::PlayerDataOut: playerData(in); break;
        case State::Exit:
            if (timer_ >= 60) {
                ctx_.request(FlowState::MainMenu);
                done_ = true;
                return;
            }
            break;
        case State::NameEntry: nameEntry(in); break;
        case State::Statistics:
        case State::StatisticsOut: statistics(in); break;
        case State::Prompt:
        case State::CannotSave: prompt(in); break;
        case State::SlotList: slotList(in); break;
        case State::Overwrite: overwrite(in); break;
        case State::ReplayName: replayName(in); break;
        case State::Leave:
            if (timer_ >= 60) {
                ctx_.request(FlowState::MainMenu);
                done_ = true;
                return;
            }
            break;
    }
    AnmLibrary& lib = *ctx_.anm;
    const bool phantasm = ctx_.scores->phantasmUnlocked();
    for (int i = 0; i < kInstances; ++i) {
        if (!has_[static_cast<std::size_t>(i)] || (i == 5 && !phantasm)) continue;
        vm_[static_cast<std::size_t>(i)].tick(lib, *ctx_.rng);
    }
    if (state_ == State::Menu) {
        for (int i = 0; i < 9; ++i) vm_[static_cast<std::size_t>(i)].color = i == menuCursor_ ? 0xFFFFFFFFu : 0xB0FFFFFFu;
    }
    // The prompts' Yes (instance 19) and No (20): the selected one red, alpha kept.
    if ((state_ == State::Prompt && promptDecided_) || state_ == State::Overwrite) {
        for (int i = 0; i < 2; ++i) {
            AnmVm& vm = vm_[static_cast<std::size_t>(19 + i)];
            const bool lit = (i == 0) == promptYes_;
            vm.color = (vm.color & 0xFF000000u) | (lit ? 0xFF6060u : 0x606060u);
        }
    }
    ++timer_;
}

void ResultScreen::menu(const MenuInput& in) {
    const bool phantasm = ctx_.scores->phantasmUnlocked();
    if (!entered_) {
        entered_ = true;
        sendAll(1);
    }
    if (timer_ < 20) return;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        menuCursor_ = wrap(menuCursor_ + d, 9);
        if (!phantasm && menuCursor_ == 5) menuCursor_ = wrap(menuCursor_ + d, 9);
        ctx_.playSound(kSndMove);
    }
    if (in.pressed(kSelect)) {
        if (menuCursor_ <= 5) {
            ctx_.playSound(kSndSelect);
            pageDifficulty_ = menuCursor_;
            sendAll(menuCursor_ + 3);
            setState(State::ScorePage);
        } else if (menuCursor_ == 6) {
            ctx_.playSound(kSndSelect);
            sendAll(10);
            spellDirty_ = true;
            setState(State::SpellList);
        } else if (menuCursor_ == 7) {
            ctx_.playSound(kSndSelect);
            sendAll(9);
            setState(State::PlayerData);
        } else {
            ctx_.playSound(kSndBack);
            sendAll(2);
            setState(State::Exit);
        }
    } else if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        if (menuCursor_ == 8) {
            sendAll(2);
            setState(State::Exit);
        } else {
            menuCursor_ = 8;
        }
    }
}

void ResultScreen::scorePage(const MenuInput& in) {
    if (timer_ == 20) renderText(0, kShotNames[pageShot_]);
    if (pageDifficulty_ == Hard) cheat(in);
    if (timer_ < 30) return;
    int d = 0;
    if (in.pressedOrRepeat(BtnLeft)) d = -1;
    else if (in.pressedOrRepeat(BtnRight)) d = 1;
    if (d != 0) {
        pageShot_ = wrap(pageShot_ + d, 6);
        ctx_.playSound(kSndMove);
        sendAll(pageDifficulty_ + 3);
        timer_ = -1;  // the page pass restarts
        return;
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        sendAll(1);
        menuCursor_ = pageDifficulty_;
        textShown_[0] = false;
        setState(State::Menu);
        entered_ = true;
    }
}

// The hidden code on the Hard page, with Focus or Skip held: Up x3, D x2, Down x2, Q x3.
void ResultScreen::cheat(const MenuInput& in) {
    if (cheatFire_) {
        cheatFire_ = false;
        cheatStep_ = 0;
        for (ClearRecord& c : ctx_.scores->clears) {
            c.noContinue.fill(kCleared);
            c.reached.fill(kCleared);
        }
        ctx_.playSound(kSndExtend);
        log::info("th07: everything unlocked (result screen code)");
        return;
    }
    if (!in.held(BtnFocus | BtnSkip)) {
        cheatStep_ = 0;
        return;
    }
    static constexpr std::uint16_t kSequence[10] = {BtnUp, BtnUp, BtnUp, BtnD, BtnD, BtnDown, BtnDown, BtnQ, BtnQ, BtnQ};
    const std::uint16_t expected = kSequence[cheatStep_];
    if (in.pressed(expected)) {
        if (++cheatStep_ == 10) cheatFire_ = true;
    } else if (in.pressed(static_cast<std::uint16_t>((BtnShoot | BtnBomb | BtnMenu | BtnQ | BtnS | BtnEnter) & ~expected))) {
        cheatStep_ = 0;
    }
}

void ResultScreen::renderSpellPage() {
    const ScoreData& sd = *ctx_.scores;
    const int col = spellColumn_;
    renderText(0, std::format("{}  {}/{}", kShotNames[col], capturedCount_[static_cast<std::size_t>(col)], kSpellCards));
    for (int r = 0; r < 10; ++r) {
        const int card = spellPage_ * 10 + r;
        if (card >= kSpellCards) {
            textShown_[static_cast<std::size_t>(1 + r)] = false;
            continue;
        }
        const SpellRecord& s = sd.spells[static_cast<std::size_t>(card)];
        std::string name = s.attempts[6] == 0 ? std::string("?????") : std::string(reinterpret_cast<const char*>(s.name.data()));
        // English text mode (thcrap's spell_name#result): shown translated, kept as it is.
        if (s.attempts[6] != 0 && ctx_.english) {
            if (const auto t = ctx_.english->spellName(card)) name = English::gameText(*t);
        }
        renderText(1 + r, name);
    }
}

void ResultScreen::spellList(const MenuInput& in) {
    if (rowAnim_ < 40) ++rowAnim_;
    if (timer_ == 20 && spellDirty_) {
        renderSpellPage();
        spellDirty_ = false;
    }
    if (timer_ < 30) return;
    int h = 0;
    if (in.pressedOrRepeat(BtnLeft)) h = -1;
    else if (in.pressedOrRepeat(BtnRight)) h = 1;
    if (h != 0) {
        spellPage_ = wrap(spellPage_ + h, 15);
        ctx_.playSound(kSndMove);
        sendAll(10);
        spellDirty_ = true;
        timer_ = -1;
        return;
    }
    int v = 0;
    if (in.pressedOrRepeat(BtnUp)) v = -1;
    else if (in.pressedOrRepeat(BtnDown)) v = 1;
    if (v != 0) {
        spellColumn_ = wrap(spellColumn_ + v, 7);
        ctx_.playSound(kSndMove);
        rowAnim_ = 0;
        spellDirty_ = true;
        timer_ = -1;
        return;
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        sendAll(1);
        for (int i = 0; i <= 10; ++i) textShown_[static_cast<std::size_t>(i)] = false;
        setState(State::Menu);
        entered_ = true;
    }
}

void ResultScreen::renderPlayerData() {
    const ScoreData& sd = *ctx_.scores;
    const PlayStats& st = sd.stats;
    const bool phantasm = sd.phantasmUnlocked();
    const int cols = phantasm ? 6 : 5;
    // The original's Shift-JIS labels and formats from the player's th07.exe; the English mode
    // (the patch has none for this page) and a missing exe keep the port's own English.
    if (ctx_.tables && ctx_.tables->playerData.complete() && !(ctx_.english && ctx_.english->active())) {
        const ExeTables::PlayerDataText& p = ctx_.tables->playerData;
        const std::size_t v = phantasm ? 1 : 0;
        auto clock = [&](const std::string& f, const PlayTime& t) {
            const FormatArg a[3] = {{t.hours}, {t.minutes}, {t.seconds}};
            return formatExeString(f, a);
        };
        auto counts = [&](const std::string& f, std::string_view label, auto value) {
            std::vector<FormatArg> a;
            if (!label.empty()) a.push_back({0, label, true});
            for (int d = 0; d < cols; ++d) a.push_back({static_cast<std::int64_t>(value(d))});
            a.push_back({static_cast<std::int64_t>(value(6))});
            return formatExeString(f, a);
        };
        int line = 0;
        renderText(line++, clock(p.runningTime, st.running));
        renderText(line++, clock(p.playTime, st.play));
        renderText(line++, p.header[v]);
        for (int s = 0; s < kShots; ++s) {
            renderText(line++, counts(p.row[v], p.rowLabels[static_cast<std::size_t>(s)],
                                      [&](int d) { return st.rows[static_cast<std::size_t>(d)].playsPerShot[static_cast<std::size_t>(s)]; }));
        }
        renderText(line++, counts(p.row[v], p.rowLabels[6], [&](int d) { return st.rows[static_cast<std::size_t>(d)].plays; }));
        renderText(line++, counts(p.clear[v], {}, [&](int d) { return d == 6 ? st.clearTotal() : st.rows[static_cast<std::size_t>(d)].clears; }));
        renderText(line++, counts(p.continues[v], {}, [&](int d) { return st.rows[static_cast<std::size_t>(d)].continues; }));
        renderText(line++, counts(p.practice[v], {}, [&](int d) { return st.rows[static_cast<std::size_t>(d)].practices; }));
        renderText(line++, counts(p.retry[v], {}, [&](int d) { return st.rows[static_cast<std::size_t>(d)].retries; }));
        return;
    }
    auto timeLine = [&](const char* label, const PlayTime& t) { return std::format("{:<20}{:>4}:{:02}:{:02}", label, t.hours, t.minutes, t.seconds); };
    auto row = [&](const std::string& label, auto value) {
        std::string s = std::format("{:<9}", label);
        for (int d = 0; d < cols; ++d) s += std::format("{:>7}", value(d));
        s += std::format("{:>7}", value(6));
        return s;
    };
    int line = 0;
    renderText(line++, timeLine("Total running time", st.running));
    renderText(line++, timeLine("Total play time", st.play));
    {
        std::string h = std::format("{:<9}", "Plays");
        static constexpr const char* kCols[6] = {"Easy", "Normal", "Hard", "Lunatic", "Extra", "Phantm"};
        for (int d = 0; d < cols; ++d) h += std::format("{:>7}", kCols[d]);
        h += std::format("{:>7}", "Total");
        renderText(line++, h);
    }
    for (int s = 0; s < kShots; ++s) {
        renderText(line++, row(std::string(shotName7(s)), [&](int d) { return st.rows[static_cast<std::size_t>(d)].playsPerShot[static_cast<std::size_t>(s)]; }));
    }
    renderText(line++, row("Total", [&](int d) { return st.rows[static_cast<std::size_t>(d)].plays; }));
    renderText(line++, row("Clear", [&](int d) { return d == 6 ? st.clearTotal() : st.rows[static_cast<std::size_t>(d)].clears; }));
    renderText(line++, row("Continue", [&](int d) { return st.rows[static_cast<std::size_t>(d)].continues; }));
    renderText(line++, row("Practice", [&](int d) { return st.rows[static_cast<std::size_t>(d)].practices; }));
    renderText(line++, row("Retry", [&](int d) { return st.rows[static_cast<std::size_t>(d)].retries; }));
}

void ResultScreen::playerData(const MenuInput& in) {
    if (!entered_) {
        entered_ = true;
        if (ctx_.refreshRunningTime) ctx_.refreshRunningTime();
        lastSeconds_ = ctx_.scores->stats.running.seconds;
        renderPlayerData();
    }
    if (state_ == State::PlayerData) {
        if (timer_ % 60 == 59) {
            if (ctx_.refreshRunningTime) ctx_.refreshRunningTime();
            if (ctx_.scores->stats.running.seconds != lastSeconds_) {
                lastSeconds_ = ctx_.scores->stats.running.seconds;
                renderPlayerData();
            }
        }
        if (in.pressed(BtnShoot | BtnBomb | BtnMenu | BtnEnter)) {
            ctx_.playSound(kSndBack);
            state_ = State::PlayerDataOut;
            timer_ = 0;
        }
    } else if (timer_ >= 20) {
        for (bool& b : textShown_) b = false;
        sendAll(1);
        setState(State::Menu);
        entered_ = true;
    }
}

// ---- after a game ---------------------------------------------------------------------------

void ResultScreen::nameEntry(const MenuInput& in) {
    const GameState& g = ctx_.lastGame;
    if (!entered_) {
        entered_ = true;
        if (g.slowMode || ctx_.timingBad || ctx_.recordsOff) {
            ctx_.scores->spells = ctx_.spellSnapshot;
            std::memcpy(name_.data(), lastName_.data(), 8);
            setState(State::Statistics);
            return;
        }
        pageDifficulty_ = std::clamp(g.difficulty, 0, 5);
        pageShot_ = std::clamp(g.shotIndex, 0, 5);
        sendAll(pageDifficulty_ + 3);
        renderText(0, kShotNames[pageShot_]);
        ScoreEntry e;
        e.shot = static_cast<std::uint8_t>(pageShot_);
        e.difficulty = static_cast<std::uint8_t>(pageDifficulty_);
        e.score = g.score;
        e.continues = static_cast<std::int8_t>(g.continues);
        e.stage = static_cast<std::uint8_t>(g.finished ? kCleared : g.stage);
        e.thisRun = true;
        std::memcpy(e.name.data(), lastName_.data(), 8);
        e.name[8] = 0;
        e.date = todayDate();
        e.slow = ctx_.lastSlow;
        ScoreList& l = lists_[ScoreFile::listIndex(pageDifficulty_, pageShot_)];
        rank_ = ScoreFile::insert(l, e);
        if (rank_ >= 10) {
            std::memcpy(name_.data(), lastName_.data(), 8);
            l[static_cast<std::size_t>(rank_)].thisRun = false;
            rank_ = -1;
            setState(State::Statistics);
            return;
        }
        std::memcpy(name_.data(), lastName_.data(), 8);
        nameCursor_ = 0;
        keyCursor_ = lastNameFound_ ? kKeyEnd : 0;
    }
    if (timer_ < 30) return;
    auto moveKey = [&](int step, bool inRow) {
        for (int guard = 0; guard < kKeys; ++guard) {
            if (inRow) {
                const int row = keyCursor_ / 16, col = wrap(keyCursor_ % 16 + step, 16);
                keyCursor_ = row * 16 + col;
            } else {
                keyCursor_ = wrap(keyCursor_ + step, kKeys);
            }
            if (keyCursor_ != kKeySpace) break;
        }
        ctx_.playSound(kSndMove);
    };
    if (in.pressedOrRepeat(BtnUp)) moveKey(-16, false);
    else if (in.pressedOrRepeat(BtnDown)) moveKey(16, false);
    else if (in.pressedOrRepeat(BtnLeft)) moveKey(-1, true);
    else if (in.pressedOrRepeat(BtnRight)) moveKey(1, true);
    ScoreEntry& e = lists_[ScoreFile::listIndex(pageDifficulty_, pageShot_)][static_cast<std::size_t>(rank_)];
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
    if (in.pressedOrRepeat(kReturn) && nameCursor_ > 0) {
        name_[static_cast<std::size_t>(std::min(nameCursor_, 7))] = ' ';
        --nameCursor_;
        name_[static_cast<std::size_t>(nameCursor_)] = ' ';
        ctx_.playSound(kSndBack);
    }
    std::memcpy(e.name.data(), name_.data(), 8);
    if (in.pressed(BtnMenu)) finishName();  // Esc backspaces, then finishes [QUIRK]
}

void ResultScreen::finishName() {
    sendAll(2);
    if (rank_ >= 0) {
        ScoreEntry& e = lists_[ScoreFile::listIndex(pageDifficulty_, pageShot_)][static_cast<std::size_t>(rank_)];
        std::memcpy(e.name.data(), name_.data(), 8);
        e.thisRun = false;
    }
    std::memcpy(lastName_.data(), name_.data(), 8);
    lastName_[8] = 0;
    textShown_[0] = false;
    setState(State::Statistics);
}

void ResultScreen::statistics(const MenuInput& in) {
    AnmVm& panel = vm_[40];
    if (state_ == State::Statistics) {
        if (timer_ < 30 && has_[40]) panel.interrupt(18);
        if (timer_ >= 90 && in.pressed(kSelect)) {
            ctx_.playSound(kSndSelect);
            if (has_[40]) panel.interrupt(2);
            state_ = State::StatisticsOut;
            timer_ = 0;
        }
    } else if (timer_ >= 30) {
        setState(State::Prompt);
    }
}

// ---- the replay save prompt (spec 6.5.4) ----------------------------------------------------

void ResultScreen::leave() {
    sendAll(2);
    ctx_.playSound(kSndBack);
    setState(State::Leave);
}

void ResultScreen::prompt(const MenuInput& in) {
    const GameState& g = ctx_.lastGame;
    if (state_ == State::CannotSave) {
        if (timer_ >= 20 && in.pressed(kSelect | kReturn)) leave();
        return;
    }
    if (!promptDecided_) {
        // The decision pass: one pass after the statistics close, 60 passes after creation in
        // practice.
        const int decision = mode_ == Mode::PracticeEnd ? 60 : 1;
        if (timer_ + 1 < decision) return;
        promptDecided_ = true;
        if (g.slowMode || ctx_.timingBad || ctx_.recordsOff) {
            sendAll(19);
            setState(State::CannotSave);
        } else if (g.continues != 0) {
            sendAll(14);
            setState(State::CannotSave);
        } else if (!ctx_.replayRecorded || !ctx_.replayRecorded()) {
            leave();  // (the port's developer aids reach this screen without a game)
        } else {
            sendAll(11);
            promptYes_ = true;
        }
        return;
    }
    if (timer_ < 80) return;
    if (in.pressed(BtnLeft | BtnRight)) {
        promptYes_ = !promptYes_;
        ctx_.playSound(kSndMove);
    }
    if (in.pressed(kReturn)) {
        leave();
    } else if (in.pressed(kSelect)) {
        if (!promptYes_) {
            leave();
            return;
        }
        ctx_.playSound(kSndSelect);
        sendAll(12);
        readSlots();
        setState(State::SlotList);
    }
}

void ResultScreen::readSlots() {
    if (!ctx_.replayDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(ctx_.replayDir, ec);
    }
    for (int i = 0; i < 15; ++i) {
        Slot& s = slots_[static_cast<std::size_t>(i)];
        s = Slot{};
        if (ctx_.replayDir.empty()) continue;
        std::ifstream f(std::filesystem::path(ctx_.replayDir) / std::format("th7_{:02d}.rpy", i + 1), std::ios::binary);
        if (!f) continue;
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // Filled when it validates (its version modulo 0x1000 is then always 0x100).
        const auto r = ReplayFile::parse(bytes, ctx_.verFile, nullptr, true);
        if (!r) continue;
        s.filled = true;
        s.name = r->summary.name();
        s.date = r->summary.date();
        s.shot = r->summary.shot();
        s.score = r->summary.score();
    }
}

void ResultScreen::slotList(const MenuInput& in) {
    if (timer_ < 20) return;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        slotCursor_ = wrap(slotCursor_ + d, 15);
        ctx_.playSound(kSndMove);
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        sendAll(11);
        setState(State::Prompt);
        timer_ = 80;  // (the prompt's lock is over)
        return;
    }
    if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        const auto today = todayDate();
        newDate_ = std::string(today.data(), 5);
        sendAll(17);
        const int row = 25 + slotCursor_;
        if (has_[static_cast<std::size_t>(row)]) vm_[static_cast<std::size_t>(row)].interrupt(16);
        nameCursor_ = 0;
        keyCursor_ = lastNameFound_ ? kKeyEnd : 0;
        if (slots_[static_cast<std::size_t>(slotCursor_)].filled) {
            sendAll(13);
            promptYes_ = true;
            setState(State::Overwrite);
        } else {
            setState(State::ReplayName);
        }
    }
}

void ResultScreen::overwrite(const MenuInput& in) {
    if (timer_ < 20) return;
    if (in.pressed(BtnLeft | BtnRight)) {
        promptYes_ = !promptYes_;
        ctx_.playSound(kSndMove);
    }
    if (in.pressed(kReturn) || (in.pressed(kSelect) && !promptYes_)) {
        ctx_.playSound(kSndBack);
        sendAll(12);
        setState(State::SlotList);
        timer_ = 20;
    } else if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        sendAll(17);
        const int row = 25 + slotCursor_;
        if (has_[static_cast<std::size_t>(row)]) vm_[static_cast<std::size_t>(row)].interrupt(16);
        setState(State::ReplayName);
    }
}

void ResultScreen::moveKey(const MenuInput& in) {
    auto step = [&](int delta, bool inRow) {
        for (int guard = 0; guard < kKeys; ++guard) {
            if (inRow) {
                const int row = keyCursor_ / 16, col = wrap(keyCursor_ % 16 + delta, 16);
                keyCursor_ = row * 16 + col;
            } else {
                keyCursor_ = wrap(keyCursor_ + delta, kKeys);
            }
            if (keyCursor_ != kKeySpace) break;
        }
        ctx_.playSound(kSndMove);
    };
    if (in.pressedOrRepeat(BtnUp)) step(-16, false);
    else if (in.pressedOrRepeat(BtnDown)) step(16, false);
    else if (in.pressedOrRepeat(BtnLeft)) step(-1, true);
    else if (in.pressedOrRepeat(BtnRight)) step(1, true);
}

void ResultScreen::replayName(const MenuInput& in) {
    if (timer_ < 1) return;
    moveKey(in);
    if (in.pressedOrRepeat(kSelect)) {
        const int at = std::min(nameCursor_, 7);
        if (keyCursor_ == kKeyEnd) {
            // END: the file is written under this name, which becomes the last name.
            const std::string path =
                ctx_.replayDir.empty() ? std::string() : (std::filesystem::path(ctx_.replayDir) / std::format("th7_{:02d}.rpy", slotCursor_ + 1)).string();
            const std::string name(name_.data(), 8);
            saved_ = ctx_.saveReplay ? ctx_.saveReplay(path, name) : false;
            std::memcpy(lastName_.data(), name_.data(), 8);
            lastName_[8] = 0;
            sendAll(2);
            setState(State::Leave);
            // [QUIRK] END also falls into the cursor advance and the select sound.
            if (nameCursor_ < 8) ++nameCursor_;
            ctx_.playSound(kSndSelect);
            return;
        }
        name_[static_cast<std::size_t>(at)] = keyCursor_ == kKeySp ? ' ' : kKeyTable[keyCursor_];
        if (nameCursor_ < 8 && ++nameCursor_ == 8) keyCursor_ = kKeyEnd;
        ctx_.playSound(kSndSelect);
    }
    if (in.pressedOrRepeat(kReturn) && nameCursor_ > 0) {
        name_[static_cast<std::size_t>(std::min(nameCursor_, 7))] = ' ';
        --nameCursor_;
        name_[static_cast<std::size_t>(nameCursor_)] = ' ';
        ctx_.playSound(kSndBack);
    }
    if (in.pressed(BtnMenu)) {
        sendAll(12);
        setState(State::SlotList);
        timer_ = 20;
    }
}

void ResultScreen::drawSlots() {
    const GameState& g = ctx_.lastGame;
    const bool naming = state_ == State::ReplayName;
    if (has_[24]) ascii_.add(vm_[24].position.x, vm_[24].position.y, "No.   Name     Date   Player  Score", 0xFFFFFFFFu);
    for (int i = 0; i < 15; ++i) {
        const auto k = static_cast<std::size_t>(25 + i);
        if (!has_[k]) continue;
        const AnmVm& at = vm_[k];
        const Slot& s = slots_[static_cast<std::size_t>(i)];
        const std::uint32_t color = i == slotCursor_ ? 0xFFFF8080u : 0xFF808080u;
        std::string line;
        if (naming) {
            line = std::format("No.{:02d} {:<8} {:<5} {} {:9d}0", i + 1, std::string(name_.data(), 8), newDate_, shotName7(g.shotIndex), g.guiScore);
        } else if (s.filled) {
            line = std::format("No.{:02d} {:<8} {:<5} {} {:9d}0", i + 1, s.name, s.date, shotName7(s.shot), s.score);
        } else {
            line = std::format("No.{:02d} -------- --/-- ------- ----------", i + 1);
        }
        ascii_.add(at.position.x, at.position.y, line, color);
        if (naming && (timer_ / 8) % 2 == 0) ascii_.add(at.position.x + 14.0f * static_cast<float>(6 + std::min(nameCursor_, 7)), at.position.y + 2.0f, "_", color);
    }
}

// ---- drawing --------------------------------------------------------------------------------

float ResultScreen::completion() const {
    const GameState& g = ctx_.lastGame;
    if (g.finished) return 1.0f;
    const float total = g.difficulty == Extra ? 80000.0f : g.difficulty == Phantasm ? 85000.0f : 180621.0f;
    return std::min(static_cast<float>(g.completion) / total, 0.99f);
}

void ResultScreen::drawScorePage(SpriteSink& sink, int difficulty, int shot) {
    const AnmVm& anchor = vm_[16];
    if (!has_[16] || anchor.position.x >= 640.0f) return;
    const float ax = anchor.position.x, ay = anchor.position.y;
    if (textShown_[0]) {
        AnmVm& t = text_[0];
        t.position = {ax + 64.0f, ay, 0.0f};
        drawVm(sink, t, AnmDrawState{});
    }
    ascii_.add(ax + 24.0f, ay + 18.0f, "No  Name      Score(Stage)  Date   Slow", 0xFFE0E0EFu);
    const ScoreList& l = lists_[ScoreFile::listIndex(difficulty, shot)];
    const bool naming = state_ == State::NameEntry && rank_ >= 0;
    for (int i = 0; i < 10 && i < static_cast<int>(l.size()); ++i) {
        const ScoreEntry& e = l[static_cast<std::size_t>(i)];
        const float y = ay + 36.0f + 18.0f * static_cast<float>(i);
        const std::uint32_t color = !naming ? 0xFFFFC0C0u : i == rank_ ? 0xFFF0F0FFu : 0xC0FFC0C0u;
        const char stage = e.stage == kCleared ? 'C' : e.stage >= 7 ? '1' : static_cast<char>('0' + std::clamp<int>(e.stage, 0, 9));
        const std::string name(e.name.data(), strnlen(e.name.data(), 8));
        ascii_.add(ax + 24.0f, y, std::format("{:02d}", i + 1), color);
        ascii_.add(ax + 72.0f, y, std::format("{:<8} {:9d}{}({})", name, e.score, e.continues % 10, stage), color);
        ascii_.add(ax + 392.0f, y, std::format("{} {:3.2f}", e.date.data(), e.slow), color);
        if (naming && i == rank_ && (timer_ / 8) % 2 == 0) {
            ascii_.add(ax + 72.0f + 14.0f * static_cast<float>(std::min(nameCursor_, 7)), y + 2.0f, "_", color);
        }
    }
}

void ResultScreen::drawSpellList(SpriteSink& sink) {
    const AnmVm& anchor = vm_[16];
    if (!has_[16]) return;
    const float ax = anchor.position.x, ay = anchor.position.y;
    if (ax >= 640.0f) return;
    const ScoreData& sd = *ctx_.scores;
    const int col = spellColumn_;
    if (textShown_[0]) {
        text_[0].position = {ax + 64.0f, ay, 0.0f};
        drawVm(sink, text_[0], AnmDrawState{});
    }
    const int spacing = rowAnim_ < 20 ? (20 - rowAnim_) * 33 / 20 : rowAnim_ < 40 ? (rowAnim_ - 20) * 33 / 20 : 33;
    for (int r = 0; r < 10; ++r) {
        const int card = spellPage_ * 10 + r;
        if (card >= kSpellCards) break;
        const SpellRecord& s = sd.spells[static_cast<std::size_t>(card)];
        const auto c = static_cast<std::size_t>(col);
        const float x = ax, y = ay + 24.0f + static_cast<float>(r * spacing);
        const bool tried = s.attempts[c] != 0;
        const std::uint32_t color = !tried ? 0xC0C0C0FFu : s.captures[c] == 0 ? 0xFFC0A0A0u : 0xFFF0F0FFu - static_cast<std::uint32_t>(r) * 0x00080800u;
        bar_.position = {x + 320.0f, y + 16.0f, 0.0f};
        bar_.scale = {2.375f, 1.0f};
        bar_.color = color;
        drawVm(sink, bar_, AnmDrawState{});
        ascii_.add(x, y, std::format("No.{:03d}", card + 1), color);
        AnmVm& name = text_[static_cast<std::size_t>(1 + r)];
        if (textShown_[static_cast<std::size_t>(1 + r)]) {
            name.position = {x + 96.0f, y, 0.0f};
            name.color = color | 0xFF000000u;
            drawVm(sink, name, AnmDrawState{});
        }
        ascii_.add(x + 496.0f, y, tried ? std::format("{:3d}/{:3d}", s.captures[c], s.attempts[c]) : std::string("---/---"), color);
        if (tried) ascii_.add(x + 424.0f, y - 13.0f, std::format("MaxBonus {:8d}", s.bestBonus[c]), 0xFFA08090u, 0.8f);
    }
}

void ResultScreen::drawStatistics() {
    const AnmVm& p = vm_[40];
    if (!has_[40]) return;
    const GameState& g = ctx_.lastGame;
    const float x = p.position.x + 210.0f, y = p.position.y + 32.0f;
    const std::uint32_t color = p.color;
    ascii_.add(x, y, std::format("{:9d}", g.guiScore), color);
    ascii_.add(x + 126.0f, y, std::format("{}", g.continues % 10), color);
    float yy = y;
    ascii_.add(x, yy += 22.0f, std::format("{:>10}", difficultyName(g.difficulty)), color);
    const float c = completion();
    ascii_.add(x + 14.0f, yy += 22.0f, g.finished ? std::string("     100%") : std::format("{:8.2f}%", c * 100.0f), color);
    ascii_.add(x + 14.0f, yy += 22.0f, std::format("{:9d}", g.continues), color);
    ascii_.add(x + 14.0f, yy += 22.0f, std::format("{:9d}", static_cast<int>(g.misses)), color);
    ascii_.add(x + 14.0f, yy += 22.0f, std::format("{:9d}", static_cast<int>(g.bombsUsed)), color);
    ascii_.add(x + 14.0f, yy += 22.0f, std::format("{:9d}", g.spellsCaptured), color);
    ascii_.add(x + 14.0f, yy += 22.0f, std::format("{:8.2f}%", ctx_.lastSlow), color);
}

void ResultScreen::drawGrid() {
    for (int i = 0; i < kKeys; ++i) {
        if (i == kKeySpace) continue;
        const float x = 160.0f + 20.0f * static_cast<float>(i % 16), y = 356.0f + 18.0f * static_cast<float>(i / 16);
        std::string glyph(1, kKeyTable[i]);
        if (i == kKeySp) glyph = std::string(1, static_cast<char>(0x80));
        if (i == kKeyEnd) glyph = std::string(1, static_cast<char>(0x81));
        if (i == keyCursor_) {
            const int t = timer_ % 64, u = timer_ % 32;
            const float s = t < 32 ? 1.2f + static_cast<float>(u) * 0.8f / 32.0f : 2.0f - static_cast<float>(u) * 0.8f / 32.0f;
            ascii_.add(x - (s - 1.0f) * 8.0f, y - (s - 1.0f) * 8.0f, glyph, 0xFFFFFFC0u, s);
        } else {
            ascii_.add(x, y, glyph, 0xC0C0C0C0u);
        }
    }
}

void ResultScreen::draw(SpriteSink& sink) {
    if (done_ && finished_) return;
    background_.blit(sink, 0, 0);
    const AnmDrawState none;
    const bool phantasm = ctx_.scores->phantasmUnlocked();
    for (int i = 0; i < kInstances; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (!has_[k] || (i == 5 && !phantasm)) continue;
        AnmVm vm = vm_[k];
        if (i >= 6 && i <= 8 && !phantasm) vm.position.y -= 32.0f;
        if (state_ == State::Menu && i == menuCursor_) {
            vm.position.x -= 4.0f;
            vm.position.y -= 4.0f;
        }
        drawVm(sink, vm, none);
    }
    switch (state_) {
        case State::ScorePage:
        case State::NameEntry: drawScorePage(sink, pageDifficulty_, pageShot_); break;
        case State::SpellList: drawSpellList(sink); break;
        case State::PlayerData:
        case State::PlayerDataOut: {
            const int a = state_ == State::PlayerData ? std::min(255, timer_ * 255 / 40) : std::max(0, 255 - timer_ * 255 / 20);
            for (int i = 0; i < kTexts; ++i) {
                if (!textShown_[static_cast<std::size_t>(i)]) continue;
                AnmVm& t = text_[static_cast<std::size_t>(i)];
                t.position = {56.0f, 128.0f + 17.0f * static_cast<float>(i) + (i >= 10 ? 34.0f : 0.0f), 0.0f};
                t.color = (static_cast<std::uint32_t>(a) << 24) | 0xFFFFFFu;
                drawVm(sink, t, none);
            }
            break;
        }
        case State::Statistics:
        case State::StatisticsOut: drawStatistics(); break;
        default: break;
    }
    if (state_ == State::NameEntry && timer_ >= 0 && rank_ >= 0) drawGrid();
    if (state_ == State::SlotList || state_ == State::Overwrite || state_ == State::ReplayName) drawSlots();
    if (state_ == State::ReplayName) drawGrid();
    ascii_.draw(sink, *ctx_.anm);
}

}  // namespace touhou::th07
