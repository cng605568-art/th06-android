#include "game/ResultScreen.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <format>

#include "core/Log.hpp"
#include "game/Gui.hpp"
#include "game/World.hpp"
#include "gfx/Image.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"

namespace th06 {
namespace {

// The enemy slots, free once the game is over (result00..03 load at the enemy offset).
constexpr int kFileSlots[4] = {8, 9, 11, 12};
constexpr const char* kFiles[4] = {"data/result/result00.anm", "data/result/result01.anm", "data/result/result02.anm",
                                   "data/result/result03.anm"};
constexpr int kOffsets[4] = {0x100, 0x108, 0x10F, 0x125};
constexpr int kScriptFirst = 0x100;
constexpr int kTextSpriteFirst = 0x708;  // text.anm sprites 8..23

constexpr int kVmTableAnchor = 14, kVmStatistics = 37;
// The save-replay prompt: V15..V20 its pictures (V16 Yes, V17 No), V21 the slot list's
// header, V22..V36 its rows.
constexpr int kVmReplayFirst = 15, kVmReplayLast = 20, kVmYes = 16, kVmNo = 17, kVmSlotHeader = 21, kVmSlotRows = 22;
constexpr int kSoundOk = 10, kSoundCancel = 11, kSoundSelect = 12;

constexpr std::uint32_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kShoot = 16, kBomb = 32, kEsc = 64, kFocus = 128,
                        kSkip = 256;

// Key grid: 6 rows of 16; 0 = blank. Cell 94 is SP, 95 is END (drawn with the font's
// special glyphs 0x80 and 0x81).
constexpr const char* kKeys[6] = {
    "ABCDEFGHIJKLMNOP", "QRSTUVWXYZ.,:;\xa5@", "abcdefghijklmnop",
    "qrstuvwxyz+-/*=%", "0123456789(){}[]", "<>#!?'\"$",
};
constexpr int kKeySpace = 94, kKeyEnd = 95;

bool blankKey(int k) { return k >= 88 && k < kKeySpace; }
char keyChar(int k) {
    if (k == kKeySpace) return ' ';
    if (k == kKeyEnd) return '\0';
    return kKeys[k / 16][k % 16];
}

// Shot type names (Shift-JIS), rendered into text.anm for the score table.
constexpr const char* kShotNames[4] = {
    "\x94\x8e\x97\xed\x20\x97\xec\x96\xb2\x20\x28\x97\xec\x29",          // Hakurei Reimu (spirit)
    "\x94\x8e\x97\xed\x20\x97\xec\x96\xb2\x20\x28\x96\xb2\x29",          // Hakurei Reimu (dream)
    "\x96\xb6\x89\x4a\x20\x96\x82\x97\x9d\x8d\xb9\x20\x28\x96\x82\x29",  // Kirisame Marisa (devil)
    "\x96\xb6\x89\x4a\x20\x96\x82\x97\x9d\x8d\xb9\x20\x28\x97\xf6\x29",  // Kirisame Marisa (love)
};
constexpr const char* kDifficultyText[5] = {"     Easy", "   Normal", "     Hard", "  Lunatic", "    Extra"};

std::string entryText(const ScoreFile::Entry& e) {
    std::string stage;
    if (e.stage == ScoreFile::kStageCleared) stage = "C";
    else if (e.stage >= 1 && e.stage <= 6) stage = std::to_string(e.stage);
    else stage = "1";  // the Extra stage shows as 1
    return std::string(e.name.begin(), e.name.end()) + std::format(" {:9d}({})", e.score, stage);
}

}  // namespace

bool ResultScreen::loadFiles(World& world, const Vfs& vfs, TextureLibrary& textures) {
    AnmManager& anm = *world.anm;
    for (int i = 0; i < 4; ++i) {
        std::string err;
        anm.releaseAnm(kFileSlots[i]);
        if (!anm.loadAnm(vfs, kFileSlots[i], kFiles[i], kOffsets[i], &err)) {
            log::warn("Result screen: {} not loaded ({})", kFiles[i], err);
            return false;
        }
    }
    haveBackground_ = false;
    if (auto jpg = vfs.read("data/result/result.jpg")) {
        std::string err;
        if (auto img = decodeImage(*jpg, &err)) {
            bgWidth_ = static_cast<float>(img->width);
            bgHeight_ = static_cast<float>(img->height);
            auto shared = std::make_shared<Image>(std::move(*img));
            background_ = textures.add("data/result/result.jpg", shared->width, shared->height,
                                       [shared] { return std::optional<Image>(*shared); });
            haveBackground_ = true;
        } else {
            log::warn("Result screen: result.jpg: {}", err);
        }
    }
    for (int i = 0; i < kVms; ++i) {
        AnmVm& vm = vms_[static_cast<std::size_t>(i)];
        vm = AnmVm{};  // position and offset (0, 0) first: what frame 0 sets is kept
        anm.setAndExecuteScript(vm, kScriptFirst + i);
    }
    for (int i = 0; i < kTexts; ++i) {
        AnmVm& vm = texts_[static_cast<std::size_t>(i)];
        vm = AnmVm{};
        anm.initializeAndSetSprite(vm, kTextSpriteFirst + i);
        vm.anchor = AnmAnchor::TopLeft;
        vm.visible = false;
    }
    t_ = 0;
    prevButtons_ = 0;
    repeatCounter_ = 0;
    return true;
}

bool ResultScreen::start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui, ScoreFile& scores,
                         const Run& run) {
    if (!loadFiles(world, vfs, textures)) return false;
    scores_ = &scores;
    run_ = run;
    titleMode_ = false;
    (void)gui;
    state_ = State::NameEntry;
    t_ = 0;
    rank_ = 10;
    key_ = cursor_ = 0;
    choice_ = slot_ = 0;
    replayName_ = std::string(8, ' ');
    savedPath_.clear();
    for (auto& s : slots_) s.reset();
    prevButtons_ = 0;
    repeatCounter_ = 0;
    return true;
}

ResultScreen::Edges ResultScreen::readInput(const InputState& in) {
    const std::uint32_t bits = (in.up ? kUp : 0) | (in.down ? kDown : 0) | (in.left ? kLeft : 0) | (in.right ? kRight : 0) |
                               (in.shoot ? kShoot : 0) | (in.bomb ? kBomb : 0) | (in.pause ? kEsc : 0) |
                               (in.focus ? kFocus : 0) | (in.skip ? kSkip : 0);
    // One repeat clock for everything: 33 frames after the last change, then every 8.
    bool tick = false;
    if (bits != prevButtons_) {
        repeatCounter_ = 0;
    } else {
        tick = repeatCounter_ >= 30 && repeatCounter_ % 8 == 0;
        if (repeatCounter_ >= 38) repeatCounter_ = 30;
        ++repeatCounter_;
    }
    auto pressed = [&](std::uint32_t m) { return (bits & m) != 0 && (bits & m) != (prevButtons_ & m); };
    auto repeating = [&](std::uint32_t m) { return pressed(m) || ((bits & m) != 0 && tick); };
    Edges e;
    e.up = repeating(kUp);
    e.down = repeating(kDown);
    e.left = repeating(kLeft);
    e.right = repeating(kRight);
    e.confirm = pressed(kShoot);
    e.confirmRepeat = repeating(kShoot);
    e.cancelRepeat = repeating(kBomb | kEsc);
    e.esc = pressed(kEsc);
    e.cancel = pressed(kBomb | kEsc);
    prevButtons_ = bits;
    return e;
}

// ---- the score viewer (from the title) ---------------------------------------------

bool ResultScreen::startTitleMode(World& world, const Vfs& vfs, TextureLibrary& textures, ScoreFile& scores) {
    if (!loadFiles(world, vfs, textures)) return false;
    scores_ = &scores;
    run_ = Run{};
    titleMode_ = true;
    state_ = State::MenuIntro;
    menuCursor_ = 1;  // Normal
    pageItem_ = 0;
    rememberedCharacter_ = rememberedPage_ = 0;
    renderedCharacter_ = renderedPage_ = 0;
    spellsShown_ = false;
    rank_ = 10;
    return true;
}

void ResultScreen::renderShotNames(World& world, Gui& gui, int character) {
    for (int s = 0; s < 2; ++s) {
        AnmVm& vm = texts_[static_cast<std::size_t>(s)];
        const char* text = kShotNames[character * 2 + s];
        gui.renderText(world, vm, std::span(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)), 15, 0xffffffu,
                       Gui::Align::Centre);
        vm.color = 0xffffffffu;  // neither name is dimmed here
    }
}

void ResultScreen::menuPass(World& world, const Edges& e) {
    if (e.up) menuCursor_ = (menuCursor_ + 6) % 7, world.playSound(kSoundSelect);
    if (e.down) menuCursor_ = (menuCursor_ + 1) % 7, world.playSound(kSoundSelect);
    for (int k = 0; k < 7; ++k) {
        AnmVm& vm = vms_[static_cast<std::size_t>(1 + k)];
        if (k == menuCursor_) {
            vm.color = 0xff505050u;
            vm.posOffset = {-4.0f, -4.0f, 0.0f};
        } else {
            vm.color = 0xb0000000u;
            vm.posOffset = {};
        }
    }
    bool opened = false;
    if (e.confirm) {
        if (menuCursor_ <= 4) {  // a difficulty's tables (no sound)
            interruptAll(static_cast<std::int16_t>(3 + menuCursor_));
            pageItem_ = menuCursor_;
            state_ = State::ScorePage;
            spellsShown_ = false;
            t_ = 0;
            menuCursor_ = rememberedCharacter_;
            renderedCharacter_ = renderedPage_ = -1;
            opened = true;
        } else if (menuCursor_ == 5) {  // the spell cards
            interruptAll(8);
            pageItem_ = 5;
            state_ = State::SpellPage;
            spellsShown_ = true;
            t_ = 0;
            menuCursor_ = rememberedPage_;
            renderedCharacter_ = renderedPage_ = -1;
            opened = true;
        } else {  // Exit (the 60-pass wait counts from the menu's start)
            interruptAll(2);
            state_ = State::Exiting;
            world.playSound(kSoundCancel);
        }
    }
    if (e.cancel && !opened) {  // (the original also took it after opening a page)
        menuCursor_ = 6;
        world.playSound(kSoundCancel);
    }
}

bool ResultScreen::titleUpdate(World& world, Gui& gui, const Edges& e) {
    auto backToMenu = [&](int item) {
        world.playSound(kSoundCancel);
        state_ = State::MenuIntro;
        t_ = 1;  // the first pass's colour reset is skipped
        interruptAll(1);
        menuCursor_ = item;
    };
    switch (state_) {
        case State::MenuIntro:
            if (t_ == 0) {
                for (AnmVm& vm : vms_) {
                    vm.pendingInterrupt = 1;
                    vm.colorAdd = true;
                    vm.color &= 0xff000000u;
                }
                for (int k = 0; k < 7; ++k) {
                    AnmVm& vm = vms_[static_cast<std::size_t>(1 + k)];
                    vm.color = k == menuCursor_ ? 0xff505050u : 0xb0000000u;
                    vm.posOffset = k == menuCursor_ ? Vec3f{-4.0f, -4.0f, 0.0f} : Vec3f{};
                }
            }
            if (t_ != 20) break;
            state_ = State::Menu;
            t_ = 0;
            [[fallthrough]];
        case State::Menu:
            menuPass(world, e);
            break;
        case State::Exiting:
            if (t_ >= 60) {
                state_ = State::Done;
                return true;
            }
            break;
        case State::ScorePage:
            if (t_ == 20 && renderedCharacter_ != menuCursor_) {
                renderedCharacter_ = menuCursor_;
                renderShotNames(world, gui, renderedCharacter_);
            }
            if (t_ < 30) break;
            if (e.left || e.right) {  // Reimu <-> Marisa, and the page plays again
                menuCursor_ = 1 - menuCursor_;
                world.playSound(kSoundSelect);
                t_ = 0;
                interruptAll(static_cast<std::int16_t>(3 + pageItem_));
            }
            if (e.cancel) {
                rememberedCharacter_ = menuCursor_;
                backToMenu(pageItem_);
            }
            break;
        case State::SpellPage:
            if (t_ == 20 && renderedPage_ != menuCursor_) {
                renderedPage_ = menuCursor_;
                static constexpr char kUnknown[] = "\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48";  // five full-width '?'
                for (int n = 10 * renderedPage_; n < std::min(10 * renderedPage_ + 10, ScoreFile::kSpells); ++n) {
                    const ScoreFile::Spell& sp = scores_->spell(n);
                    std::span<const std::uint8_t> name;
                    if (sp.attempts == 0) {
                        name = std::span(reinterpret_cast<const std::uint8_t*>(kUnknown), sizeof kUnknown - 1);
                    } else {
                        const auto end = std::find(sp.name.begin(), sp.name.end(), std::uint8_t{0});
                        name = std::span(sp.name.data(), static_cast<std::size_t>(end - sp.name.begin()));
                    }
                    gui.renderText(world, texts_[static_cast<std::size_t>(n % 10)], name, 15, 0xffffffu, Gui::Align::Left);
                }
            }
            if (t_ < 30) break;
            if (e.left || e.right) {
                menuCursor_ = (menuCursor_ + (e.left ? 6 : 1)) % 7;
                world.playSound(kSoundSelect);
                t_ = 0;
                interruptAll(8);
            }
            if (e.cancel) {
                rememberedPage_ = menuCursor_;
                backToMenu(5);
            }
            break;
        default:
            break;
    }
    for (AnmVm& vm : vms_) world.anm->executeScript(vm);
    ++t_;
    return false;
}

void ResultScreen::drawSpellList(Gui& gui, SpriteSink& sink, World& world) {
    const Vec3f p = vms_[kVmTableAnchor].pos;
    for (int i = 0; i < 10; ++i) {
        const int n = 10 * renderedPage_ + i;
        if (n >= ScoreFile::kSpells) break;
        const ScoreFile::Spell& sp = scores_->spell(n);
        const float y = p.y + 16.0f + 30.0f * static_cast<float>(i);
        const std::uint32_t color = sp.attempts == 0   ? 0x80c0c0ffu
                                    : sp.captures == 0 ? 0xffc0a0a0u
                                                       : 0xfff0f0ffu - static_cast<std::uint32_t>(i) * 0x00080800u;
        gui.queueString(p.x, y, std::format("No.{:02d}", n + 1), color);
        AnmVm vm = texts_[static_cast<std::size_t>(i)];
        vm.pos = {p.x + 96.0f, y, 0.0f};
        world.anm->drawScreen(sink, vm);
        gui.queueString(p.x + 368.0f, y, std::format("{:3d}/{:3d}", sp.captures, sp.attempts), color);
    }
}

void ResultScreen::interruptAll(std::int16_t k) {
    for (AnmVm& vm : vms_) vm.pendingInterrupt = k;
}

std::string ResultScreen::name() const {
    if (!scores_ || rank_ >= 10) return std::string(8, ' ');
    const auto& e = scores_->table(shownDifficulty_, run_.shotType)[static_cast<std::size_t>(rank_)];
    return std::string(e.name.begin(), e.name.end());
}

void ResultScreen::moveKey(int step, bool vertical) {
    do {
        if (vertical) {
            key_ += step;
            if (key_ < 0) key_ += 96;
            if (key_ >= 96) key_ -= 96;
        } else {
            const int row = key_ / 16, col = (key_ % 16 + step + 16) % 16;
            key_ = row * 16 + col;
        }
    } while (blankKey(key_));
}

void ResultScreen::finishNameEntry(World& world) {
    (void)world;
    replayName_ = name();  // the replay's name starts as the high-score name (or blank)
    state_ = State::Statistics;
    t_ = 0;
    interruptAll(2);
    if (scores_ && rank_ < 10) scores_->table(shownDifficulty_, run_.shotType)[static_cast<std::size_t>(rank_)].naming = false;
}

void ResultScreen::nameEntry(World& world, Gui& gui, const Edges& e) {
    if (t_ == 0) {
        shownDifficulty_ = run_.difficulty;
        shownCharacter_ = run_.shotType / 2;
        interruptAll(static_cast<std::int16_t>(3 + shownDifficulty_));
        for (int s = 0; s < 2; ++s) {
            AnmVm& vm = texts_[static_cast<std::size_t>(s)];
            const char* text = kShotNames[shownCharacter_ * 2 + s];
            gui.renderText(world, vm, std::span(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)), 15, 0xffffffu,
                           Gui::Align::Centre);
            vm.color = shownCharacter_ * 2 + s == run_.shotType ? 0xffffffffu : 0x80ffffffu;
        }
        ScoreFile::Entry entry;
        entry.score = run_.score;
        entry.shotType = static_cast<std::uint8_t>(run_.shotType);
        entry.difficulty = static_cast<std::uint8_t>(run_.difficulty);
        entry.stage = run_.cleared ? ScoreFile::kStageCleared : static_cast<std::uint8_t>(run_.stage);
        entry.naming = true;
        if (!run_.recordable) {  // mischief: the score does not go into the table
            rank_ = 10;
            finishNameEntry(world);
            return;
        }
        ScoreFile::Table& table = scores_->table(run_.difficulty, run_.shotType);
        rank_ = ScoreFile::insert(table, entry);
        if (rank_ >= 10) {  // not in the top 10: straight on, silently, and never saved
            finishNameEntry(world);
            return;
        }
        cursor_ = 0;
        key_ = 0;
        return;
    }
    if (t_ < 30) return;
    auto& entry = scores_->table(shownDifficulty_, run_.shotType)[static_cast<std::size_t>(rank_)];
    if (e.up) moveKey(-16, true), world.playSound(kSoundSelect);
    if (e.down) moveKey(16, true), world.playSound(kSoundSelect);
    if (e.left) moveKey(-1, false), world.playSound(kSoundSelect);
    if (e.right) moveKey(1, false), world.playSound(kSoundSelect);
    if (e.confirmRepeat) {
        const int p = std::min(cursor_, 7);
        if (key_ == kKeyEnd) {
            world.playSound(kSoundCancel);
            finishNameEntry(world);
            return;
        }
        entry.name[static_cast<std::size_t>(p)] = keyChar(key_);
        if (cursor_ < 8 && ++cursor_ == 8) key_ = kKeyEnd;
        world.playSound(kSoundOk);
    }
    if (e.cancelRepeat) {
        // The original clears the cell the cursor leaves (blank unless the name is full).
        const int p = std::min(cursor_, 7);
        if (cursor_ > 0) {
            --cursor_;
            entry.name[static_cast<std::size_t>(p)] = ' ';
        }
        world.playSound(kSoundCancel);
    }
    if (e.esc) {
        world.playSound(kSoundCancel);
        finishNameEntry(world);
    }
}

bool ResultScreen::update(World& world, Gui& gui, const InputState& in) {
    if (state_ == State::Done) return true;
    const Edges e = readInput(in);
    if (titleMode_) return titleUpdate(world, gui, e);
    switch (state_) {
        case State::NameEntry:
            nameEntry(world, gui, e);
            break;
        case State::Statistics:
            if (t_ <= 30) vms_[kVmStatistics].pendingInterrupt = 16;  // holds the page's opening
            if (t_ >= 90 && e.confirm) {
                vms_[kVmStatistics].pendingInterrupt = 2;
                state_ = State::StatisticsFade;
                t_ = 0;
            }
            break;
        case State::StatisticsFade:
            if (t_ == 30) {
                if (replay_ && run_.recordable) {  // the save-replay prompt (its first pass at t = 60)
                    state_ = State::ReplayQuestion;
                    t_ = 59;
                } else {
                    state_ = State::Exiting;
                    interruptAll(2);
                    t_ = 0;
                }
            }
            break;
        case State::ReplayQuestion:
        case State::ReplayCannot:
        case State::ReplaySlots:
        case State::ReplayOverwrite:
            replayPrompt(world, e);
            break;
        case State::ReplayName:
            replayNameEntry(world, e);
            break;
        case State::Exiting:
            if (t_ >= 60) {
                state_ = State::Done;
                return true;
            }
            break;
        case State::Done:
            return true;
        default:
            break;
    }
    for (AnmVm& vm : vms_) world.anm->executeScript(vm);
    ++t_;
    return false;
}

// ---- the save-replay prompt ------------------------------------------------------------

void ResultScreen::colourChoices() {
    auto set = [this](int vm, bool chosen) {
        std::uint32_t& c = vms_[static_cast<std::size_t>(vm)].color;
        c = (c & 0xff000000u) | (chosen ? 0xff6060u : 0x606060u);
    };
    set(kVmYes, choice_ == 0);
    set(kVmNo, choice_ == 1);
}

void ResultScreen::enterSlots(World& world) {
    (void)world;
    interruptAll(10);
    t_ = 0;
    state_ = State::ReplaySlots;
    for (auto& s : slots_) s.reset();
    if (replayDir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(replayDir_, ec);
    for (int n = 0; n < kReplaySlots; ++n) {
        std::vector<std::uint8_t> bytes;
        if (std::FILE* f = std::fopen((replayDir_ + std::format("th6_{:02d}.rpy", n + 1)).c_str(), "rb")) {
            std::uint8_t buf[8192];
            for (std::size_t k; (k = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + k);
            std::fclose(f);
        }
        if (bytes.empty()) continue;
        if (auto r = Replay::parse(bytes)) slots_[static_cast<std::size_t>(n)] = SlotInfo{r->name, r->date, r->shotType, r->score};
    }
}

void ResultScreen::replayPrompt(World& world, const Edges& e) {
    auto toV1to37 = [this](std::int16_t k) {
        for (int i = 1; i < kVms; ++i) vms_[static_cast<std::size_t>(i)].pendingInterrupt = k;
    };
    auto decline = [&] {
        t_ = 0;
        world.playSound(kSoundCancel);
        interruptAll(2);
        state_ = State::Exiting;
    };
    auto toName = [&] {
        interruptAll(15);
        vms_[static_cast<std::size_t>(kVmSlotRows + slot_)].pendingInterrupt = 14;
        cursor_ = 0;
        key_ = 0;
        state_ = State::ReplayName;
    };
    switch (state_) {
        case State::ReplayQuestion:
            if (t_ == 60) {
                choice_ = 0;
                if (run_.continues != 0) {  // "cannot save: a continue was used"
                    toV1to37(12);
                    state_ = State::ReplayCannot;
                    break;
                }
                toV1to37(9);
            }
            colourChoices();
            if (t_ < 80) break;
            if (e.left || e.right) {
                choice_ ^= 1;  // (two choices: Left and Right both toggle)
                world.playSound(kSoundSelect);
            }
            if (e.cancel) {
                decline();
            } else if (e.confirm) {
                if (choice_ == 1) {
                    decline();
                } else {
                    world.playSound(kSoundOk);
                    enterSlots(world);
                }
            }
            break;
        case State::ReplayCannot:
            if (t_ >= 20 && (e.confirm || e.cancel)) decline();
            break;
        case State::ReplaySlots:
            if (t_ < 20) break;
            if (e.up) {
                slot_ = (slot_ + kReplaySlots - 1) % kReplaySlots;
                world.playSound(kSoundSelect);
            }
            if (e.down) {
                slot_ = (slot_ + 1) % kReplaySlots;
                world.playSound(kSoundSelect);
            }
            if (e.confirm) {
                world.playSound(kSoundOk);
                t_ = 0;
                if (slots_[static_cast<std::size_t>(slot_)]) {  // used: overwrite?
                    interruptAll(11);
                    vms_[static_cast<std::size_t>(kVmSlotRows + slot_)].pendingInterrupt = 14;
                    choice_ = 0;
                    cursor_ = 0;
                    key_ = 0;
                    state_ = State::ReplayOverwrite;
                } else {
                    toName();
                }
            } else if (e.cancel) {  // back to the question (asked again at t = 60)
                world.playSound(kSoundCancel);
                interruptAll(2);
                t_ = 0;
                state_ = State::ReplayQuestion;
            }
            break;
        case State::ReplayOverwrite:
            colourChoices();
            if (t_ < 20) break;
            if (e.left || e.right) {
                choice_ ^= 1;
                world.playSound(kSoundSelect);
            }
            if (e.cancel) {
                world.playSound(kSoundOk);
                enterSlots(world);
            } else if (e.confirm) {
                t_ = 0;
                if (choice_ == 0) {
                    toName();  // (no sound)
                } else {
                    world.playSound(kSoundOk);
                    enterSlots(world);
                }
            }
            break;
        default:
            break;
    }
}

void ResultScreen::replayNameEntry(World& world, const Edges& e) {
    if (t_ < 30) return;
    if (e.up) moveKey(-16, true), world.playSound(kSoundSelect);
    if (e.down) moveKey(16, true), world.playSound(kSoundSelect);
    if (e.left) moveKey(-1, false), world.playSound(kSoundSelect);
    if (e.right) moveKey(1, false), world.playSound(kSoundSelect);
    if (e.confirmRepeat) {
        const int p = std::min(cursor_, 7);
        const bool end = key_ == kKeyEnd;
        if (end) {
            saveReplay();
            t_ = 0;
            interruptAll(2);
            state_ = State::Exiting;
        } else {
            replayName_[static_cast<std::size_t>(p)] = keyChar(key_);
        }
        if (cursor_ < 8 && ++cursor_ == 8) key_ = kKeyEnd;
        world.playSound(kSoundOk);
        if (end) return;
    }
    if (e.cancelRepeat) {
        const int p = std::min(cursor_, 7);  // (the original blanks the cell at the cursor)
        if (cursor_ > 0) {
            replayName_[static_cast<std::size_t>(p)] = ' ';
            --cursor_;
        }
        world.playSound(kSoundCancel);
    }
    if (e.esc) {
        world.playSound(kSoundOk);
        enterSlots(world);
    }
}

std::string ResultScreen::today() const {
    if (!date_.empty()) return date_;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    return std::format("{:02d}/{:02d}/{:02d}", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
}

bool ResultScreen::saveReplay() {
    if (!replay_ || replayDir_.empty()) return false;
    Replay r = *replay_;
    r.name = replayName_;
    r.date = today();
    r.score = run_.score;
    r.slowdown = 0.0f;  // the port runs at a steady 60
    if (const int last = r.lastStage(); last >= 0) r.stages[static_cast<std::size_t>(last)].endScore = static_cast<std::int32_t>(run_.score);
    ZunRng local(0);
    const std::vector<std::uint8_t> bytes = r.serialize(rng_ ? *rng_ : local);
    std::error_code ec;
    std::filesystem::create_directories(replayDir_, ec);
    const std::string path = replayDir_ + std::format("th6_{:02d}.rpy", slot_ + 1);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        log::warn("Could not write {}", tmp);
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !ok) {
        log::warn("Could not write {}", tmp);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        log::warn("Could not save {}: {}", path, ec.message());
        return false;
    }
    savedPath_ = path;
    log::info("Replay saved to {}", path);
    return true;
}

void ResultScreen::drawReplaySlots(Gui& gui, SpriteSink& sink, World& world) {
    for (int i = kVmReplayFirst; i <= kVmReplayLast; ++i) {  // drawn again on top, without their offset
        AnmVm vm = vms_[static_cast<std::size_t>(i)];
        vm.rotation.z = 0.0f;
        world.anm->drawScreen(sink, vm);
    }
    const Vec3f h = vms_[kVmSlotHeader].pos;
    gui.queueString(h.x, h.y, "No.   Name     Date     Player Score", 0xffffffffu);
    const bool naming = state_ == State::ReplayName;
    std::string underscore = "      " + std::string(8, ' ');
    underscore[6 + static_cast<std::size_t>(std::min(cursor_, 7))] = '_';
    for (int n = 0; n < kReplaySlots; ++n) {
        const Vec3f p = vms_[static_cast<std::size_t>(kVmSlotRows + n)].pos;
        const std::uint32_t color = n == slot_ ? 0xffff8080u : 0xff808080u;
        std::string text;
        if (naming) {
            text = std::format("No.{:02d} {} {} {} {:9d}", n + 1, replayName_, today(), Replay::shotName(run_.shotType), run_.score);
        } else if (const auto& s = slots_[static_cast<std::size_t>(n)]) {
            text = std::format("No.{:02d} {:8.8} {:8.8} {} {:9d}", n + 1, s->name, s->date, Replay::shotName(s->shotType), s->score);
        } else {
            text = std::format("No.{:02d} -------- --/--/-- -------         0", n + 1);
        }
        gui.queueString(p.x, p.y, text, color);
        if (naming) gui.queueString(p.x, p.y, underscore, 0xfff0f0ffu);
    }
    if (naming) drawKeys(gui);
}

std::string ResultScreen::completionText(const Run& run) {
    if (run.cleared && run.difficulty != Easy) return "      100%";
    const double denom = run.difficulty == Extra ? 39600.0 : 89500.0;
    return std::format("    {:.2f}%", static_cast<double>(run.completion) / denom * 100.0);
}

void ResultScreen::drawTable(Gui& gui, SpriteSink& sink, World& world) {
    const AnmVm& anchor = vms_[kVmTableAnchor];
    if (!(anchor.pos.x < 640.0f)) return;
    const Vec3f p = anchor.pos;
    for (int s = 0; s < 2; ++s) {
        AnmVm vm = texts_[static_cast<std::size_t>(s)];
        vm.pos = p + Vec3f{320.0f * static_cast<float>(s), 0.0f, 0.0f};
        world.anm->drawScreen(sink, vm);
    }
    const bool naming = state_ == State::NameEntry && rank_ < 10;
    const bool playingA = run_.shotType % 2 == 0;
    const std::string underscore = [&] {
        std::string u(8, ' ');
        u[static_cast<std::size_t>(std::min(cursor_, 7))] = '_';
        return u;
    }();
    const ScoreFile::Table& ta = scores_->table(shownDifficulty_, shownCharacter_ * 2);
    const ScoreFile::Table& tb = scores_->table(shownDifficulty_, shownCharacter_ * 2 + 1);
    for (int i = 0; i < 10; ++i) {
        const float y = p.y + 36.0f + 18.0f * static_cast<float>(i);
        const bool namedRow = naming && i == rank_;
        std::uint32_t ca = 0xffffc0c0u, cb = 0xffc0c0ffu;
        if (naming) {
            if (playingA) {
                ca = namedRow ? 0xfff0f0ffu : 0x80ffffc0u;
                cb = 0x80c0c0ffu;
            } else {
                ca = 0x80ffc0c0u;
                cb = namedRow ? 0xfffff0f0u : 0xc0c0c0ffu;
            }
        }
        if (namedRow && playingA) gui.queueString(p.x, y, "   " + underscore, ca);
        gui.queueString(p.x, y, std::format("{:2d}", i + 1), ca);
        if (static_cast<std::size_t>(i) < ta.size()) gui.queueString(p.x + 36.0f, y, entryText(ta[static_cast<std::size_t>(i)]), ca);
        if (namedRow && !playingA) gui.queueString(p.x + 336.0f, y, underscore, cb);
        if (static_cast<std::size_t>(i) < tb.size()) gui.queueString(p.x + 336.0f, y, entryText(tb[static_cast<std::size_t>(i)]), cb);
    }
}

void ResultScreen::drawKeys(Gui& gui) {
    const int tm = t_ % 32;
    const float s = t_ % 64 < 32 ? 1.2f + 0.8f * static_cast<float>(tm) / 32.0f : 2.0f - 0.8f * static_cast<float>(tm) / 32.0f;
    for (int k = 0; k < 96; ++k) {
        if (blankKey(k)) continue;
        const char glyph = k == kKeySpace ? '\x80' : k == kKeyEnd ? '\x81' : keyChar(k);
        float x = 160.0f + 20.0f * static_cast<float>(k % 16), y = 356.0f + 18.0f * static_cast<float>(k / 16);
        if (k == key_) {
            x -= (s - 1.0f) * 8.0f;
            y -= (s - 1.0f) * 8.0f;
            gui.queueString(x, y, std::string(1, glyph), 0xffffffc0u, s);
        } else {
            gui.queueString(x, y, std::string(1, glyph), 0x60c0c0c0u);
        }
    }
}

void ResultScreen::drawStatistics(Gui& gui) {
    const AnmVm& page = vms_[kVmStatistics];
    const Vec3f v = page.pos;
    const std::uint32_t c = page.color;
    const std::string lines[8] = {
        std::format("{:9d}", run_.score),
        kDifficultyText[std::clamp(run_.difficulty, 0, 4)],
        completionText(run_),
        std::format("{:9d}", run_.continues),
        std::format("{:9d}", run_.deaths),
        std::format("{:9d}", run_.bombs),
        std::format("{:9d}", run_.spellsCaptured),
        "    0.00%",  // slowdown: the port always runs at full speed
    };
    for (int j = 0; j < 8; ++j) gui.queueString(v.x + 224.0f, v.y + 32.0f + 22.0f * static_cast<float>(j), lines[j], c);
}

void ResultScreen::draw(World& world, Gui& gui, SpriteSink& sink) {
    if (haveBackground_) {
        SpriteInstance s;
        s.x = bgWidth_ * 0.5f;
        s.y = bgHeight_ * 0.5f;
        s.w = bgWidth_;
        s.h = bgHeight_;
        sink.draw(background_, Blend::Alpha, s);
    }
    for (const AnmVm& v : vms_) {
        AnmVm vm = v;
        vm.pos = v.pos + v.posOffset;
        vm.rotation.z = 0.0f;  // drawn without rotation
        world.anm->drawScreen(sink, vm);
    }
    if (titleMode_) {
        if (scores_ && vms_[kVmTableAnchor].pos.x < 640.0f) {
            if (!spellsShown_ && renderedCharacter_ >= 0) {
                shownDifficulty_ = pageItem_;
                shownCharacter_ = renderedCharacter_;
                drawTable(gui, sink, world);
            } else if (spellsShown_ && renderedPage_ >= 0) {
                drawSpellList(gui, sink, world);
            }
        }
        return;
    }
    if (scores_) drawTable(gui, sink, world);
    if (state_ == State::NameEntry && rank_ < 10) drawKeys(gui);
    if (state_ == State::Statistics || state_ == State::StatisticsFade) drawStatistics(gui);
    if (state_ == State::ReplayQuestion || state_ == State::ReplayCannot || state_ == State::ReplaySlots ||
        state_ == State::ReplayName || state_ == State::ReplayOverwrite) {
        drawReplaySlots(gui, sink, world);
    }
}

}  // namespace th06
