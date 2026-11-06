#pragma once

// th06's text in another language, from the Touhou Patch Center translation patches the
// launcher downloads on request (the patch stack: text/PatchStack). Applied by the game itself:
//   th06/msgN.dat.jdiff   dialogue and boss title cards (thcrap's box-numbering rules)
//   th06/<end>.end.jdiff  ending and staff-roll text, by line number
//   th06/spells.js        spell card names by spell id
//   th06/stages.js        stage titles; themes.js: music titles; th06/musiccmt.js: Music Room
//   stringdefs.js         a few of the game's own strings (bomb names, the "♪" caption)
// Translated strings go through the text pipeline as UTF-8 marked with kUtf8TextMarker;
// markup and translator notes are reduced to plain text.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "text/PatchStack.hpp"

namespace touhou::th06 {

class Translation : public PatchStack {
  public:
    Translation() : PatchStack("v1.02h") {}

    std::optional<std::string> spellName(int spellId) const;   // 0-based ECL id
    std::optional<std::string> stageTitle(int stage) const;    // 1..7
    std::optional<std::string> musicTitle(int track) const;    // 1..17
    std::optional<std::string> stringDef(const std::string& id) const;
    // The in-game music caption ("♪" + title, from stringdefs).
    std::string musicCaption(const std::string& title) const;
    // A Music Room comment: one entry per line ("@" = the numbered title), or nothing.
    std::optional<std::vector<std::string>> musicComment(int track) const;

    // Rewrites msgN.dat / an .end file in place with the merged jdiff (false: unchanged).
    bool patchMsg(std::vector<std::uint8_t>& msg, int stage) const;
    bool patchEnd(std::vector<std::uint8_t>& end, std::string_view fileName) const;
};

}  // namespace touhou::th06
