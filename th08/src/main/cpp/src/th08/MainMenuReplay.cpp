// The title's Replay menu (main-menu state 7, claude/th08-specs/spec_replay_menu.md §1): the
// saved files (replay/th8_01.rpy .. th8_15.rpy, then up to 45 replay/th8_ud????.rpy), each
// file's stage slots with the score carried out of them, and the three playback modes. The
// lists are ASCII text drawn at invisible title01.anm anchor VMs (77-110), which slide in and
// out with the interrupts below; the only pictures are the banner, the slowdown caption and
// the mode captions (replay00.png). Choosing a mode starts the playback (scene Game with the
// replay flag); after it the title opens this menu again.

#include <algorithm>
#include <cstdio>
#include <format>
#include <string_view>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "io/Vfs.hpp"
#include "th08/Bgm.hpp"
#include "th08/ExeTables.hpp"
#include "th08/MainMenu.hpp"
#include "th08/Version.hpp"

namespace touhou::th08 {
namespace {

// title01.anm VMs (spec 1.1).
constexpr int kSlowVm = 78, kFileHeaderVm = 79, kFileRowVm = 80, kStageHeaderVm = 95, kStageRowVm = 96, kModeVm = 108;
constexpr int kPage = 15;
constexpr std::uint32_t kWhite = 0xFFFFFFFFu, kDim = 0xFF808080u, kFadedWhite = 0x60FFFFFFu, kFadedDim = 0x60808080u;

// The playback-mode interrupts (spec 1.1): 20 selected, 21 not.
constexpr int kModeOn = 20, kModeOff = 21;

std::string shortName(const ExeTables* tb, int c) {
    c = std::clamp(c, 0, 11);  // (the original reads past the table for a bad byte)
    if (tb && !tb->shortNames[static_cast<std::size_t>(c)].empty()) return tb->shortNames[static_cast<std::size_t>(c)];
    return std::format("Char{:<3d}", c);
}
std::string difficultyName(const ExeTables* tb, int d) {
    d = std::clamp(d, 0, 4);
    if (tb && !tb->replayDifficulties[static_cast<std::size_t>(d)].empty()) return tb->replayDifficulties[static_cast<std::size_t>(d)];
    return std::format("Level{:<3d}", d);
}
std::string stageLabel(const ExeTables* tb, int s) {
    if (tb && !tb->replayStages[static_cast<std::size_t>(s)].empty()) return tb->replayStages[static_cast<std::size_t>(s)];
    return std::format("Slot{:<3d}", s);
}

// FILES 3.10 / 6.7: the replay's version tag, size and checksum must be a line of the archive's
// version file (any line with that tag); "debug", or no version file, passes.
bool versionOk(const Vfs* vfs, const ReplaySummary& s) {
    const std::string tag = s.versionTag();
    if (tag == "debug" || !vfs) return true;
    const auto ver = vfs->read(kVersionFile);
    if (!ver) return true;
    return versionMatches(*ver, tag, ExeIdentity{s.exeSize(), s.exeChecksum()});
}

}  // namespace

// One vertical move over n rows (0x47030B): Up first, then Down; wrap.
int MainMenu::verticalMove(const MenuInput& in, int n) {
    if (n <= 0) return 0;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d == 0) return 0;
    ctx_.playSound(kSndMove);
    cursor_ += d;
    if (cursor_ < 0) cursor_ = n - 1;
    if (cursor_ >= n) cursor_ = 0;
    return d;
}

// From the title's Replay row (with SE 10) or the title entry after a playback (spec 1.2): the
// next pass is the menu's entry pass.
void MainMenu::enterReplays(bool sound) {
    if (sound) ctx_.playSound(kSndSelect);
    ctx_.practice = ctx_.spellPractice = false;
    setState(State::Replays);
    sendAll(13);
    hideDescription();
    // (the title handler returns before the timers' increment)
    inputTimer_ = stateTimer_ = idle_ = -1;
}

// The scan (spec 1.3 steps 3-5): files that open and pass the loader's checks, with at least
// one stage block (the original would quit on Select of a file with none).
void MainMenu::scanReplays() {
    replays_.clear();
    if (!ctx_.replayFiles || !ctx_.readReplay) return;
    for (const std::string& path : ctx_.replayFiles()) {
        if (replays_.size() >= static_cast<std::size_t>(kReplaySlots + kReplayUserFiles)) break;
        const auto bytes = ctx_.readReplay(path);
        if (bytes.empty()) continue;
        const auto file = ReplayFile::parse(bytes, nullptr, true);
        if (!file || file->firstStage() < 0 || !versionOk(ctx_.vfs, file->summary)) continue;
        ReplayEntry e;
        e.path = path;
        const std::string_view name = std::string_view(path).substr(path.rfind('/') + 1);
        int slot = 0;
        if (name.rfind("th8_ud", 0) != 0 && std::sscanf(std::string(name).c_str(), "th8_%d.rpy", &slot) == 1) {
            e.label = std::format("No.{:02d}", slot);
        } else {
            e.label = "User ";
        }
        e.summary = file->summary;
        for (int s = 0; s < kReplayStages; ++s) {
            const ReplayStage& st = file->stages[static_cast<std::size_t>(s)];
            e.present[static_cast<std::size_t>(s)] = st.present;
            e.carriedScore[static_cast<std::size_t>(s)] = st.carriedScore;
        }
        replays_.push_back(std::move(e));
    }
}

// The mode captions (VMs 108-110): the cursor's row 20, the others 21; the third row is kept
// at alpha 0 for a spell-practice replay (2 rows).
void MainMenu::highlightModes() {
    const bool spell = chosen_ && chosen_->summary.card() >= 0;
    for (int i = 0; i < 3; ++i) {
        const auto k = static_cast<std::size_t>(kModeVm + i);
        if (!has_[k]) continue;
        if (i == 2 && spell) vm_[k].color &= 0x00FFFFFFu;
        else vm_[k].interrupt(kModeOff);
    }
    const auto sel = static_cast<std::size_t>(kModeVm + std::clamp(cursor_, 0, 2));
    if (has_[sel]) vm_[sel].interrupt(kModeOn);
}

void MainMenu::replays(const MenuInput& in) {
    const int k = static_cast<int>(replays_.size());
    switch (sub_) {
        case 0: {  // entry and the scan (spec 1.3)
            if (inputTimer_ == 0) {
                loadBackground("title/select00.png");
                sendAll(14);
                cursor_ = 0;
                stateTimer_ = 0;
                descShown_ = false;
                chosen_.reset();
                scanReplays();
            }
            if (inputTimer_ >= 8) {
                sub_ = 1;
                stateTimer_ = 0;
            }
            return;
        }
        case 1: {  // the file list (spec 1.4)
            verticalMove(in, k);
            if (k > kPage) {
                if (in.pressedOrRepeat(BtnLeft)) {
                    cursor_ -= kPage;
                    if (cursor_ < 0) cursor_ += k;
                    ctx_.playSound(kSndMove);
                }
                if (in.pressedOrRepeat(BtnRight)) {
                    cursor_ += kPage;
                    if (cursor_ >= k) cursor_ -= k;
                    ctx_.playSound(kSndMove);
                }
            }
            chosenFile_ = cursor_;
            if (stateTimer_ < 10) return;
            if (in.pressed(kSelect)) {
                if (k == 0) return;
                const ReplayEntry& e = replays_[static_cast<std::size_t>(chosenFile_)];
                auto file = ReplayFile::parse(ctx_.readReplay ? ctx_.readReplay(e.path) : std::vector<std::uint8_t>{}, nullptr, true);
                if (!file || file->firstStage() < 0) {  // (gone or broken since the scan: list again)
                    log::warn("th08: {} can no longer be read", e.path);
                    scanReplays();
                    cursor_ = chosenFile_ = std::clamp(cursor_, 0, std::max(0, static_cast<int>(replays_.size()) - 1));
                    return;
                }
                ctx_.playSound(kSndSelect);
                sub_ = 2;
                sendAll(15);
                vm_[static_cast<std::size_t>(kFileRowVm + chosenFile_ % kPage)].interrupt(17);
                chosen_ = std::move(*file);
                cursor_ = 0;
                for (int s = 0; s < kReplayStages; ++s) {
                    if (e.present[static_cast<std::size_t>(s)]) {
                        cursor_ = s;
                        break;
                    }
                }
                chosenStage_ = cursor_;
                // The card name under the spell-practice line: resulttext.anm sprite 11 (the
                // 512 x 17 band at y 162), top-left anchored, MS Gothic 15 x 15.
                AnmVm& v = listText_[0];
                v = AnmVm{};
                if (resultTextLoaded_) v.initialiseWithSprite(*ctx_.anm, kSlotResultText, 11);
                v.flags |= 0x1800u;
                v.fontWidth = v.fontHeight = 15;
                renderText(v, listLines_[0], chosen_->summary.cardName());
                v.color = kWhite;
                return;
            }
            if (in.pressed(kCancel)) {
                ctx_.playSound(kSndBack);
                sub_ = 4;
                stateTimer_ = 0;
                sendAll(16);
            }
            return;
        }
        case 2: {  // the stage list (spec 1.5): no input delay, no sounds on Select/Cancel
            if (!chosen_) {
                sub_ = 1;
                return;
            }
            const ReplayEntry& e = replays_[static_cast<std::size_t>(chosenFile_)];
            const int d = verticalMove(in, kReplayStages);
            for (int guard = 0; d != 0 && guard < kReplayStages && !e.present[static_cast<std::size_t>(cursor_)]; ++guard) {
                cursor_ += d;
                if (cursor_ < 0) cursor_ = kReplayStages - 1;
                if (cursor_ >= kReplayStages) cursor_ = 0;
            }
            chosenStage_ = cursor_;
            if (in.pressed(kSelect)) {
                sendAll(19);
                vm_[static_cast<std::size_t>(kFileRowVm + chosenFile_ % kPage)].interrupt(17);
                sub_ = 3;
                cursor_ = 0;
                highlightModes();
                return;
            }
            if (in.pressed(kCancel)) {
                chosen_.reset();
                sub_ = 1;
                inputTimer_ = 0;
                sendAll(14);
                cursor_ = chosenFile_;
            }
            return;
        }
        case 3: {  // the playback mode (spec 1.6)
            if (!chosen_) {
                sub_ = 1;
                return;
            }
            const int card = chosen_->summary.card();
            if (verticalMove(in, card < 0 ? 3 : 2) != 0) highlightModes();
            if (in.pressed(kSelect)) {
                const ReplayEntry& e = replays_[static_cast<std::size_t>(chosenFile_)];
                ctx_.replayFlag = true;
                ctx_.demo = false;
                ctx_.replayPath = e.path;
                ctx_.replayStage = chosenStage_;
                ctx_.replayMode = cursor_;
                ctx_.spellPractice = card >= 0;
                if (card >= 0) ctx_.card = card;
                chosen_.reset();
                if (ctx_.music) ctx_.music->stop();
                removed_ = true;
                if (ctx_.request) ctx_.request(Scene::Game);
                return;
            }
            if (in.pressed(kCancel)) {
                sub_ = 2;
                inputTimer_ = 0;
                cursor_ = chosenStage_;
                sendAll(15);
                vm_[static_cast<std::size_t>(kFileRowVm + chosenFile_ % kPage)].interrupt(17);
            }
            return;
        }
        case 4:  // back to the title (spec 1.7)
            if (stateTimer_ >= 30) {
                titleCursor_ = Replay;
                setState(State::Title);
            }
            return;
        default:
            sub_ = 1;
            return;
    }
}

// The strings of the replay menu's draw (spec 1.8), at the anchors' current positions.
std::vector<MainMenu::TextLine> MainMenu::replayMenuText() const {
    std::vector<TextLine> out;
    if (state_ != State::Replays) return out;
    const ExeTables* tb = ctx_.tables;
    auto at = [&](int vm) {
        const AnmVm& v = vm_[static_cast<std::size_t>(vm)];
        return std::pair{v.position.x, v.position.y};
    };
    auto add = [&](int vm, std::string text, std::uint32_t color, bool selected) {
        const auto [x, y] = at(vm);
        out.push_back(TextLine{x, y, std::move(text), color, selected});
    };
    add(kFileHeaderVm, "No.   Name       Date  Player   Rank", kWhite, false);
    const int k = static_cast<int>(replays_.size());
    const int page = chosenFile_ - chosenFile_ % kPage;
    for (int i = page; i < page + kPage && i < k; ++i) {
        const ReplayEntry& e = replays_[static_cast<std::size_t>(i)];
        const ReplaySummary& s = e.summary;
        const bool chosen = i == chosenFile_;
        std::string line;
        if (s.card() < 0) {
            line = std::format("{} {:>8}  {:>6} {:>7}  {:>8}", e.label, s.name(), s.date(), shortName(tb, s.character()), difficultyName(tb, s.difficulty()));
        } else {
            line = std::format("{} {:>8}  {:>6} {:>7}  Spell No.{:3d}", e.label, s.name(), s.date(), shortName(tb, s.character()), s.card() + 1);
        }
        add(kFileRowVm + (i - page), std::move(line), chosen ? kWhite : kDim, chosen);
    }
    if ((sub_ != 2 && sub_ != 3) || !chosen_ || chosenFile_ >= k) return out;
    const ReplayFile& f = *chosen_;
    add(kSlowVm, std::format("       {:2.3f}%", f.summary.slowdown()), kWhite, false);
    add(kStageHeaderVm, "Stage    LastScore", kWhite, false);
    if (f.summary.card() < 0) {
        for (int s = 0; s < kReplayStages; ++s) {
            const ReplayStage& st = f.stages[static_cast<std::size_t>(s)];
            const bool on = s == chosenStage_;
            std::uint32_t color = on ? kWhite : kDim;
            if (sub_ == 3) color = on ? kFadedWhite : kFadedDim;
            std::string line = st.present ? std::format("{} {:9d}0", stageLabel(tb, s), static_cast<std::int32_t>(st.carriedScore))
                                          : std::format("{} ----------", stageLabel(tb, s));
            add(kStageRowVm + s, std::move(line), color, sub_ == 2 && on);
        }
    } else {
        add(kStageRowVm + 1, std::format("No.{:03d} {:9d}0", f.summary.card() + 1, static_cast<std::int32_t>(f.summary.score())), kWhite, false);
    }
    return out;
}

void MainMenu::drawReplays(SpriteSink& sink) {
    for (TextLine& l : replayMenuText()) ascii_.add(l.x, l.y, l.text, l.color, 1.0f, 13.0f, -1.0f, l.selected);
    // A spell-practice replay's card name, one line under its "No." line.
    if ((sub_ == 2 || sub_ == 3) && chosen_ && chosen_->summary.card() >= 0) {
        const AnmVm& anchor = vm_[static_cast<std::size_t>(kStageRowVm + 1)];
        AnmVm v = listText_[0];
        v.position = {anchor.position.x, anchor.position.y + 16.0f, anchor.position.z};
        drawVm(sink, v, AnmDrawState{});
    }
}

}  // namespace touhou::th08
