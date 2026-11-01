#include "game/ResultScreen.hpp"

#include <algorithm>
#include <cstring>
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

bool ResultScreen::start(World& world, const Vfs& vfs, TextureLibrary& textures, Gui& gui, ScoreFile& scores,
                         const Run& run) {
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
    scores_ = &scores;
    run_ = run;
    for (int i = 0; i < kVms; ++i) {
        AnmVm& vm = vms_[static_cast<std::size_t>(i)];
        vm = AnmVm{};
        anm.setAndExecuteScript(vm, kScriptFirst + i);
        vm.pos = {};
        vm.posOffset = {};
    }
    for (int i = 0; i < kTexts; ++i) {
        AnmVm& vm = texts_[static_cast<std::size_t>(i)];
        vm = AnmVm{};
        anm.initializeAndSetSprite(vm, kTextSpriteFirst + i);
        vm.anchor = AnmAnchor::TopLeft;
        vm.visible = false;
    }
    (void)gui;
    state_ = State::NameEntry;
    t_ = 0;
    rank_ = 10;
    key_ = cursor_ = 0;
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
    prevButtons_ = bits;
    return e;
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
            if (t_ == 30) {  // (the save-replay prompt would follow; no replays here)
                state_ = State::Exiting;
                interruptAll(2);
                t_ = 0;
            }
            break;
        case State::Exiting:
            if (t_ >= 60) {
                state_ = State::Done;
                return true;
            }
            break;
        case State::Done:
            return true;
    }
    for (AnmVm& vm : vms_) world.anm->executeScript(vm);
    ++t_;
    return false;
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
        world.anm->drawScreen(sink, vm);
    }
    if (scores_) drawTable(gui, sink, world);
    if (state_ == State::NameEntry && rank_ < 10) drawKeys(gui);
    if (state_ == State::Statistics || state_ == State::StatisticsFade) drawStatistics(gui);
}

}  // namespace th06
