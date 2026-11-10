#include "th08/MainMenu.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "th08/Bgm.hpp"
#include "th08/ExeTables.hpp"
#include "th08/GameConfig.hpp"
#include "th08/ScoreFile.hpp"
#include "th08/SoundPlayer.hpp"

namespace touhou::th08 {
namespace {

constexpr int kDescScript = 9;                  // text.anm
constexpr std::uint32_t kDescColor = 0xFFF0E0;  // GDI 0x00BBGGRR (shadow 0x300000: the thick outline)
constexpr std::uint32_t kGrey = 0xFF404040u;
constexpr int kFadeFrames = 70;

// title01.anm VMs (spec 3.1).
constexpr int kTitleRowVm = 1, kOptionRowVm = 10, kLivesVm = 20, kColourVm = 27, kBgmVm = 29, kBgmVolVm = 32, kSeVolVm = 36,
              kWindowVm = 40, kSlowVm = 42, kDifficultyVm = 131, kExtraCardVm = 135;
constexpr int kDigitSprite = 39;  // '0'; digit d is sprite 39 + 2d, its dim version + 1

// The character cards (exe table 0x4B8CD8, spec 4.4): name card, first and second portrait,
// and a solo character's partner portrait (dimmed); -1 = none.
struct Cards {
    int name, first, second, partner;
};
constexpr Cards kCards[12] = {
    {119, 111, 112, -1}, {120, 114, 113, -1}, {121, 115, 116, -1}, {122, 118, 117, -1},
    {123, 111, -1, 112}, {124, 112, -1, 111}, {125, 114, -1, 113}, {126, 113, -1, 114},
    {127, 115, -1, 116}, {128, 116, -1, 115}, {129, 118, -1, 117}, {130, 117, -1, 118},
};

int wrap(int v, int n) { return ((v % n) + n) % n; }

bool isDifficultyState(MainMenu::State s) {
    return s == MainMenu::State::Difficulty || s == MainMenu::State::PracticeDifficulty || s == MainMenu::State::ExtraDifficulty;
}
bool isCharacterState(MainMenu::State s) {
    return s == MainMenu::State::Character || s == MainMenu::State::PracticeCharacter || s == MainMenu::State::ExtraCharacter;
}

}  // namespace

bool MainMenu::create(std::string* error) {
    if (!ctx_.vfs || !ctx_.anm || !ctx_.textures) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx_.anm;
    if (!lib.load(*ctx_.vfs, *ctx_.textures, "title01.anm", kSlotTitle, error)) return false;
    loadBackground("title/title00.png");
    // The 14 description VMs: text.anm script 9, each on that script's sprite + i.
    for (int i = 0; i < kDescriptions; ++i) {
        AnmVm& d = desc_[static_cast<std::size_t>(i)];
        d.startFresh(lib, *ctx_.rng, kSlotText, kDescScript);
        d.selectSprite(d.activeSprite + i);
        d.set(AnmVm::Visible, false);
    }
    currentDesc_ = 0;
    descShown_ = false;
    // The cursor from the previous scene (spec 3.2 step 8).
    switch (ctx_.previous) {
        case Scene::ResultViewer: titleCursor_ = PlayerData; break;
        case Scene::MusicRoom: titleCursor_ = MusicRoom; break;
        case Scene::Game:
        case Scene::NextStage:
        case Scene::ResultAfterGame: titleCursor_ = ctx_.config && ctx_.config->difficulty >= 4 ? ExtraStart : GameStart; break;
        default: titleCursor_ = GameStart; break;
    }
    // Step 9: after a practice or spell-practice game (finished or quit) the menus walk back to
    // the stage list or the card list (spec 4.7); the flags are cleared until then.
    autoReturn_ = ctx_.practice ? (ctx_.spellPractice ? 2 : 1) : 0;
    if (autoReturn_ != 0) {
        titleCursor_ = autoReturn_ == 2 ? SpellPractice : PracticeStart;
        ctx_.practice = ctx_.spellPractice = false;
    }
    cursor_ = titleCursor_;
    // Step 3: after a playback the character is reset; after a demo the replay flag is dropped
    // (so the title, not the replay menu, comes back). Step 11: the demo flag is cleared.
    if (ctx_.replayFlag) {
        ctx_.character = 0;
        if (ctx_.demo) ctx_.replayFlag = false;
    }
    ctx_.demo = false;
    // Step 6: the Last Word unlocks; step 11.2: resulttext.anm (the spell practice lists).
    if (ctx_.records && ctx_.tables) ctx_.records->updateLastWords(ctx_.tables->bossFinal, ctx_.tables->cardCategories[1]);
    {
        std::string ignored;
        resultTextLoaded_ = lib.load(*ctx_.vfs, *ctx_.textures, "resulttext.anm", kSlotResultText, &ignored, true);
    }
    if (ctx_.music && ctx_.previous != Scene::ResultViewer) ctx_.music->load(8, "bgm/th08_01.mid");
    fade_ = kFadeFrames;  // the white fade-in (screen effect type 0, 70 frames)
    state_ = prevState_ = State::Title;
    sub_ = 0;
    firstEntry_ = true;
    return true;
}

// ---- helpers --------------------------------------------------------------------------------

void MainMenu::setState(State s) {
    prevState_ = state_;
    state_ = s;
    sub_ = 0;
    stateTimer_ = inputTimer_ = idle_ = 0;
    if (s == State::Title) cursor_ = titleCursor_;
}

void MainMenu::loadBackground(const char* path) {
    if (background_.loaded() && background_.path() == path) return;
    if (!background_.load(*ctx_.vfs, *ctx_.textures, path)) log::warn("{} could not be loaded", path);
}

bool MainMenu::rowLocked(int row) const {
    if (row == ExtraStart) return !ctx_.extraUnlocked;
    if (row == SpellPractice) return !ctx_.spellPracticeUnlocked;
    return false;
}

void MainMenu::sendAll(int interrupt) {
    for (int i = 0; i < kInstances; ++i) {
        if (has_[static_cast<std::size_t>(i)]) vm_[static_cast<std::size_t>(i)].interrupt(interrupt);
    }
}

// The selected VM on its base sprite, the others on base + 1.
void MainMenu::highlightRows(int first, int count, int selected) {
    for (int i = first; i < first + count && i < kInstances; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (has_[k]) vm_[k].selectSprite(base_[k] + (i == selected ? 0 : 1));
    }
}

void MainMenu::showDescription(int row) {
    currentDesc_ = std::clamp(row, 0, kDescriptions - 1);
    desc_[static_cast<std::size_t>(currentDesc_)].interrupt(1);
    descShown_ = true;
}

void MainMenu::hideDescription() { desc_[static_cast<std::size_t>(currentDesc_)].interrupt(2); }

void MainMenu::notPorted() { notice_ = 150; }

void MainMenu::restartTitleSong() {
    if (!ctx_.music) return;
    ctx_.music->stop();
    ctx_.music->setMode(ctx_.config ? static_cast<int>(ctx_.config->music) : static_cast<int>(BgmPlayer::Wav));
    ctx_.music->load(8, "bgm/th08_01.mid");
    ctx_.music->playLoaded(8);
    ctx_.notePlayed("bgm/th08_01.mid");
}

// ---- title (state 0) ------------------------------------------------------------------------

void MainMenu::enterTitle() {
    AnmLibrary& lib = *ctx_.anm;
    if (firstEntry_ && ctx_.previous != Scene::ResultViewer) {
        if (ctx_.music) ctx_.music->playLoaded(8);
        ctx_.notePlayed("bgm/th08_01.mid");
    }
    // A screen that changed the background (or the first entry) brings title00.png back.
    if (prevState_ == State::Title || isDifficultyState(prevState_) || prevState_ == State::Replays || prevState_ == State::SpellCharacter ||
        prevState_ == State::SpellStage) {
        loadBackground("title/title00.png");
    }
    if (!instancesMade_) {
        for (int i = 0; i < kInstances; ++i) {
            const auto k = static_cast<std::size_t>(i);
            vm_[k] = AnmVm{};
            has_[k] = lib.file(kSlotTitle) && lib.file(kSlotTitle)->script(i) != nullptr;
            if (!has_[k]) continue;
            vm_[k].startFresh(lib, *ctx_.rng, kSlotTitle, i);
            base_[k] = vm_[k].activeSprite;
        }
        instancesMade_ = true;
    }
    firstEntry_ = false;
    sendAll(2);
    highlightRows(kTitleRowVm, kTitleRows, kTitleRowVm + cursor_);
    for (int r = 0; r < kTitleRows; ++r) {
        if (rowLocked(r) && has_[static_cast<std::size_t>(kTitleRowVm + r)]) vm_[static_cast<std::size_t>(kTitleRowVm + r)].color = kGrey;
    }
}

void MainMenu::title(const MenuInput& in) {
    if (sub_ == 0) {
        if (inputTimer_ == 0) enterTitle();
        if (ctx_.replayFlag) {  // step 5: a playback just ended: the replay menu
            ctx_.replayFlag = false;
            enterReplays(false);
            return;
        }
        if (autoReturn_ == 1) {  // state 8 next (difficulty, one pass)
            setState(State::PracticeDifficulty);
            sendAll(5);
            return;
        }
        if (autoReturn_ == 2) {  // state 15 (character, one pass)
            ctx_.spellPractice = true;
            setState(State::SpellCharacter);
            sendAll(5);
            return;
        }
        // One description per pass, rows 0..8 (TH07 rendered them all at once).
        const int k = inputTimer_;
        if (k < kTitleRows) {
            const std::string_view text = ctx_.tables ? std::string_view(ctx_.tables->titleText[static_cast<std::size_t>(k)].sjis) : "";
            renderSpriteText(ctx_.textures, ctx_.text, desc_[static_cast<std::size_t>(k)], text, kDescColor, TextAlign::Centre);
            desc_[static_cast<std::size_t>(k)].set(AnmVm::Visible, false);
            return;
        }
        inputTimer_ = 0;
        stateTimer_ = 0;
        idle_ = 0;
        remembered_ = -1;
        sub_ = 1;
    }
    if (sub_ == 2) {  // quitting
        if (stateTimer_ >= 60) {
            removed_ = true;
            if (ctx_.request) ctx_.request(Scene::Exit);
        }
        return;
    }
    if (sub_ == 3) {  // on the way to the Option screen
        if (stateTimer_ >= 30) {
            setState(State::Options);
            cursor_ = 0;
            if (ctx_.config) {
                savedColour_ = ctx_.config->colourMode;
                savedWindow_ = ctx_.config->windowed;
            }
        }
        return;
    }
    if (sub_ != 1) return;

    // Vertical move over the nine rows, skipping the locked ones.
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        cursor_ = wrap(cursor_ + d, kTitleRows);
        for (int guard = 0; guard < kTitleRows && rowLocked(cursor_); ++guard) cursor_ = wrap(cursor_ + d, kTitleRows);
        ctx_.playSound(kSndMove);
        highlightRows(kTitleRowVm, kTitleRows, kTitleRowVm + cursor_);
        for (int r = 0; r < kTitleRows; ++r) {
            if (rowLocked(r) && has_[static_cast<std::size_t>(kTitleRowVm + r)]) vm_[static_cast<std::size_t>(kTitleRowVm + r)].color = kGrey;
        }
    }
    // Idle: the attract demo after 1500 passes (spec_replays 5.1).
    if (in.current() != 0) {
        idle_ = 0;
    } else if (++idle_ > 1500) {
        if (ctx_.attractDemo && startDemo()) return;
        idle_ = 0;
    }
    if (cursor_ != remembered_) {
        showDescription(cursor_);
        remembered_ = cursor_;
    }
    if (inputTimer_ < 10) return;
    if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        switch (cursor_) {
            case GameStart:
                ctx_.practice = ctx_.spellPractice = false;
                titleCursor_ = GameStart;
                setState(State::Difficulty);
                sendAll(5);
                hideDescription();
                break;
            case ExtraStart:
                ctx_.practice = ctx_.spellPractice = false;
                titleCursor_ = ExtraStart;
                setState(State::ExtraDifficulty);
                cursor_ = 0;
                sendAll(5);
                hideDescription();
                break;
            case SpellPractice:
                ctx_.practice = ctx_.spellPractice = true;
                titleCursor_ = SpellPractice;
                setState(State::SpellStage);
                cursor_ = ctx_.character;
                sendAll(5);
                hideDescription();
                break;
            case PracticeStart:
                ctx_.practice = true;
                ctx_.spellPractice = false;
                titleCursor_ = PracticeStart;
                setState(State::PracticeDifficulty);
                sendAll(5);
                hideDescription();
                break;
            case Option:
                cursor_ = 0;
                sub_ = 3;
                stateTimer_ = 0;
                break;
            case Quit:
                sub_ = 2;
                stateTimer_ = 0;
                sendAll(1);
                break;
            case MusicRoom:
                hideDescription();
                removed_ = true;
                if (ctx_.request) ctx_.request(Scene::MusicRoom);
                break;
            case PlayerData:
                hideDescription();
                removed_ = true;
                if (ctx_.request) ctx_.request(Scene::ResultViewer);
                break;
            case Replay:
                titleCursor_ = Replay;
                enterReplays(false);  // (SE 10 played above)
                break;
            default:
                break;
        }
        return;
    }
    if (in.pressed(kCancel)) {
        cursor_ = Quit;
        highlightRows(kTitleRowVm, kTitleRows, kTitleRowVm + cursor_);
        ctx_.playSound(kSndBack);
    }
}

// ---- Option (state 2) -----------------------------------------------------------------------

void MainMenu::showOptionValues() {
    if (!ctx_.config) return;
    GameConfig& c = *ctx_.config;
    const int maxLives = ctx_.totalPlays >= 60 ? 6 : ctx_.totalPlays >= 30 ? 5 : 4;
    highlightRows(kLivesVm, 7, kLivesVm + c.lives);
    for (int i = maxLives + 1; i <= 6; ++i) vm_[static_cast<std::size_t>(kLivesVm + i)].set(AnmVm::Visible, false);
    highlightRows(kColourVm, 2, kColourVm + c.colourMode);
    highlightRows(kBgmVm, 3, kBgmVm + c.music);
    highlightRows(kWindowVm, 2, kWindowVm + c.windowed);
    highlightRows(kSlowVm, 2, kSlowVm + c.slowMode);
    // The volumes: the hundreds digit only at 100, the tens digit from 10, then the units
    // and the fourth sprite; bright while the cursor is on the row.
    auto digits = [&](int vm, int value, bool lit) {
        const int dim = lit ? 0 : 1;
        AnmVm* v = &vm_[static_cast<std::size_t>(vm)];
        v[0].selectSprite(base_[static_cast<std::size_t>(vm)] + dim);
        if (value < 100) v[0].set(AnmVm::Visible, false);
        v[1].selectSprite(kDigitSprite + 2 * ((value / 10) % 10) + dim);
        if (value < 10) v[1].set(AnmVm::Visible, false);
        v[2].selectSprite(kDigitSprite + 2 * (value % 10) + dim);
        v[3].selectSprite(base_[static_cast<std::size_t>(vm + 3)] + dim);
    };
    digits(kBgmVolVm, std::clamp<int>(c.bgmVolume, 0, 100), cursor_ == BgmVolume);
    digits(kSeVolVm, std::clamp<int>(c.seVolume, 0, 100), cursor_ == SeVolume);
}

void MainMenu::leaveOptions() {
    ctx_.playSound(kSndBack);
    titleCursor_ = Option;
    if (ctx_.saveConfig) ctx_.saveConfig();
    // A colour or window mode change rebuilds the device in the original; nothing to do here.
    setState(State::Title);
}

void MainMenu::options(const MenuInput& in) {
    if (!ctx_.config) {
        setState(State::Title);
        return;
    }
    GameConfig& c = *ctx_.config;
    if (sub_ == 0) {
        sendAll(3);
        highlightRows(kOptionRowVm, kOptionRows, kOptionRowVm + cursor_);
        for (int i = 0; i < kOptionRows && i < kDescriptions; ++i) {
            const std::string_view text = ctx_.tables ? std::string_view(ctx_.tables->optionText[static_cast<std::size_t>(i)].sjis) : "";
            renderSpriteText(ctx_.textures, ctx_.text, desc_[static_cast<std::size_t>(i)], text, kDescColor, TextAlign::Centre);
            desc_[static_cast<std::size_t>(i)].set(AnmVm::Visible, false);
        }
        showDescription(cursor_);
        sub_ = 1;
    }
    showOptionValues();
    if (ctx_.sound) ctx_.sound->setVolumePercent(c.seVolume);
    if (ctx_.music) ctx_.music->setVolumePercent(c.bgmVolume);
    if (inputTimer_ < 4) return;

    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        cursor_ = wrap(cursor_ + d, kOptionRows);
        ctx_.playSound(kSndMove);
        highlightRows(kOptionRowVm, kOptionRows, kOptionRowVm + cursor_);
        showDescription(cursor_);
    }
    const int lr = in.pressedOrRepeat(BtnLeft) ? -1 : in.pressedOrRepeat(BtnRight) ? 1 : 0;
    const int maxLives = ctx_.totalPlays >= 60 ? 6 : ctx_.totalPlays >= 30 ? 5 : 4;
    switch (cursor_) {
        case Lives:
            if (lr) {
                c.lives = static_cast<std::uint8_t>(wrap(c.lives + lr, maxLives + 1));
                ctx_.playSound(kSndMove);
            }
            break;
        case Colour:
            if (lr) {
                c.colourMode = static_cast<std::uint8_t>(wrap(c.colourMode + lr, 2));
                ctx_.playSound(kSndMove);
            }
            break;
        case BgmMode:
            if (lr) {
                c.music = static_cast<std::uint8_t>(wrap(c.music + lr, 3));
                ctx_.playSound(kSndMove);
                restartTitleSong();
            }
            break;
        case Window:
            if (lr) {
                c.windowed = static_cast<std::uint8_t>(wrap(c.windowed + lr, 2));
                ctx_.playSound(kSndMove);
            }
            break;
        case Slow:
            if (lr) {
                c.slowMode = static_cast<std::uint8_t>(wrap(c.slowMode + lr, 2));
                ctx_.playSound(kSndMove);
            }
            break;
        case BgmVolume:
        case SeVolume: {
            // Pressed: 4, and in the same pass the second rule's 1 (a press moves 5); held for
            // 30 passes or more: 1 per pass.
            int delta = 0;
            if (in.pressed(BtnLeft)) delta -= 4;
            if (in.pressed(BtnRight)) delta += 4;
            if (in.pressed(BtnLeft) || (in.held(BtnLeft) && in.holdCount() >= 30)) delta -= 1;
            if (in.pressed(BtnRight) || (in.held(BtnRight) && in.holdCount() >= 30)) delta += 1;
            std::int8_t& vol = cursor_ == BgmVolume ? c.bgmVolume : c.seVolume;
            vol = static_cast<std::int8_t>(std::clamp(vol + delta, 0, 100));
            if (inputTimer_ % 50 == 0) ctx_.playSound(kSndTimeout);  // the test tone
            break;
        }
        default:
            break;
    }
    if (in.current() != 0) idle_ = 0;
    else if (++idle_ >= 3600) {
        leaveOptions();
        return;
    }
    if (in.pressed(kSelect)) {
        switch (cursor_) {
            case Reset:
                c.lives = 2;
                c.bombs = 3;
                c.music = GameConfig::MusicWav;
                c.sound = 1;
                c.slowMode = 0;
                ctx_.playSound(kSndSelect);
                return;
            case KeyConfig:
                ctx_.playSound(kSndSelect);
                notPorted();  // not needed with touch controls; a pad is mapped by Android
                return;
            case OptionExit:
                leaveOptions();
                return;
            default:
                break;
        }
    }
    if (in.pressed(kCancel)) {
        if (cursor_ == OptionExit) {
            leaveOptions();
            return;
        }
        cursor_ = OptionExit;
        highlightRows(kOptionRowVm, kOptionRows, kOptionRowVm + cursor_);
        showDescription(cursor_);
        ctx_.playSound(kSndBack);
    }
}

// ---- difficulty select (states 4, 8, 12) ------------------------------------------------------

void MainMenu::highlightDifficulty() {
    for (int i = 0; i < 4; ++i) {
        const auto k = static_cast<std::size_t>(kDifficultyVm + i);
        if (!has_[k]) continue;
        vm_[k].selectSprite(base_[k] + (i == cursor_ ? 0 : 1));
        vm_[k].interrupt(i == cursor_ ? 24 : 25);
    }
}

void MainMenu::difficulty(const MenuInput& in) {
    if (!ctx_.config) return;
    const bool extra = state_ == State::ExtraDifficulty;
    if (sub_ == 0) {
        if (!isCharacterState(prevState_)) loadBackground("title/select00.png");
        cursor_ = ctx_.config->difficulty;
        if (extra) {
            sendAll(12);
            cursor_ = 0;
            highlightRows(kExtraCardVm, 1, kExtraCardVm);
        } else {
            sendAll(7);
            if (cursor_ >= 4) cursor_ = 1;
            highlightDifficulty();
        }
        sub_ = 1;
        if (autoReturn_) {  // then the character page (one pass)
            setState(State::PracticeCharacter);
            cursor_ = 0;
            return;
        }
    }
    if (sub_ == 3) {  // backing out to the title
        if (stateTimer_ >= 20) {
            ctx_.practice = false;
            setState(State::Title);
        }
        return;
    }
    if (inputTimer_ < 8) return;
    const int rows = extra ? 1 : 4;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        cursor_ = wrap(cursor_ + d, rows);
        ctx_.playSound(kSndMove);
        if (!extra) highlightDifficulty();
    }
    if (in.pressed(kSelect)) {
        ctx_.config->difficulty = static_cast<std::uint8_t>(cursor_ + (extra ? 4 : 0));
        ctx_.playSound(kSndSelect);
        const State next = state_ == State::Difficulty ? State::Character
                           : state_ == State::PracticeDifficulty ? State::PracticeCharacter
                                                                 : State::ExtraCharacter;
        setState(next);
        cursor_ = 0;
        return;
    }
    if (in.pressed(kCancel)) {
        ctx_.config->difficulty = static_cast<std::uint8_t>(cursor_ + (extra ? 4 : 0));
        ctx_.playSound(kSndBack);
        sub_ = 3;
        stateTimer_ = 0;
        sendAll(6);
    }
}

// ---- character select (states 5, 9, 13) -------------------------------------------------------

void MainMenu::showCards(bool afterMove, int character) {
    const Cards& c = kCards[std::clamp(character >= 0 ? character : cursor_, 0, 11)];
    for (int i = 111; i <= 130; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (!has_[k]) continue;
        const bool main = i == c.name || i == c.first || i == c.second;
        const bool partner = i == c.partner;
        if (!afterMove) {
            vm_[k].set(AnmVm::Visible, main || partner);
            vm_[k].interrupt(main ? 9 : partner ? 23 : 8);
        } else {
            vm_[k].set(AnmVm::Visible, true);
            vm_[k].interrupt(main ? 9 : partner ? 23 : 8);
        }
    }
}

void MainMenu::character(const MenuInput& in) {
    if (!ctx_.config) return;
    const int columns = ctx_.solosUnlocked ? 12 : 4;
    if (sub_ == 0) {
        cursor_ = ctx_.character < columns ? ctx_.character : 0;
        sendAll(8);
        const int card = kDifficultyVm + std::clamp<int>(ctx_.config->difficulty, 0, 4);
        if (has_[static_cast<std::size_t>(card)]) vm_[static_cast<std::size_t>(card)].interrupt(9);
        showCards(false);
        sub_ = 1;
        if (autoReturn_) {  // then the stage list, on the stage just played
            autoReturn_ = false;
            setState(State::PracticeStage);
            cursor_ = ctx_.stage <= 7 ? ctx_.stage : 0;
            return;
        }
    }
    if (inputTimer_ < 8) return;
    const int lr = in.pressedOrRepeat(BtnLeft) ? -1 : in.pressedOrRepeat(BtnRight) ? 1 : 0;
    if (lr != 0) {
        cursor_ = wrap(cursor_ + lr, columns);
        ctx_.playSound(kSndMove);
        showCards(true);
    }
    if (in.pressed(kSelect)) {
        ctx_.character = cursor_;
        ctx_.playSound(kSndSelect);
        if (state_ == State::PracticeCharacter) {  // on to the stage list, at once
            setState(State::PracticeStage);
            cursor_ = ctx_.stage <= 7 ? ctx_.stage : 0;
            practiceStage(in);
            return;
        }
        ctx_.stage = state_ == State::ExtraCharacter ? 8 : 0;
        ctx_.practice = state_ == State::PracticeCharacter;
        ctx_.spellPractice = false;
        ctx_.replayFlag = false;
        if (ctx_.music) ctx_.music->stop();
        removed_ = true;
        if (ctx_.request) ctx_.request(Scene::Game);
        return;
    }
    if (in.pressed(kCancel)) {
        ctx_.playSound(kSndBack);
        ctx_.character = cursor_;
        const State back = state_ == State::Character ? State::Difficulty
                           : state_ == State::PracticeCharacter ? State::PracticeDifficulty
                                                                : State::ExtraDifficulty;
        setState(back);
        cursor_ = 0;
    }
}

// ---- practice stage select (state 11) ---------------------------------------------------------

// A stage can be practised once it was cleared on this difficulty with this character (the
// CLRD reached word; Stage 1 always); a 6B clear opens both Stage 4 variants (spec 4.5).
bool MainMenu::stageSelectable(int stage) const {
    std::uint16_t mask = 1;
    if (ctx_.scores && ctx_.config) {
        const int c = std::clamp(ctx_.character, 0, kCharacters - 1);
        mask = ctx_.scores->clears[static_cast<std::size_t>(c)].reached[std::clamp<std::size_t>(ctx_.config->difficulty, 0, 4)];
        if (mask == 0) mask = 1;
        if (mask & 0x80) mask |= 0x18;
    }
    return ((mask >> stage) & 1) != 0;
}

void MainMenu::practiceStage(const MenuInput& in) {
    if (sub_ == 0) {
        sendAll(18);
        cursor_ = std::clamp(cursor_, 0, 7);
        showCards(false);
        ctx_.practice = true;
        sub_ = 1;
    }
    if (!stageSelectable(cursor_)) cursor_ = 0;
    if (inputTimer_ < 8) return;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        int c = cursor_;
        for (int i = 0; i < 8; ++i) {
            c = wrap(c + d, 8);
            if (stageSelectable(c)) break;
        }
        if (c != cursor_) {
            cursor_ = c;
            ctx_.playSound(kSndMove);
        }
    }
    if (in.pressed(kSelect)) {
        ctx_.playSound(kSndSelect);
        ctx_.stage = cursor_;
        ctx_.practice = true;
        ctx_.spellPractice = false;
        ctx_.replayFlag = false;
        if (ctx_.music) ctx_.music->stop();
        removed_ = true;
        if (ctx_.request) ctx_.request(Scene::Game);
        return;
    }
    if (in.pressed(kCancel)) {
        ctx_.playSound(kSndBack);
        setState(State::PracticeCharacter);
        cursor_ = ctx_.character;
        character(in);
    }
}

// The list (spec 4.5 drawing): "Stage    HI-Score", then each stage's best practice score and
// play count; the cursor row white, selectable rows grey, locked rows dark.
void MainMenu::drawStageList() {
    static constexpr const char* kLabels[8] = {"Stage1 ", "Stage2 ", "Stage3 ", "Stage4A", "Stage4B", "Stage5 ", "Stage6A", "Stage6B"};
    // (The spec anchors the list to VM 139, whose script stays at the top of the screen; VM
    // 141, the same sprite, slides in to (256, 320) on interrupt 18. Until the anchor is
    // settled the list sits left of the character's card, 96 px above that point.)
    float x = 256.0f, y = 320.0f;
    if (has_[141]) {
        x = vm_[141].position.x;
        y = vm_[141].position.y;
    }
    x -= 208.0f;
    y -= 96.0f;
    ascii_.add(x, y, "Stage    HI-Score", 0xFFFFFFFFu);
    const int c = std::clamp(ctx_.character, 0, kCharacters - 1);
    const std::size_t d = ctx_.config ? std::clamp<std::size_t>(ctx_.config->difficulty, 0, 4) : 1;
    for (int k = 0; k < 8; ++k) {
        std::uint32_t best = 0, plays = 0;
        if (ctx_.scores) {
            const PracticeRecord& r = ctx_.scores->practice[static_cast<std::size_t>(c)];
            best = r.best[static_cast<std::size_t>(k) * 5 + d];
            plays = r.plays[static_cast<std::size_t>(k) * 5 + d];
        }
        const std::uint32_t color = k == cursor_ ? 0xFFFFFFFFu : stageSelectable(k) ? 0xFFA0A0A0u : 0xFF404040u;
        ascii_.add(x, y + 16.0f * static_cast<float>(k + 1), std::format("{} {:9d}0 ({:3d})", kLabels[k], best, plays), color);
    }
}

// ---- the pass and the picture ----------------------------------------------------------------

void MainMenu::update(const MenuInput& in) {
    if (removed_) return;
    switch (state_) {
        case State::Title: title(in); break;
        case State::Options: options(in); break;
        case State::Difficulty:
        case State::PracticeDifficulty:
        case State::ExtraDifficulty: difficulty(in); break;
        case State::Character:
        case State::PracticeCharacter:
        case State::ExtraCharacter: character(in); break;
        case State::PracticeStage: practiceStage(in); break;
        case State::SpellCharacter: spellCharacter(in); break;
        case State::SpellStage: spellStage(in); break;
        case State::SpellCards: spellCards(in); break;
        case State::Replays: replays(in); break;
        default: setState(State::Title); break;
    }
    // After the state handler: all 142 VMs and the current description advance.
    for (int i = 0; i < kInstances; ++i) {
        if (has_[static_cast<std::size_t>(i)]) vm_[static_cast<std::size_t>(i)].tick(*ctx_.rng);
    }
    if (descShown_) desc_[static_cast<std::size_t>(currentDesc_)].tick(*ctx_.rng);
    ++inputTimer_;
    ++stateTimer_;
    if (fade_ > 0) --fade_;
    if (notice_ > 0) --notice_;
}

void MainMenu::draw(SpriteSink& sink) {
    if (removed_) return;
    background_.blit(sink, 0, 0);
    const AnmDrawState none;
    if (instancesMade_) {
        for (int i = 0; i < kInstances; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (has_[k]) drawVm(sink, vm_[k], none);
        }
    }
    if (descShown_) drawVm(sink, desc_[static_cast<std::size_t>(currentDesc_)], none);
    if (state_ == State::PracticeStage) drawStageList();
    if (state_ == State::SpellStage) drawSpellStage(sink);
    if (state_ == State::SpellCards) drawSpellCards(sink);
    if (state_ == State::Replays) drawReplays(sink);
    if (notice_ > 0) {
        const std::uint32_t a = static_cast<std::uint32_t>(std::min(255, notice_ * 8)) << 24;
        ascii_.add(160, 448, "NOT PORTED YET", a | 0x00FFE0A0u);
    }
    if (ctx_.anm) ascii_.draw(sink, *ctx_.anm);
    if (fade_ > 0) fillScreen(sink, ctx_.ui, (static_cast<std::uint32_t>(fade_ * 255 / kFadeFrames) << 24) | 0x00FFFFFFu);
}

}  // namespace touhou::th08
