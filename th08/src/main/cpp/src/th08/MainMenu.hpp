#pragma once

// Imperishable Night's main menu (claude/th08-specs/spec_flow_menus.md §3): title00.png
// behind the 142 VMs of title01.anm (VM i runs script i), the nine title rows with their
// descriptions (Shift-JIS strings from th08.exe, rendered into text.anm sprites), the white
// fade-in and the title song. The screens behind the rows are ported one by one; a row whose
// screen is not ported yet says so.

#include <array>
#include <cstdint>

#include "th08/Anm.hpp"
#include "th08/Flow.hpp"

namespace touhou {
class SpriteSink;
}

namespace touhou::th08 {

class MainMenu {
  public:
    static constexpr int kInstances = 142;
    static constexpr int kDescriptions = 14;
    static constexpr int kTitleRows = 9;
    enum Row { GameStart, ExtraStart, SpellPractice, PracticeStart, Replay, PlayerData, MusicRoom, Option, Quit };

    explicit MainMenu(FlowContext& ctx) : ctx_(ctx) {}

    // Loads title01.anm and the background and sets up the description VMs (spec 3.2).
    bool create(std::string* error);
    void update(const MenuInput& in);
    void draw(SpriteSink& sink);
    bool removed() const { return removed_; }
    int cursor() const { return cursor_; }
    int subState() const { return sub_; }

  private:
    void enterTitle();
    void title(const MenuInput& in);
    void highlightRows();
    void showDescription(int row);
    void sendAll(int interrupt);
    bool rowLocked(int row) const;

    FlowContext& ctx_;
    std::array<AnmVm, kInstances> vm_{};
    std::array<std::int16_t, kInstances> base_{};
    std::array<bool, kInstances> has_{};
    bool instancesMade_ = false;
    std::array<AnmVm, kDescriptions> desc_{};
    int currentDesc_ = 0;
    bool descShown_ = false;
    Surface background_;
    AsciiText ascii_;

    int sub_ = 0;          // the title's sub-state: 0 entry, 1 input, 2 quitting, 3 to Option
    int stateTimer_ = 0;   // MM+0x10
    int inputTimer_ = 0;   // MM+0x1442C
    int idle_ = 0;         // MM+0x14430
    int cursor_ = 0;
    int remembered_ = -1;
    bool firstEntry_ = true;
    int fade_ = 0;          // the white fade-in: frames left of 70
    int notice_ = 0;        // "not ported yet": frames left
    bool removed_ = false;
};

}  // namespace touhou::th08
