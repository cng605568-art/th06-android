#include "game/Title.hpp"

#include <algorithm>
#include <format>
#include <memory>

#include "audio/MusicPlayer.hpp"
#include "core/Log.hpp"
#include "game/Gui.hpp"
#include "game/ScoreFile.hpp"
#include "game/World.hpp"
#include "gfx/Image.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

struct File {
    int slot;
    const char* path;
    int offset;
};
// The title pages (title01s / title04s, the pre-highlighted copies for cards without
// additive colour blending, are not needed: the port always tints).
constexpr File kTitleFiles[4] = {
    {21, "data/title01.anm", 0x100}, {24, "data/title02.anm", 0x11B},
    {25, "data/title03.anm", 0x11F}, {26, "data/title04.anm", 0x122},
};
constexpr File kSelectFiles[9] = {
    {27, "data/select01.anm", 0x150}, {28, "data/select02.anm", 0x153}, {29, "data/select03.anm", 0x15A},
    {30, "data/select04.anm", 0x15C}, {31, "data/select05.anm", 0x155}, {32, "data/slpl00a.anm", 0x156},
    {33, "data/slpl00b.anm", 0x157},  {34, "data/slpl01a.anm", 0x158}, {35, "data/slpl01b.anm", 0x159},
};
constexpr int kScriptTitle = 0x100, kScriptSelect = 0x150;
constexpr const char* kTitleTheme = "bgm/th06_01.mid";

constexpr int kSoundSelect = 10, kSoundBack = 11, kSoundMove = 12;

// Instances.
constexpr int kVmItems = 0;      // V0..V7: the main menu
constexpr int kVmSelect = 80;    // V80..V95: the select screens
constexpr int kVmDifficulty = 81, kVmExtraCard = 85, kVmPairs = 86, kVmPanels = 92;
constexpr int kVmDrawn = 98;     // V98.. are only text anchors (replays)
// The Option rows: label instance, and the instances of its values.
constexpr int kOptionRows = 9;
constexpr int kOptionLabel[kOptionRows] = {8, 9, 10, 11, 12, 72, 73, 74, 13};
constexpr int kOptionValueFirst[6] = {14, 19, 23, 77, 25, 75};
constexpr int kOptionValueCount[6] = {5, 4, 2, 3, 2, 2};
enum OptionRow { RowLives, RowBombs, RowColour, RowMusic, RowSound, RowScreen, RowDefault, RowKeys, RowQuit };

constexpr std::uint32_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kShoot = 16, kBomb = 32, kEsc = 64, kQ = 128;

int wrap(int v, int n) { return ((v % n) + n) % n; }

}  // namespace

bool Title::extraUnlocked(int shotType) const {
    if (!scores_) return false;
    const ScoreFile::Clear& c = scores_->clear(shotType);
    for (int d = Normal; d <= Lunatic; ++d) {  // an Easy clear does not count
        if (c.noContinue[static_cast<std::size_t>(d)] == ScoreFile::kStageCleared) return true;
    }
    return false;
}

bool Title::start(World& world, const Vfs& vfs, TextureLibrary& textures, const ScoreFile& scores, GameConfig& config,
                  TitleSession& session, int cursor, bool firstVisit) {
    vfs_ = &vfs;
    textures_ = &textures;
    scores_ = &scores;
    config_ = &config;
    session_ = &session;
    for (AnmVm& vm : vms_) vm = AnmVm{};
    sub_ = Sub::Startup;
    timer_ = 60;  // so a key is taken at once on a fresh title
    cursor_ = std::clamp(cursor, 0, 7);
    if (session.practice) cursor_ = 2;  // back from a practice run (or its screens)
    session.practice = false;
    optCursor_ = selCursor_ = character_ = 0;
    extraUnlocked_ = false;
    for (int t = 0; t < 4; ++t) extraUnlocked_ |= extraUnlocked(t);
    overlayColor_ = 0x40000000u;
    overlayMin_ = overlayN_ = overlayOn_ = overlayOff_ = 0;
    fadeOn_ = true;
    fadeT_ = -1;  // 255 on the first frame
    fadeN_ = firstVisit ? 200 : 120;
    fadeAlpha_ = 255;
    prevButtons_ = 0;
    repeatCounter_ = 0;
    configChanged_ = false;
    choice_ = Choice{};
    titleLoaded_ = false;
    if (!loadTitleGraphics(world)) return false;
    playTitleTheme(world);
    return true;
}

void Title::playTitleTheme(World& world) {
    if (!world.music) return;
    world.music->setEnabled(config_->music != GameConfig::MusicOff);
    world.music->play(kTitleTheme);
}

void Title::releaseFiles(World& world, int first, int last, int vmFirst, int vmLast) {
    for (int s = first; s <= last; ++s) world.anm->releaseAnm(s);
    for (int i = vmFirst; i <= vmLast; ++i) vms_[static_cast<std::size_t>(i)] = AnmVm{};
}

void Title::release(World& world) {
    releaseFiles(world, 21, 36, 0, kVms - 1);
    haveBackground_ = false;
    titleLoaded_ = false;
}

void Title::setBackground(const char* path) {
    haveBackground_ = false;
    auto bytes = vfs_->read(path);
    if (!bytes) {
        log::warn("Title: {} not found", path);
        return;
    }
    std::string err;
    auto img = decodeImage(*bytes, &err);
    if (!img) {
        log::warn("Title: {}: {}", path, err);
        return;
    }
    bgWidth_ = static_cast<float>(img->width);
    bgHeight_ = static_cast<float>(img->height);
    auto shared = std::make_shared<Image>(std::move(*img));
    background_ = textures_->add(path, shared->width, shared->height, [shared] { return std::optional<Image>(*shared); });
    haveBackground_ = true;
}

bool Title::loadTitleGraphics(World& world) {
    AnmManager& anm = *world.anm;
    releaseFiles(world, 27, 36, kVmSelect, kVms - 1);  // the select screens' files
    for (const File& f : kTitleFiles) {
        std::string err;
        anm.releaseAnm(f.slot);
        if (!anm.loadAnm(*vfs_, f.slot, f.path, f.offset, &err)) {
            log::warn("Title: {} not loaded ({})", f.path, err);
            return false;
        }
    }
    for (int k = 0; k < kVmSelect; ++k) {
        AnmVm& vm = vms_[static_cast<std::size_t>(k)];
        vm = AnmVm{};
        anm.setAndExecuteScript(vm, kScriptTitle + k);
        vm.visible = false;
        vm.baseSpriteIndex = vm.activeSpriteIndex;
        vm.zWriteDisable = true;
    }
    setBackground("data/title/title00.jpg");
    titleLoaded_ = true;
    return true;
}

bool Title::loadSelectGraphics(World& world) {
    AnmManager& anm = *world.anm;
    setBackground("data/title/select00.jpg");
    for (const File& f : kSelectFiles) {
        std::string err;
        anm.releaseAnm(f.slot);
        if (!anm.loadAnm(*vfs_, f.slot, f.path, f.offset, &err)) {
            log::error("Failed to load character/difficulty selection screen: {} ({})", f.path, err);
            return false;
        }
    }
    for (int j = 0; j < 16; ++j) {
        AnmVm& vm = vms_[static_cast<std::size_t>(kVmSelect + j)];
        vm = AnmVm{};
        anm.setAndExecuteScript(vm, kScriptSelect + j);
        vm.visible = false;
        vm.colorAdd = true;
        vm.color = 0xff000000u;
        vm.posOffset = {};
        vm.baseSpriteIndex = vm.activeSpriteIndex;
        vm.zWriteDisable = true;
    }
    return true;
}

Title::Edges Title::readInput(const InputState& in) {
    const std::uint32_t bits = (in.up ? kUp : 0) | (in.down ? kDown : 0) | (in.left ? kLeft : 0) | (in.right ? kRight : 0) |
                               (in.shoot ? kShoot : 0) | (in.bomb ? kBomb : 0) | (in.pause ? kEsc : 0) | (in.quit ? kQ : 0);
    // One repeat clock for everything: 33 passes after the last change, then every 8.
    bool tick = false;
    if (bits != prevButtons_) {
        repeatCounter_ = 0;
    } else {
        tick = repeatCounter_ >= 30 && repeatCounter_ % 8 == 0;
        if (repeatCounter_ >= 38) repeatCounter_ = 30;
        ++repeatCounter_;
    }
    // A combined button is pressed when one of its keys is down and the set that is down
    // changed (so letting go of one of two held keys counts too, as in the original).
    auto pressed = [&](std::uint32_t m) { return (bits & m) != 0 && (bits & m) != (prevButtons_ & m); };
    auto repeating = [&](std::uint32_t m) { return pressed(m) || ((bits & m) != 0 && tick); };
    Edges e;
    e.up = repeating(kUp);
    e.down = repeating(kDown);
    e.left = repeating(kLeft);
    e.right = repeating(kRight);
    e.confirm = pressed(kShoot);
    e.cancel = pressed(kBomb | kEsc);
    e.q = pressed(kQ);
    e.anyKey = repeating(kShoot) || repeating(kBomb | kEsc) || repeating(kQ);
    prevButtons_ = bits;
    return e;
}

void Title::interruptAll(std::int16_t k) {
    for (AnmVm& vm : vms_) vm.pendingInterrupt = k;
}

void Title::tint(int i, std::uint32_t color, float dx, float dy) {
    AnmVm& vm = vms_[static_cast<std::size_t>(i)];
    vm.color = color;
    vm.posOffset = {dx, dy, 0.0f};
}

int Title::overlayAlpha() const {
    const int range = static_cast<int>(overlayColor_ >> 24) - overlayMin_;
    if (overlayOn_ != 0) return range * overlayN_ / overlayOn_ + overlayMin_;
    if (overlayN_ != 0 && overlayOff_ != 0) return range * overlayN_ / overlayOff_ + overlayMin_;
    return 0;
}

void Title::stepOverlay() {
    if (overlayOn_ != 0) {
        if (overlayN_ < overlayOn_) ++overlayN_;
    } else if (overlayN_ != 0) {
        --overlayN_;
    }
}

int Title::difficultyCard() const {
    return session_->difficulty == Extra ? kVmExtraCard : kVmDifficulty + std::clamp(session_->difficulty, 0, 3);
}

// The select screens keep the current card, character and panels on screen: the interrupt
// just sent to all is taken back from the difficulty card, the other character's pair and
// the other character's two shot panels.
void Title::dropRiders(int keep) {
    const int other = 1 - keep;
    cancel(difficultyCard());
    cancel(kVmPairs + 2 * other);
    cancel(kVmPairs + 2 * other + 1);
    cancel(kVmPanels + 2 * other);
    cancel(kVmPanels + 2 * other + 1);
}

// ---- the passes ---------------------------------------------------------------------

Title::Result Title::update(World& world, const InputState& in) {
    const Edges e = readInput(in);
    if (fadeOn_) {
        if (++fadeT_ >= fadeN_) fadeOn_ = false;
        else fadeAlpha_ = static_cast<int>(255.0f - 255.0f * static_cast<float>(fadeT_) / static_cast<float>(fadeN_));
    }
    Result r = Result::None;
    switch (sub_) {
        case Sub::Startup:
            if (!titleLoaded_ && !loadTitleGraphics(world)) return Result::Failed;
            for (AnmVm& vm : vms_) {
                vm.pendingInterrupt = 1;
                vm.colorAdd = true;
                vm.color = 0xff000000u;
                vm.posOffset = {};
            }
            sub_ = Sub::PreInput;
            [[fallthrough]];
        case Sub::PreInput:
            // "Press a key": Shoot, Bomb, Esc or Q (the arrows do not count). The menu's own
            // pass follows at once.
            if (timer_ < 30 || !e.anyKey) break;
            openMainMenu();
            [[fallthrough]];
        case Sub::MainMenu:
            r = mainMenu(world, e);
            break;
        case Sub::DifficultyLoad:
            if (timer_ == 60) {
                releaseFiles(world, 21, 26, 0, kVmSelect - 1);
                titleLoaded_ = false;
                if (!loadSelectGraphics(world)) return Result::Failed;
                sub_ = Sub::DifficultySelect;
                overlayMin_ = 0;  // fade in from black
                overlayOff_ = overlayOn_;
                overlayOn_ = 0;
                if (session_->difficulty != Extra) {
                    interruptAll(6);
                    selCursor_ = std::clamp<int>(config_->defaultDifficulty, 0, 3);
                } else {
                    interruptAll(18);
                    selCursor_ = 0;
                }
            }
            break;
        case Sub::DifficultySelect:
            difficultySelect(world, e);
            break;
        case Sub::BackToTitle:
            if (timer_ == 36) {
                sub_ = Sub::Startup;
                timer_ = 0;
            }
            break;
        case Sub::CharacterSelect:
            characterSelect(world, e);
            break;
        case Sub::ShotSelect:
            r = shotSelect(world, e);
            break;
        case Sub::PracticeSelect:
            r = practiceSelect(world, e);
            break;
        case Sub::Options:
            options(world, e);
            break;
        case Sub::Quit:
            if (timer_ == 60) return Result::Quit;
            break;
    }
    if (r != Result::None) return r;  // the title is gone
    stepOverlay();
    ++timer_;
    for (AnmVm& vm : vms_) {
        if (world.anm->sprite(vm)) world.anm->executeScript(vm);  // frozen while its file is out
    }
    return Result::None;
}

void Title::openMainMenu() {
    timer_ = 0;
    sub_ = Sub::MainMenu;
    interruptAll(2);
    overlayColor_ = 0x40000000u;  // the background darkens a little
    overlayMin_ = 0;
    overlayN_ = 0;
    overlayOn_ = 60;
}

Title::Result Title::mainMenu(World& world, const Edges& e) {
    int dir = 0;
    if (e.up) {
        --cursor_;
        dir = -1;
        world.playSound(kSoundMove);
    }
    if (e.down) {
        ++cursor_;
        dir = 1;
        world.playSound(kSoundMove);
    }
    cursor_ = wrap(cursor_, 8);
    if (dir != 0 && cursor_ == 1 && !extraUnlocked_) cursor_ = wrap(cursor_ + dir, 8);  // Extra Start is locked
    for (int i = 0; i < 8; ++i) {
        if (i == cursor_) tint(kVmItems + i, 0xffff0000u, -4.0f, -4.0f);
        else tint(kVmItems + i, 0x80300000u, 0.0f, 0.0f);
    }
    if (timer_ >= 20) {
        if (e.confirm) choose(world);
        if (e.q) {  // quits like the Quit item
            interruptAll(4);
            timer_ = 0;
            sub_ = Sub::Quit;
            world.playSound(kSoundBack);
        }
        if (e.cancel) {
            cursor_ = 7;
            world.playSound(kSoundBack);
        }
    }
    return Result::None;
}

void Title::choose(World& world) {
    auto leave = [&] {
        interruptAll(4);
        timer_ = 0;
    };
    auto darken = [&] {  // the background fades to black
        overlayColor_ = 0xff000000u;
        overlayMin_ = 64;
        overlayN_ = 0;
        overlayOn_ = 60;
    };
    auto toSelect = [&] {
        darken();
        leave();
        sub_ = Sub::DifficultyLoad;
        world.playSound(kSoundSelect);
    };
    switch (cursor_) {
        case 0:  // Start
            session_->practice = false;
            if (session_->difficulty == Extra) session_->difficulty = Normal;
            if (config_->defaultDifficulty == Extra) config_->defaultDifficulty = Normal;
            toSelect();
            break;
        case 1:  // Extra Start
            if (!extraUnlocked_) {
                world.playSound(kSoundBack);
                break;
            }
            session_->practice = false;
            session_->difficulty = Extra;
            toSelect();
            break;
        case 2:  // Practice Start
            session_->practice = true;
            if (session_->difficulty == Extra) session_->difficulty = Normal;
            if (config_->defaultDifficulty == Extra) config_->defaultDifficulty = Normal;
            toSelect();
            break;
        case 6:  // Option
            interruptAll(3);
            optCursor_ = 0;
            timer_ = 0;
            sub_ = Sub::Options;
            world.playSound(kSoundSelect);
            break;
        case 7:  // Quit
            leave();
            sub_ = Sub::Quit;
            world.playSound(kSoundBack);
            break;
        default:  // Replay, Result, Music Room: not in the port
            world.playSound(kSoundBack);
            break;
    }
}

void Title::difficultySelect(World& world, const Edges& e) {
    const bool extra = session_->difficulty == Extra;
    if (!extra) {
        if (e.up) --selCursor_, world.playSound(kSoundMove);
        if (e.down) ++selCursor_, world.playSound(kSoundMove);
        selCursor_ = wrap(selCursor_, 4);
        for (int d = 0; d < 4; ++d) {
            if (d == selCursor_) {
                tint(kVmDifficulty + d, 0xff000000u, -6.0f, -6.0f);
            } else {
                tint(kVmDifficulty + d, 0x60000000u, 0.0f, 0.0f);
                vms_[static_cast<std::size_t>(kVmDifficulty + d)].alphaInterpEndTime = 0;
            }
        }
        vms_[kVmExtraCard].visible = false;
    } else {
        for (int d = 0; d < 4; ++d) vms_[static_cast<std::size_t>(kVmDifficulty + d)].visible = false;
        tint(kVmExtraCard, 0xff000000u, -6.0f, -6.0f);
    }
    if (e.cancel) {  // back to the title
        sub_ = Sub::BackToTitle;
        timer_ = 0;
        interruptAll(4);
        world.playSound(kSoundBack);
        if (!extra) {
            config_->defaultDifficulty = static_cast<std::uint8_t>(selCursor_);
            configChanged_ = true;
            cursor_ = session_->practice ? 2 : 0;
        } else {
            cursor_ = 1;
        }
    } else if (e.confirm) {
        sub_ = Sub::CharacterSelect;
        timer_ = 0;
        interruptAll(7);
        world.playSound(kSoundSelect);
        if (!extra) {
            vms_[static_cast<std::size_t>(kVmDifficulty + selCursor_)].pendingInterrupt = 8;
            session_->difficulty = selCursor_;
            selCursor_ = session_->character;
        } else {
            vms_[kVmExtraCard].pendingInterrupt = 8;
            int c = session_->character;
            if (!extraUnlocked(2 * c) && !extraUnlocked(2 * c + 1)) c = 1 - c;
            selCursor_ = c;
        }
        config_->defaultDifficulty = static_cast<std::uint8_t>(session_->difficulty);  // 4 after Extra
        configChanged_ = true;
        const int other = 1 - selCursor_;
        cancel(kVmPairs + 2 * other);
        cancel(kVmPairs + 2 * other + 1);
    }
}

void Title::characterSelect(World& world, const Edges& e) {
    if (timer_ < 30) return;
    const bool extra = session_->difficulty == Extra;
    auto locked = [&](int c) { return extra && !extraUnlocked(2 * c) && !extraUnlocked(2 * c + 1); };
    auto pair = [&](int c, std::int16_t k) {
        vms_[static_cast<std::size_t>(kVmPairs + 2 * c)].pendingInterrupt = k;
        vms_[static_cast<std::size_t>(kVmPairs + 2 * c + 1)].pendingInterrupt = k;
    };
    bool stop = false;
    if (e.left) {
        const int n = wrap(selCursor_ + 1, 2);
        if (locked(n)) {
            stop = true;
        } else {
            selCursor_ = n;
            world.playSound(kSoundMove);
            pair(selCursor_, 9);
            pair(1 - selCursor_, 12);
        }
    }
    if (!stop && e.right) {
        const int n = wrap(selCursor_ - 1, 2);
        if (!locked(n)) {
            selCursor_ = n;
            world.playSound(kSoundMove);
            pair(selCursor_, 10);
            pair(1 - selCursor_, 11);
        }
    }
    if (e.cancel) {  // back to the difficulty select (the character is not kept)
        sub_ = Sub::DifficultySelect;
        timer_ = 0;
        if (!extra) {
            interruptAll(6);
            selCursor_ = std::clamp<int>(config_->defaultDifficulty, 0, 3);
        } else {
            interruptAll(18);
            selCursor_ = 0;
        }
        world.playSound(kSoundBack);
    } else if (e.confirm) {
        sub_ = Sub::ShotSelect;
        timer_ = 0;
        interruptAll(13);
        character_ = selCursor_;
        session_->character = character_;
        dropRiders(character_);
        selCursor_ = session_->shot;
        if (extra && !extraUnlocked(2 * character_ + selCursor_)) selCursor_ = 1 - selCursor_;
        world.playSound(kSoundSelect);
    }
}

Title::Result Title::shotSelect(World& world, const Edges& e) {
    const bool extra = session_->difficulty == Extra;
    if (e.up) --selCursor_, world.playSound(kSoundMove);
    if (e.down) ++selCursor_, world.playSound(kSoundMove);
    selCursor_ = wrap(selCursor_, 2);
    if (extra && !extraUnlocked(2 * character_ + selCursor_)) selCursor_ = 1 - selCursor_;
    vms_[kVmPanels + 1].colorAdd = true;
    vms_[kVmPanels + 3].colorAdd = true;
    for (int s = 0; s < 2; ++s) {
        const int i = kVmPanels + 2 * character_ + s;
        AnmVm& vm = vms_[static_cast<std::size_t>(i)];
        vm.colorAdd = true;
        vm.visible = true;
        if (s == selCursor_) tint(i, 0xff202020u, -6.0f, -6.0f);
        else tint(i, 0xa0000000u, 0.0f, 0.0f);
    }
    if (timer_ < 30) return Result::None;
    if (e.cancel) {  // back to the character select
        sub_ = Sub::CharacterSelect;
        timer_ = 0;
        interruptAll(7);
        dropRiders(character_);
        world.playSound(kSoundBack);
        session_->shot = selCursor_;
        selCursor_ = character_;
    } else if (e.confirm) {
        session_->shot = selCursor_;
        if (!session_->practice) return startGame(world);
        enterPracticeSelect(world);
    }
    return Result::None;
}

void Title::enterPracticeSelect(World& world) {
    (void)world;
    sub_ = Sub::PracticeSelect;
    timer_ = 0;
    interruptAll(19);
    dropRiders(character_);
    // Stage k is offered once stage k + 1 has been reached (or the game cleared) with this
    // shot type and difficulty, continues allowed; stage 1 always. Easy has no stage 6.
    const int t = 2 * character_ + session_->shot;
    const int d = std::clamp(session_->difficulty, 0, 3);
    int n = scores_ ? std::min<int>(6, scores_->clear(t).cleared[static_cast<std::size_t>(d)]) : 1;
    if (d == Easy && n == 6) n = 5;
    practiceStages_ = std::max(n, 1);
    selCursor_ = session_->practiceStage < practiceStages_ ? session_->practiceStage : 0;
}

Title::Result Title::practiceSelect(World& world, const Edges& e) {
    if (e.up) --selCursor_, world.playSound(kSoundMove);
    if (e.down) ++selCursor_, world.playSound(kSoundMove);
    selCursor_ = wrap(selCursor_, practiceStages_);
    if (timer_ < 30) return Result::None;
    if (e.cancel) {  // back to the shot select (with the select sound, as in the original)
        sub_ = Sub::ShotSelect;
        timer_ = 0;
        interruptAll(13);
        dropRiders(character_);
        selCursor_ = session_->shot;
        world.playSound(kSoundSelect);
    } else if (e.confirm) {
        session_->practiceStage = selCursor_;
        return startGame(world);
    }
    return Result::None;
}

Title::Result Title::startGame(World& world) {
    choice_.difficulty = session_->difficulty;
    choice_.shotType = 2 * character_ + session_->shot;
    choice_.practice = session_->practice;
    choice_.stage = choice_.practice ? session_->practiceStage + 1 : (choice_.difficulty == Extra ? 7 : 1);
    choice_.lives = config_->lives;
    choice_.bombs = config_->bombs;
    if (choice_.difficulty == Extra || choice_.practice) {
        choice_.lives = 2;
        choice_.bombs = 3;
    }
    world.playSound(kSoundSelect);
    if (world.music) world.music->stop();
    return Result::StartGame;
}

// ---- the Option menu ----------------------------------------------------------------

void Title::colourOptions() {
    const GameConfig& c = *config_;
    const int current[6] = {c.lives, c.bombs, c.colourMode, c.music, c.sound, c.windowMode};
    auto shade = [&](int i, bool selected, bool rowUnderCursor) {
        AnmVm& vm = vms_[static_cast<std::size_t>(i)];
        if (selected) {
            vm.color = 0x800000ffu;
            vm.posOffset = {-2.0f, -2.0f, 0.0f};
        } else {
            vm.color = 0x80202050u;
            vm.scaleX = vm.scaleY = 1.0f;
            vm.posOffset = {};
        }
        if (rowUnderCursor) {
            vm.color = (vm.color & 0x00ffffffu) | 0xff000000u;
            vm.posOffset.x -= 4.0f;
            vm.posOffset.y -= 4.0f;
        } else {
            vm.color = (vm.color & 0x00ffffffu) | 0x80000000u;
        }
    };
    for (int r = 0; r < kOptionRows; ++r) {
        shade(kOptionLabel[r], r == optCursor_, r == optCursor_);
        if (r >= 6) continue;
        for (int v = 0; v < kOptionValueCount[r]; ++v) shade(kOptionValueFirst[r] + v, v == current[r], r == optCursor_);
    }
}

void Title::optionStep(World& world, int step) {
    GameConfig& c = *config_;
    switch (optCursor_) {
        case RowLives:
            c.lives = static_cast<std::uint8_t>(wrap(c.lives + step, 5));
            break;
        case RowBombs:
            c.bombs = static_cast<std::uint8_t>(wrap(c.bombs + step, 4));
            break;
        case RowMusic:
            if (world.music) world.music->stop();
            c.music = static_cast<std::uint8_t>(wrap(c.music + step, 3));
            playTitleTheme(world);
            break;
        case RowSound:
            c.sound = c.sound ? 0 : 1;
            break;
        default:  // colour and screen mode mean nothing on a phone; the actions take Confirm
            return;
    }
    world.playSound(kSoundMove);
}

void Title::options(World& world, const Edges& e) {
    if (e.up) --optCursor_, world.playSound(kSoundMove);
    if (e.down) ++optCursor_, world.playSound(kSoundMove);
    optCursor_ = wrap(optCursor_, kOptionRows);
    colourOptions();
    if (timer_ < 32) return;
    if (e.left) optionStep(world, -1);
    if (e.cancel) {
        optCursor_ = RowQuit;
        world.playSound(kSoundBack);
    }
    if (e.right) optionStep(world, 1);
    if (e.confirm) {
        GameConfig& c = *config_;
        switch (optCursor_) {
            case RowDefault:  // (the original resets these and plays no sound)
                if (world.music) world.music->stop();
                c.lives = 2;
                c.bombs = 3;
                c.music = GameConfig::MusicWav;
                c.sound = 1;
                c.defaultDifficulty = Normal;
                c.windowMode = 0;
                c.frameSkip = 0;
                playTitleTheme(world);
                break;
            case RowKeys:  // Key Config: Android maps controllers itself
                world.playSound(kSoundBack);
                break;
            case RowQuit:
                sub_ = Sub::MainMenu;
                timer_ = 0;
                interruptAll(2);
                cursor_ = 6;
                world.playSound(kSoundBack);
                configChanged_ = true;  // the port saves the settings here
                break;
            default:
                break;
        }
    }
}

// ---- drawing ------------------------------------------------------------------------

void Title::draw(World& world, Gui& gui, SpriteSink& sink, const SpriteRegion& white) {
    if (haveBackground_) {
        SpriteInstance s;
        s.x = bgWidth_ * 0.5f;
        s.y = bgHeight_ * 0.5f;
        s.w = bgWidth_;
        s.h = bgHeight_;
        sink.draw(background_, Blend::Alpha, s);
    }
    if (const int a = overlayAlpha(); a > 0) {
        const std::uint32_t c = overlayColor_;
        sink.fillRect(white, 0.0f, 0.0f, 640.0f, 480.0f,
                      rgba(static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c),
                           static_cast<std::uint8_t>(std::min(a, 255))));
    }
    for (int i = 0; i < kVmDrawn; ++i) {
        AnmVm vm = vms_[static_cast<std::size_t>(i)];
        vm.pos = vm.pos + vm.posOffset;
        world.anm->drawScreen(sink, vm);
    }
    if (sub_ == Sub::PracticeSelect && scores_) {
        const int a = timer_ < 30 ? 255 * timer_ / 30 : 255;
        const int t = 2 * character_ + session_->shot;
        const int d = std::clamp(session_->difficulty, 0, 3);
        for (int i = 0; i < practiceStages_; ++i) {
            const std::uint32_t color = i == selCursor_ ? (static_cast<std::uint32_t>(a) << 24) | 0xc0f0f0u
                                                        : (static_cast<std::uint32_t>(a / 2) << 24) | 0x80c0c0u;
            gui.queueString(320.0f, 200.0f + 24.0f * static_cast<float>(i),
                            std::format("STAGE {}  {:09d}", i + 1, scores_->practice(t, i, d)), color);
        }
    }
    gui.drawStrings(world, sink);
    if (fadeOn_ && fadeAlpha_ > 0) {
        sink.fillRect(white, 0.0f, 0.0f, 640.0f, 480.0f, rgba(255, 255, 255, static_cast<std::uint8_t>(std::min(fadeAlpha_, 255))));
    }
}

}  // namespace th06
