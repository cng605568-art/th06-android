#include "th07/MainMenu.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th07/Bgm.hpp"
#include "th07/English.hpp"
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
    idle_ = 0;
    entered_ = false;
    stateChanged_ = true;
}

bool MainMenu::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures || !ctx_.scores || !ctx_.config) {
        if (error) *error = "no game files";
        return false;
    }
    ScoreData& sd = *ctx_.scores;
    // A replay or the demo resets the remembered character (the shot type stays); after a
    // replay (not the demo) the menu opens on the replay list.
    if (ctx_.replay) {
        ctx_.character = 0;
        if (ctx_.demo) ctx_.replay = false;
    }
    afterReplay_ = ctx_.replay;
    ctx_.replayFile.reset();
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

template <std::size_t N>
void MainMenu::renderDescriptions(const std::array<ExeTables::MenuText, N>* exe, const char* const* fallback) {
    for (std::size_t i = 0; i < N && i < static_cast<std::size_t>(kDescriptions); ++i) {
        AnmVm& d = desc_[i];
        // The original's Shift-JIS text; in English mode the patch's translation, or the
        // port's own English when the patch has none for it (thpatch's lang_en has none).
        std::string text = fallback[i];
        if (exe && !(*exe)[i].sjis.empty()) {
            if (ctx_.english && ctx_.english->active()) {
                if (const auto t = ctx_.english->exeString((*exe)[i].address)) text = English::gameText(*t);
            } else {
                text = (*exe)[i].sjis;
            }
        }
        renderSpriteText(ctx_.textures, ctx_.text, d, text, kDescColor, TextAlign::Centre);
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
        case State::ReplayStages: return replayStages(in);
        case State::ReplayMode: return replayMode(in);
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
        if (afterReplay_) {
            afterReplay_ = false;
            ctx_.replay = false;
            sendAll(13);
            hideDescription();
            setState(State::Replay);
            return false;
        }
        if (autoPractice_) {
            variant_ = Variant::Practice;
            sendAll(5);
            setState(State::Difficulty);
            return false;
        }
        renderDescriptions(ctx_.tables ? &ctx_.tables->mainMenuText : nullptr, kMainDesc);
        showDescription(cursor_);
    }
    const int moved = verticalMove(in, 8);
    if (moved != 0) {
        if (!sd.extraUnlocked() && cursor_ == 1) cursor_ = wrap(cursor_ + moved, 8);
        highlightRows(1, 8, cursor_ + 1);
        showDescription(cursor_);
    }
    // The attract demo after more than 900 passes with no button held (spec 3.5).
    if (in.current() != 0) idle_ = 0;
    else if (++idle_ > 900 && tryDemo()) return false;
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
        renderDescriptions(ctx_.tables ? &ctx_.tables->optionText : nullptr, kOptionDesc);
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
        renderDescriptions(ctx_.tables ? &ctx_.tables->keyConfigText : nullptr, kKeyDesc);
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

// ---- the replay menu (spec 3.10) and the attract demo (3.5) ---------------------------------

namespace {

std::optional<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

std::vector<MainMenu::ReplayItem> MainMenu::scanReplays(const std::string& dir, std::span<const std::uint8_t> verFile) {
    std::vector<ReplayItem> items;
    if (dir.empty()) return items;
    auto add = [&](const std::filesystem::path& p, std::string label) {
        const auto bytes = readFile(p);
        if (!bytes) return;
        auto f = ReplayFile::parse(*bytes, verFile, nullptr, true);
        if (f) items.push_back({std::move(label), p.string(), std::move(*f)});
    };
    for (int n = 1; n <= kReplaySlots; ++n) add(std::filesystem::path(dir) / std::format("th7_{:02d}.rpy", n), std::format("No.{:02d}", n));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<std::filesystem::path> user;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() == 14 && name.starts_with("th7_ud") && name.ends_with(".rpy")) user.push_back(e.path());
    }
    std::sort(user.begin(), user.end());
    int added = 0;
    for (const auto& p : user) {
        if (added >= kReplayUserFiles) break;
        const std::size_t before = items.size();
        add(p, "User ");
        if (items.size() > before) ++added;
    }
    return items;
}

bool MainMenu::tryDemo() {
    ctx_.demoIndex = (ctx_.demoIndex + 1) % 3;
    const auto bytes = ctx_.vfs ? ctx_.vfs->read(std::format("data/demo/demorpy{}.rpy", ctx_.demoIndex)) : std::nullopt;
    std::string err;
    auto f = bytes ? ReplayFile::parse(*bytes, ctx_.verFile, &err) : std::nullopt;
    if (!f || f->firstStage() < 0) {
        if (bytes) log::warn("Demo {}: {}", ctx_.demoIndex, f ? "no stage" : err);
        idle_ = 0;
        return false;
    }
    ctx_.replay = ctx_.demo = true;
    ctx_.playbackMode = 0;
    ctx_.character = std::clamp(f->summary.shot(), 0, 5) / 2;
    ctx_.shotType = f->summary.shot() & 1;
    ctx_.stage = f->firstStage();
    ctx_.replayFile = std::make_shared<ReplayFile>(std::move(*f));
    hideDescription();
    ctx_.request(FlowState::Game);
    done_ = true;
    return true;
}

bool MainMenu::replay(const MenuInput& in) {
    if (!entered_) {
        entered_ = true;
        // Sub-state 0: the list is rebuilt (not when coming back from the stage list).
        if (prevState_ != State::ReplayStages) {
            if (prevState_ != State::Replay && prevState_ != State::ReplayLeave) loadBackground("data/title/select00.jpg");
            sendAll(14);
            cursor_ = 0;
            replays_ = scanReplays(ctx_.replayDir, ctx_.verFile);
        } else {
            sendAll(14);
            cursor_ = replayIndex_;
            timer_ = 30;  // (the scan's 30 passes are over)
        }
        chosen_.reset();
        return false;
    }
    if (timer_ < 30) return false;
    const int n = static_cast<int>(replays_.size());
    if (verticalMove(in, n) == 0 && n > 15) {
        if (in.pressedOrRepeat(BtnLeft)) cursor_ = wrap(cursor_ - 15, n), ctx_.playSound(kSndMove);
        else if (in.pressedOrRepeat(BtnRight)) cursor_ = wrap(cursor_ + 15, n), ctx_.playSound(kSndMove);
    }
    if (in.pressed(kSelect) && timer_ >= 40 && n > 0) {
        const ReplayItem& item = replays_[static_cast<std::size_t>(cursor_)];
        // The whole file, validated again (it may have changed since the scan).
        const auto bytes = readFile(item.path);
        std::string err;
        auto f = bytes ? ReplayFile::parse(*bytes, ctx_.verFile, &err) : std::nullopt;
        if (!f || f->firstStage() < 0) {
            log::warn("Replay {}: {}", item.path, f ? "no stage" : err);
            ctx_.playSound(kSndBack);
            return false;
        }
        ctx_.playSound(kSndSelect);
        sendAll(15);
        const int row = 135 + cursor_ % 15;
        if (has_[static_cast<std::size_t>(row)]) vm_[static_cast<std::size_t>(row)].interrupt(17);
        replayIndex_ = cursor_;
        chosen_ = std::make_shared<ReplayFile>(std::move(*f));
        cursor_ = chosen_->firstStage();
        setState(State::ReplayStages);
        return false;
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        sendAll(16);
        replays_.clear();
        setState(State::ReplayLeave);
    }
    return false;
}

bool MainMenu::replayStages(const MenuInput& in) {
    if (!chosen_) {
        setState(State::Replay);
        return false;
    }
    if (!entered_) {
        entered_ = true;
        return false;
    }
    // Absent stages are skipped in the direction of the move.
    const int d = verticalMove(in, kReplayStages);
    for (int k = 0; d != 0 && k < kReplayStages && !chosen_->hasStage(cursor_); ++k) cursor_ = wrap(cursor_ + d, kReplayStages);
    if (in.pressed(kSelect) && chosen_->hasStage(cursor_)) {
        ctx_.playSound(kSndSelect);
        sendAll(19);
        const int row = 135 + replayIndex_ % 15;
        if (has_[static_cast<std::size_t>(row)]) vm_[static_cast<std::size_t>(row)].interrupt(17);
        ctx_.stage = cursor_;
        cursor_ = 0;
        for (int i = 0; i < 3; ++i) {
            if (has_[static_cast<std::size_t>(158 + i)]) vm_[static_cast<std::size_t>(158 + i)].interrupt(i == 0 ? 20 : 21);
        }
        setState(State::ReplayMode);
        return false;
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        chosen_.reset();
        setState(State::Replay);
    }
    return false;
}

bool MainMenu::replayMode(const MenuInput& in) {
    if (!chosen_) {
        setState(State::Replay);
        return false;
    }
    if (!entered_) {
        entered_ = true;
        return false;
    }
    if (verticalMove(in, 3) != 0) {
        for (int i = 0; i < 3; ++i) {
            if (has_[static_cast<std::size_t>(158 + i)]) vm_[static_cast<std::size_t>(158 + i)].interrupt(i == cursor_ ? 20 : 21);
        }
    }
    if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        ctx_.replay = true;
        ctx_.demo = false;
        ctx_.character = std::clamp(chosen_->summary.shot(), 0, 5) / 2;
        ctx_.shotType = chosen_->summary.shot() & 1;
        if (chosen_->summary.difficulty() == Phantasm) ctx_.stage = 7;  // the set-up makes it 8
        ctx_.playbackMode = cursor_;
        ctx_.replayFile = chosen_;
        ctx_.request(FlowState::Game);
        if (ctx_.music) ctx_.music->stop();
        done_ = true;
        return false;
    }
    if (in.pressed(kReturn)) {
        ctx_.playSound(kSndBack);
        cursor_ = ctx_.stage;
        sendAll(15);
        const int row = 135 + replayIndex_ % 15;
        if (has_[static_cast<std::size_t>(row)]) vm_[static_cast<std::size_t>(row)].interrupt(17);
        setState(State::ReplayStages);
        entered_ = true;
    }
    return false;
}

void MainMenu::drawReplayMenu() {
    if (!instancesMade_) return;
    const bool stages = state_ == State::ReplayStages || state_ == State::ReplayMode;
    if (state_ == State::Replay || stages) {
        const AnmVm& h = vm_[134];
        text_.add(h.position.x, h.position.y, "No.   Name     Date  Player  Rank", 0xFFFFFFFFu);
        const int page = (stages ? replayIndex_ : cursor_) / 15;
        for (int i = 0; i < 15; ++i) {
            const int k = page * 15 + i;
            if (k >= static_cast<int>(replays_.size())) break;
            const ReplayItem& it = replays_[static_cast<std::size_t>(k)];
            const AnmVm& at = vm_[static_cast<std::size_t>(135 + i)];
            const bool lit = stages ? k == replayIndex_ : k == cursor_;
            std::string name = it.file.summary.name();
            name.resize(8, ' ');
            std::string date = it.file.summary.date();
            date.resize(5, ' ');
            std::string diff(difficultyName(it.file.summary.difficulty()));
            diff.resize(8, ' ');
            text_.add(at.position.x, at.position.y,
                      std::format("{} {} {} {} {}", it.label, name, date, shotName7(it.file.summary.shot()), diff),
                      lit ? 0xFFFFFFFFu : 0xFF808080u);
        }
    }
    if (stages && chosen_) {
        const std::uint32_t dim = state_ == State::ReplayMode ? 0x60FFFFFFu : 0xFFFFFFFFu;
        const AnmVm& s = vm_[133];
        text_.add(s.position.x, s.position.y, std::format("Slow {:2.3f}%", chosen_->summary.slowdown()), dim);
        const AnmVm& h = vm_[150];
        text_.add(h.position.x, h.position.y, "Stage    LastScore", dim);
        static constexpr const char* kNames[6] = {"Stage1  ", "Stage2  ", "Stage3  ", "Stage4  ", "Stage5  ", "Stage6  "};
        for (int i = 0; i < kReplayStages; ++i) {
            const AnmVm& at = vm_[static_cast<std::size_t>(151 + i)];
            const std::string label = i < 6 ? kNames[i] : chosen_->summary.difficulty() == Phantasm ? "Phantasm" : "Extra   ";
            const ReplayStage& st = chosen_->stages[static_cast<std::size_t>(i)];
            const std::string score = st.present ? std::format("{:9d}0", st.endScore) : std::string("----------");
            std::uint32_t c = i == cursor_ && state_ == State::ReplayStages ? 0xFFFFFFFFu : 0xFF808080u;
            if (state_ == State::ReplayMode) c = (i == ctx_.stage || (ctx_.stage == 7 && i == 6)) ? 0x60FFFFFFu : 0x60808080u;
            text_.add(at.position.x, at.position.y, label + " " + score, c);
        }
    }
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
    drawReplayMenu();
    text_.draw(sink, *ctx_.anm);
    if (fade_ >= 0) fillScreen(sink, ctx_.ui, (static_cast<std::uint32_t>(255 - fade_ * 255 / 70) << 24) | 0xFFFFFFu);
}

}  // namespace touhou::th07
