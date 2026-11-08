#include "th07/MainMenu.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "th07/Bgm.hpp"
#include "th07/Player.hpp"
#include "th07/SoundPlayer.hpp"

namespace touhou::th07 {

namespace {

constexpr int kTitleScripts = 0x900;
constexpr int kDescScript = 0x706;
constexpr std::uint32_t kDescColor = 0xFFF0E0u;  // GDI colour (0x00BBGGRR)
constexpr std::uint16_t kSelect = BtnShoot | BtnEnter, kReturn = BtnBomb | BtnMenu;

// The port's own descriptions (the original's are Shift-JIS strings in th07.exe).
constexpr const char* kMainDesc[8] = {
    "Start a new game",          "Play the Extra stage",  "Practice a stage you have reached", "Watch a replay",
    "Scores and play data",      "Listen to the music",   "Change the settings",               "Quit the game",
};
constexpr const char* kOptionDesc[9] = {
    "Spare lives at the start",       "Colour depth (kept for th07.cfg)", "Music: off, WAV, MIDI (WAV)",
    "Sound effects on or off",        "Screen mode (kept for th07.cfg)",  "Slow down with many bullets (no scores)",
    "Restore the default settings",   "Gamepad buttons",                  "Back to the title",
};
constexpr const char* kKeyDesc[12] = {
    "Shot button",  "Bomb button",  "Focus button", "Skip button",  "Pause button",       "Up button",
    "Down button",  "Left button",  "Right button", "Holding shot focuses", "Restore the buttons", "Back to the options",
};

int mainCard(int character) { return 71 + 3 * std::clamp(character, 0, 2); }
int sideCard(int character, int k) { return (k == 0 ? 80 : 83) + std::clamp(character, 0, 2); }
int wrap(int v, int n) { return n > 0 ? ((v % n) + n) % n : 0; }

}  // namespace

MainMenu::~MainMenu() { release(); }

void MainMenu::release() {
    if (loaded_) ctx_.anm->release(32);
    loaded_ = false;
    background_.release();
    splash_.release();
}

void MainMenu::setState(State s) {
    prevState_ = state_;
    state_ = s;
    timer_ = 0;
    entered_ = false;
    stateChanged_ = true;
}

bool MainMenu::create(std::string* error) {
    ScoreData& sd = *ctx_.scores;
    // A replay or the demo resets the remembered character (replays are not ported yet).
    if (ctx_.replay) {
        ctx_.character = 0;
        if (ctx_.demo) ctx_.replay = false;
    }
    // score.dat: the clear records, practice bests and spell history over memory (an unsaved
    // game's progress is discarded here).
    const auto img = ctx_.readScores ? ctx_.readScores() : std::nullopt;
    ScoreFile::readClears(img ? &*img : nullptr, sd.clears);
    ScoreFile::readPractice(img ? &*img : nullptr, sd);
    if (img) ScoreFile::readSpells(*img, sd.spells);
    const std::uint32_t hours = sd.stats.play.hours;
    ctx_.maxContinues = hours < 7 ? 3 : hours < 14 ? 4 : 5;
    // The Phantasm splash, when Phantasm was unlocked during this session.
    sd.phantasmCheck();
    const bool unlocked = sd.phantasmUnlocked();
    const bool showSplash = !ctx_.phantasmSeen && unlocked;
    ctx_.phantasmSeen = unlocked;

    state_ = prevState_ = State::Main;
    const int lastDifficulty = ctx_.lastValid ? ctx_.lastGame.difficulty : ctx_.config->difficulty;
    switch (ctx_.previous) {
        case FlowState::Game:
        case FlowState::NextStage:
        case FlowState::ResultAfterGame: cursor_ = lastDifficulty >= Extra ? 1 : 0; break;
        case FlowState::ResultViewer: cursor_ = 4; break;
        case FlowState::MusicRoom: cursor_ = 5; break;
        default: cursor_ = 0; break;
    }
    if (ctx_.lastValid && ctx_.practice) {
        cursor_ = 2;
        autoPractice_ = true;
        ctx_.practice = false;
    }
    if (!ctx_.anm->load(*ctx_.vfs, *ctx_.textures, "data/title01.anm", 32, kTitleScripts, error)) return false;
    loaded_ = true;
    if (!ctx_.demo) {
        if (ctx_.previous != FlowState::ResultViewer && ctx_.music) ctx_.music->load(8, "bgm/th07_01.mid");
        fade_ = showSplash ? -1 : 0;
    }
    AnmLibrary& lib = *ctx_.anm;
    for (int i = 0; i < kDescriptions; ++i) {
        AnmVm& d = desc_[static_cast<std::size_t>(i)];
        d = AnmVm{};
        d.startFresh(lib, *ctx_.rng, kDescScript);
        d.selectSprite(lib, d.activeSprite + i);
        d.set(AnmVm::Visible, false);
    }
    currentDesc_ = 0;
    descShown_ = false;
    ctx_.demo = false;
    timer_ = 0;
    entered_ = false;
    if (showSplash) {
        splash_.load(*ctx_.vfs, *ctx_.textures, "data/title/phantasm.jpg");
        state_ = State::Splash;
        splashT_ = 0;
    }
    return true;
}

void MainMenu::makeInstances() {
    AnmLibrary& lib = *ctx_.anm;
    for (int i = 0; i < kInstances; ++i) {
        const auto k = static_cast<std::size_t>(i);
        vm_[k] = AnmVm{};
        has_[k] = lib.script(kTitleScripts + i) != nullptr;
        active_[k] = has_[k];
        if (!has_[k]) continue;
        vm_[k].startFresh(lib, *ctx_.rng, kTitleScripts + i);
        base_[k] = vm_[k].activeSprite;
    }
    instancesMade_ = true;
}

// ---- helpers --------------------------------------------------------------------------------

int MainMenu::verticalMove(const MenuInput& in, int rows) {
    if (rows <= 0) return 0;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d == 0) return 0;
    cursor_ = wrap(cursor_ + d, rows);
    ctx_.playSound(kSndMove);
    return d;
}

int MainMenu::horizontalMove(const MenuInput& in, int columns) {
    if (columns <= 0) return 0;
    int d = 0;
    if (in.pressedOrRepeat(BtnLeft)) d = -1;
    else if (in.pressedOrRepeat(BtnRight)) d = 1;
    if (d == 0) return 0;
    cursor_ = wrap(cursor_ + d, columns);
    ctx_.playSound(kSndMove);
    return d;
}

void MainMenu::sendAll(int interrupt) {
    for (int i = 0; i < kInstances; ++i) {
        if (has_[static_cast<std::size_t>(i)]) vm_[static_cast<std::size_t>(i)].interrupt(interrupt);
    }
}

void MainMenu::highlight(int instance, bool on) {
    const auto k = static_cast<std::size_t>(instance);
    if (instance < 0 || instance >= kInstances || !has_[k]) return;
    vm_[k].selectSprite(*ctx_.anm, base_[k] + (on ? 0 : 1));
}

void MainMenu::highlightRows(int first, int count, int selected) {
    for (int i = first; i < first + count; ++i) highlight(i, i == selected);
}

void MainMenu::renderDescriptions(int count, const char* const* texts) {
    for (int i = 0; i < count && i < kDescriptions; ++i) {
        AnmVm& d = desc_[static_cast<std::size_t>(i)];
        renderSpriteText(ctx_.textures, ctx_.text, d, texts[i], kDescColor, TextAlign::Centre);
    }
}

void MainMenu::showDescription(int row) {
    currentDesc_ = std::clamp(row, 0, kDescriptions - 1);
    desc_[static_cast<std::size_t>(currentDesc_)].interrupt(1);
    descShown_ = true;
}

void MainMenu::hideDescription() { desc_[static_cast<std::size_t>(currentDesc_)].interrupt(2); }

void MainMenu::loadBackground(const char* path) {
    if (background_.loaded() && background_.path() == path) return;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, path)) log::warn("{} could not be loaded", path);
}

bool MainMenu::shotLocked(int shot) const {
    const ScoreData& sd = *ctx_.scores;
    const int d = ctx_.config->difficulty;
    if (d == Extra) return !sd.extraShotUnlocked(shot);
    if (d == Phantasm) return !sd.phantasmShotUnlocked(shot);
    return false;
}

bool MainMenu::characterLocked(int character) const { return shotLocked(character * 2) && shotLocked(character * 2 + 1); }

int MainMenu::difficultyCard() const {
    const int d = ctx_.config->difficulty;
    if (d < Extra) return 67 + d;
    return ctx_.scores->phantasmUnlocked() ? 158 + d : 161;
}

// All 15 character cards active: the cursor character's main and side cards get interrupt 9,
// the others 8.
void MainMenu::setCharacterCards(bool allActive) {
    for (int c = 0; c < 3; ++c) {
        const int cards[5] = {mainCard(c), mainCard(c) + 1, mainCard(c) + 2, sideCard(c, 0), sideCard(c, 1)};
        for (int i = 0; i < 5; ++i) {
            const auto k = static_cast<std::size_t>(cards[i]);
            if (!has_[k]) continue;
            if (allActive) active_[k] = true;
            const bool lit = c == cursor_ && (i == 0 || i >= 3);
            vm_[k].interrupt(lit ? 9 : 8);
        }
    }
}

// On entry: only the cursor character's 5 cards are active; the others are hidden at once.
void MainMenu::onlyCharacterCards(int character) {
    for (int c = 0; c < 3; ++c) {
        const int cards[5] = {mainCard(c), mainCard(c) + 1, mainCard(c) + 2, sideCard(c, 0), sideCard(c, 1)};
        for (int i = 0; i < 5; ++i) {
            const auto k = static_cast<std::size_t>(cards[i]);
            if (!has_[k]) continue;
            active_[k] = c == character;
            const bool lit = c == character && (i == 0 || i >= 3);
            vm_[k].interrupt(lit ? 9 : 8);
            if (!lit) vm_[k].color &= 0x00FFFFFFu;
        }
    }
}

void MainMenu::startGame() {
    ctx_.replay = false;
    ctx_.request(FlowState::Game);
    if (ctx_.music) ctx_.music->stop();
    done_ = true;
}

// ---- the pass -------------------------------------------------------------------------------

void MainMenu::update(const MenuInput& in) {
    if (done_) return;
    AnmLibrary& lib = *ctx_.anm;
    for (int run = 0; run < 3; ++run) {
        stateChanged_ = false;
        const bool again = handle(in);
        if (done_) return;
        if (instancesMade_) {
            for (int i = 0; i < kInstances; ++i) {
                const auto k = static_cast<std::size_t>(i);
                if (has_[k] && active_[k]) vm_[k].tick(lib, *ctx_.rng);
            }
        }
        if (descShown_) desc_[static_cast<std::size_t>(currentDesc_)].tick(lib, *ctx_.rng);
        if (!again) break;
    }
    if (!stateChanged_) ++timer_;
    if (fade_ >= 0 && ++fade_ > 70) fade_ = -1;
}

bool MainMenu::handle(const MenuInput& in) {
    switch (state_) {
        case State::Splash: splash(in); return false;
        case State::Main: return mainMenu(in);
        case State::OptionWait:
            if (timer_ >= 30) setState(State::Option);
            return false;
        case State::Option: return option(in);
        case State::KeyConfig: return keyConfig(in);
        case State::Difficulty: return difficulty(in);
        case State::DifficultyLeave:
            if (timer_ >= 30) {
                cursor_ = variant_ == Variant::Practice ? 2 : variant_ == Variant::Extra ? 1 : 0;
                ctx_.practice = false;
                setState(State::Main);
            }
            return false;
        case State::Character: return character(in);
        case State::Shot: return shot(in);
        case State::PracticeStage: return practiceStage(in);
        case State::Replay: return replay(in);
        case State::ReplayLeave:
            if (timer_ >= 30) {
                cursor_ = 3;
                setState(State::Main);
            }
            return false;
        case State::Quit:
            if (timer_ >= 60) {
                ctx_.request(FlowState::Exit);
                done_ = true;
            }
            return false;
    }
    return false;
}

void MainMenu::splash(const MenuInput& in) {
    if (splashT_ >= 120 && splashT_ < 840 && in.pressed(kSelect | BtnBomb)) {
        splashT_ = 840;
        ctx_.playSound(kSndSelect);
    }
    if (++splashT_ >= 900) {
        splash_.release();
        fade_ = 0;
        setState(State::Main);
        prevState_ = State::Main;
    }
}

bool MainMenu::mainMenu(const MenuInput& in) {
    ScoreData& sd = *ctx_.scores;
    if (!entered_) {
        entered_ = true;
        const bool reload = prevState_ == State::Main || prevState_ == State::Difficulty || prevState_ == State::DifficultyLeave ||
                            prevState_ == State::Character || prevState_ == State::Shot || prevState_ == State::PracticeStage ||
                            prevState_ == State::Replay || prevState_ == State::ReplayLeave;
        if (reload) loadBackground("data/title/title00.jpg");
        if (firstEntry_) {
            if (ctx_.previous != FlowState::ResultViewer && ctx_.music) ctx_.music->playLoaded(8);
            makeInstances();
            firstEntry_ = false;
        }
        sendAll(2);
        highlightRows(1, 8, cursor_ + 1);
        if (autoPractice_) {
            variant_ = Variant::Practice;
            sendAll(5);
            setState(State::Difficulty);
            return false;
        }
        renderDescriptions(8, kMainDesc);
        showDescription(cursor_);
    }
    const int moved = verticalMove(in, 8);
    if (moved != 0) {
        if (!sd.extraUnlocked() && cursor_ == 1) cursor_ = wrap(cursor_ + moved, 8);
        highlightRows(1, 8, cursor_ + 1);
        showDescription(cursor_);
    }
    // The attract demo after 15 idle seconds needs replays, which are not ported yet.
    if (in.current() != 0) idle_ = 0;
    else if (++idle_ > 900) idle_ = 0;
    if (timer_ < 10) return false;
    if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        const int d = ctx_.config->difficulty;
        switch (cursor_) {
            case 0:
            case 2:
                ctx_.practice = cursor_ == 2;
                variant_ = cursor_ == 2 ? Variant::Practice : Variant::Main;
                cursor_ = d >= Extra ? 2 : d;
                sendAll(5);
                hideDescription();
                setState(State::Difficulty);
                break;
            case 1:
                if (sd.extraUnlocked()) {
                    ctx_.practice = false;
                    variant_ = Variant::Extra;
                    cursor_ = d == Phantasm ? 1 : 0;
                    sendAll(5);
                    hideDescription();
                    setState(State::Difficulty);
                    break;
                }
                [[fallthrough]];  // locked: the Replay case [QUIRK]
            case 3:
                ctx_.practice = false;
                sendAll(13);
                hideDescription();
                setState(State::Replay);
                break;
            case 4:
                hideDescription();
                ctx_.request(FlowState::ResultViewer);
                done_ = true;
                break;
            case 5:
                ctx_.request(FlowState::MusicRoom);
                done_ = true;
                break;
            case 6:
                optionCursor_ = 0;
                configCopy_ = *ctx_.config;
                hideDescription();
                setState(State::OptionWait);
                break;
            case 7:
                sendAll(1);
                hideDescription();
                setState(State::Quit);
                break;
            default: break;
        }
    } else if (in.pressed(kReturn)) {
        cursor_ = 7;
        ctx_.playSound(kSndBack);
        highlightRows(1, 8, cursor_ + 1);
        showDescription(cursor_);
    }
    return false;
}

// ---- Option and Key Config ------------------------------------------------------------------

bool MainMenu::option(const MenuInput& in) {
    GameConfig& c = *ctx_.config;
    auto leave = [&]() {
        cursor_ = 6;
        ctx_.playSound(kSndBack);
        hideDescription();
        if (ctx_.saveConfig) ctx_.saveConfig();
        setState(State::Main);
    };
    auto applyMusic = [&]() {
        if (!ctx_.music) return;
        ctx_.music->stop();
        ctx_.music->setMode(c.music);
        if (c.music != GameConfig::MusicOff) {
            ctx_.music->load(8, "bgm/th07_01.mid");
            ctx_.music->playLoaded(8);
        }
    };
    if (!entered_) {
        entered_ = true;
        sendAll(3);
        highlightRows(9, 9, 9 + optionCursor_);
        renderDescriptions(9, kOptionDesc);
        showDescription(optionCursor_);
        idle_ = 0;
    }
    // The value instances show the current values every pass.
    highlightRows(18, 5, 18 + c.lives);
    highlightRows(23, 2, 23 + (c.colourMode == 1 ? 1 : 0));
    highlightRows(25, 3, 25 + c.music);
    highlightRows(28, 2, 28 + c.sound);
    highlightRows(30, 2, 30 + c.windowed);
    highlightRows(32, 2, 32 + c.slowMode);
    if (in.current() != 0) idle_ = 0;
    else if (++idle_ >= 3600) {
        leave();
        return false;
    }
    if (timer_ < 4) return false;
    cursor_ = optionCursor_;
    if (verticalMove(in, 9) != 0) {
        optionCursor_ = cursor_;
        highlightRows(9, 9, 9 + optionCursor_);
        showDescription(optionCursor_);
    }
    int h = 0;
    if (in.pressedOrRepeat(BtnLeft)) h = -1;
    else if (in.pressedOrRepeat(BtnRight)) h = 1;
    if (h != 0 && optionCursor_ <= 5) {
        ctx_.playSound(kSndMove);
        switch (optionCursor_) {
            case 0: c.lives = static_cast<std::uint8_t>(wrap(c.lives + h, 5)); break;
            case 1: c.colourMode = static_cast<std::uint8_t>(wrap((c.colourMode == 1 ? 1 : 0) + h, 2)); break;
            case 2:
                c.music = static_cast<std::uint8_t>(wrap(c.music + h, 3));
                applyMusic();
                break;
            case 3:
                c.sound = static_cast<std::uint8_t>(wrap(c.sound + h, 2));
                if (ctx_.sound) ctx_.sound->setEnabled(c.sound != 0);
                break;
            case 4: c.windowed = static_cast<std::uint8_t>(wrap(c.windowed + h, 2)); break;
            case 5: c.slowMode = static_cast<std::uint8_t>(wrap(c.slowMode + h, 2)); break;
            default: break;
        }
    }
    if (in.pressed(kSelect)) {
        switch (optionCursor_) {
            case 6: {
                const bool musicChanged = c.music != GameConfig::MusicWav;
                c.lives = 2;
                c.bombs = 3;
                c.music = GameConfig::MusicWav;
                c.sound = 1;
                c.slowMode = 0;
                if (ctx_.sound) ctx_.sound->setEnabled(true);
                if (musicChanged) applyMusic();
                ctx_.playSound(kSndSelect);
                break;
            }
            case 7:
                ctx_.playSound(kSndSelect);
                keyCursor_ = 0;
                hideDescription();
                setState(State::KeyConfig);
                break;
            case 8: leave(); break;
            default: break;
        }
    } else if (in.pressed(kReturn)) {
        if (optionCursor_ == 8) {
            leave();
        } else {
            optionCursor_ = 8;
            ctx_.playSound(kSndBack);
            highlightRows(9, 9, 9 + optionCursor_);
            showDescription(optionCursor_);
        }
    }
    return false;
}

// Key Config: the pad mapping is kept for th07.cfg (the port maps controls itself); the
// screen shows the numbers, toggles Shot-slow and restores the mapping loaded at boot.
bool MainMenu::keyConfig(const MenuInput& in) {
    GameConfig& c = *ctx_.config;
    static constexpr int kPadIndex[9] = {0, 1, 2, 8, 3, 4, 5, 6, 7};  // row -> th07.cfg slot
    auto showDigits = [&]() {
        for (int a = 0; a < 9; ++a) {
            const int b = keyWork_[static_cast<std::size_t>(kPadIndex[a])];
            for (int k = 0; k < 2; ++k) {
                const int i = 47 + 2 * a + k;
                if (!has_[static_cast<std::size_t>(i)]) continue;
                active_[static_cast<std::size_t>(i)] = b >= 0;
                if (b >= 0) vm_[static_cast<std::size_t>(i)].selectSprite(*ctx_.anm, base_[static_cast<std::size_t>(i)] + (k == 0 ? b / 10 : b % 10) * 2);
            }
        }
        highlightRows(65, 2, 65 + c.shotSlow);
    };
    auto leave = [&]() {
        for (std::size_t i = 0; i < 9; ++i) c.pad[i] = keyWork_[i];
        for (int i = 47; i <= 64; ++i) active_[static_cast<std::size_t>(i)] = has_[static_cast<std::size_t>(i)];
        optionCursor_ = 7;
        ctx_.playSound(kSndBack);
        hideDescription();
        setState(State::Option);
    };
    if (!entered_) {
        entered_ = true;
        if (!keyBootSaved_) {
            keyBoot_ = c.pad;
            keyBootSaved_ = true;
        }
        keyWork_ = c.pad;
        sendAll(4);
        highlightRows(35, 12, 35 + keyCursor_);
        renderDescriptions(12, kKeyDesc);
        showDescription(keyCursor_);
        idle_ = 0;
    }
    showDigits();
    if (in.current() != 0) idle_ = 0;
    else if (++idle_ >= 3600) {
        leave();
        return false;
    }
    if (timer_ < 4) return false;
    cursor_ = keyCursor_;
    if (verticalMove(in, 12) != 0) {
        keyCursor_ = cursor_;
        highlightRows(35, 12, 35 + keyCursor_);
        showDescription(keyCursor_);
    }
    if (keyCursor_ == 9 && in.pressed(BtnLeft | BtnRight)) {
        c.shotSlow = c.shotSlow ? 0 : 1;
        ctx_.playSound(kSndMove);
    }
    if (in.pressed(kSelect)) {
        if (keyCursor_ == 10) {
            keyWork_ = keyBoot_;
            c.shotSlow = 1;
            ctx_.playSound(kSndSelect);
        } else if (keyCursor_ == 11) {
            leave();
        }
    } else if (in.pressed(kReturn)) {
        if (keyCursor_ == 11) {
            leave();
        } else {
            keyCursor_ = 11;
            ctx_.playSound(kSndBack);
            highlightRows(35, 12, 35 + keyCursor_);
            showDescription(keyCursor_);
        }
    }
    return false;
}

// ---- the select screens ---------------------------------------------------------------------

bool MainMenu::difficulty(const MenuInput& in) {
    const ScoreData& sd = *ctx_.scores;
    GameConfig& c = *ctx_.config;
    const bool phantasm = sd.phantasmUnlocked();
    if (!entered_) {
        entered_ = true;
        if (prevState_ != State::Character) loadBackground("data/title/select00.jpg");
        ctx_.scores->phantasmCheck();
        cursor_ = c.difficulty;
        if (variant_ != Variant::Extra) {
            sendAll(7);
            if (cursor_ >= Extra) cursor_ = Normal;
            highlightRows(67, 4, 67 + cursor_);
        } else if (!phantasm) {
            sendAll(12);
            cursor_ = 0;
        } else {
            sendAll(22);
            cursor_ = std::max(0, c.difficulty - Extra);
            highlightRows(162, 2, 162 + cursor_);
        }
        if (autoPractice_) setState(State::Character);
        return false;
    }
    if (timer_ < 30) return false;
    const int rows = variant_ != Variant::Extra ? 4 : phantasm ? 2 : 1;
    if (verticalMove(in, rows) != 0) {
        if (variant_ != Variant::Extra) highlightRows(67, 4, 67 + cursor_);
        else if (phantasm) highlightRows(162, 2, 162 + cursor_);
    }
    const auto chosen = static_cast<std::uint8_t>(cursor_ + (variant_ == Variant::Extra ? Extra : 0));
    if (in.pressed(kSelect)) {
        c.difficulty = chosen;
        ctx_.playSound(kSndSelect);
        cursor_ = 0;
        setState(State::Character);
    } else if (in.pressed(kReturn)) {
        c.difficulty = chosen;
        ctx_.playSound(kSndBack);
        sendAll(6);
        setState(State::DifficultyLeave);
    }
    return false;
}

bool MainMenu::character(const MenuInput& in) {
    if (!entered_) {
        entered_ = true;
        sendAll(8);
        const int card = difficultyCard();
        if (has_[static_cast<std::size_t>(card)]) vm_[static_cast<std::size_t>(card)].interrupt(9);
        cursor_ = std::clamp(ctx_.character, 0, 2);
        for (int k = 0; k < 3 && characterLocked(cursor_); ++k) cursor_ = (cursor_ + 1) % 3;
        onlyCharacterCards(cursor_);
        if (autoPractice_) {
            ctx_.character = cursor_;
            setState(State::Shot);
        }
        return false;
    }
    if (timer_ < 30) return false;
    if (horizontalMove(in, 3) != 0) {
        // Locked characters are skipped forward whichever way was pressed [QUIRK].
        for (int k = 0; k < 3 && characterLocked(cursor_); ++k) cursor_ = (cursor_ + 1) % 3;
        setCharacterCards(true);
    }
    if (in.pressed(kSelect)) {
        ctx_.character = cursor_;
        ctx_.playSound(kSndSelect);
        cursor_ = 0;
        setState(State::Shot);
    } else if (in.pressed(kReturn)) {
        ctx_.character = cursor_;
        ctx_.playSound(kSndBack);
        cursor_ = 0;
        setState(State::Difficulty);
    }
    return false;
}

bool MainMenu::shot(const MenuInput& in) {
    const int ch = std::clamp(ctx_.character, 0, 2);
    auto showRows = [&]() {
        highlight(mainCard(ch) + 1, cursor_ == 0);
        highlight(mainCard(ch) + 2, cursor_ == 1);
    };
    if (!entered_) {
        entered_ = true;
        sendAll(10);
        const int card = difficultyCard();
        if (has_[static_cast<std::size_t>(card)]) vm_[static_cast<std::size_t>(card)].interrupt(9);
        for (int c = 0; c < 3; ++c) {
            const int cards[5] = {mainCard(c), mainCard(c) + 1, mainCard(c) + 2, sideCard(c, 0), sideCard(c, 1)};
            for (int i = 0; i < 5; ++i) active_[static_cast<std::size_t>(cards[i])] = has_[static_cast<std::size_t>(cards[i])] && c == ch && i < 3;
        }
        cursor_ = ctx_.shotType & 1;
        if (shotLocked(ch * 2 + cursor_)) cursor_ ^= 1;
        showRows();
        if (autoPractice_) {
            ctx_.shotType = cursor_;
            cursor_ = std::max(0, ctx_.stage - 1);
            setState(State::PracticeStage);
        }
        return false;
    }
    if (timer_ < 30) return false;
    if (verticalMove(in, 2) != 0) {
        if (shotLocked(ch * 2 + cursor_)) cursor_ ^= 1;
        showRows();
    }
    if (in.pressed(kSelect)) {
        ctx_.shotType = cursor_;
        ctx_.playSound(kSndSelect);
        if (!ctx_.practice) {
            const int d = ctx_.config->difficulty;
            ctx_.stage = d < Extra ? 0 : d + 2;
            startGame();
            return false;
        }
        cursor_ = 0;
        setState(State::PracticeStage);
        return true;
    }
    if (in.pressed(kReturn)) {
        ctx_.shotType = cursor_;
        ctx_.playSound(kSndBack);
        for (int i = 71; i <= 85; ++i) active_[static_cast<std::size_t>(i)] = has_[static_cast<std::size_t>(i)];
        cursor_ = 0;
        setState(State::Character);
        return true;
    }
    return false;
}

bool MainMenu::practiceStage(const MenuInput& in) {
    const int ch = std::clamp(ctx_.character, 0, 2);
    if (!entered_) {
        entered_ = true;
        sendAll(18);
        for (int i = 71; i <= 85; ++i) active_[static_cast<std::size_t>(i)] = false;
        for (int i = 0; i < 3; ++i) active_[static_cast<std::size_t>(mainCard(ch) + i)] = has_[static_cast<std::size_t>(mainCard(ch) + i)];
        ctx_.practice = true;
        autoPractice_ = false;
        return false;
    }
    if (timer_ < 30) return false;
    const int k = ctx_.scores->practiceStages(ctx_.shotIndex(), ctx_.config->difficulty);
    if (cursor_ >= k) cursor_ = 0;
    verticalMove(in, k);
    if (in.pressed(kSelect) && k > 0) {
        ctx_.playSound(kSndSelect);
        ctx_.stage = cursor_;
        startGame();
        return false;
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        cursor_ = ctx_.shotType;
        for (int i = 71; i <= 85; ++i) active_[static_cast<std::size_t>(i)] = has_[static_cast<std::size_t>(i)];
        setState(State::Shot);  // still practice [QUIRK]
        return true;
    }
    return false;
}

bool MainMenu::replay(const MenuInput& in) {
    if (!entered_) {
        entered_ = true;
        if (prevState_ != State::Replay && prevState_ != State::ReplayLeave) loadBackground("data/title/select00.jpg");
        sendAll(14);
        return false;
    }
    if (timer_ < 30) return false;
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        sendAll(16);
        setState(State::ReplayLeave);
    }
    return false;
}

// ---- drawing --------------------------------------------------------------------------------

void MainMenu::draw(SpriteSink& sink) {
    if (done_) return;
    if (state_ == State::Splash) {
        splash_.blit(sink, 0, 0);
        int a = 0;
        if (splashT_ < 60) a = (60 - splashT_) * 255 / 60;
        else if (splashT_ > 840) a = std::min(255, (splashT_ - 840) * 255 / 60);
        fillScreen(sink, ctx_.ui, static_cast<std::uint32_t>(a) << 24);
        return;
    }
    background_.blit(sink, 0, 0);
    const AnmDrawState none;
    if (instancesMade_) {
        for (int i = 0; i < kInstances; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (has_[k] && active_[k]) drawVm(sink, vm_[k], none);
        }
    }
    if (descShown_) drawVm(sink, desc_[static_cast<std::size_t>(currentDesc_)], none);
    if (state_ == State::PracticeStage && instancesMade_) {
        const AnmVm& at = vm_[131];
        const float x = at.position.x, y0 = at.position.y;
        const int shot = ctx_.shotIndex(), d = std::clamp<int>(ctx_.config->difficulty, 0, 3);
        const int k = ctx_.scores->practiceStages(shot, d);
        text_.add(x, y0, "Stage    HI-Score", 0xFFFFFFFFu);
        for (int i = 0; i < 6; ++i) {
            const PracticeRecord& r = ctx_.scores->practiceAt(shot, i, d);
            const std::uint32_t color = i == cursor_ ? 0xFFFFFFFFu : i < k ? 0xFFA0A0A0u : 0xFF404040u;
            text_.add(x, y0 + 16.0f * static_cast<float>(i + 1), std::format("Stage{}  {:9d}0({:3d})", i + 1, r.score, r.plays), color);
        }
    }
    if (state_ == State::Replay || state_ == State::ReplayLeave) {
        text_.add(96.0f, 200.0f, "Replays are not available", 0xFFFFFFFFu);
        text_.add(96.0f, 220.0f, "in this port yet.", 0xFFFFFFFFu);
    }
    text_.draw(sink, *ctx_.anm);
    if (fade_ >= 0) fillScreen(sink, ctx_.ui, (static_cast<std::uint32_t>(255 - fade_ * 255 / 70) << 24) | 0xFFFFFFu);
}

}  // namespace touhou::th07
