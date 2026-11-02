#pragma once

// The game's text in another language, from Touhou Patch Center (thpatch.net) translation
// patches the launcher downloads on request: a stack of patch folders (lowest priority
// first) whose JSON files merge the way thcrap merges them. Applied by the engine itself:
//   th06/msgN.dat.jdiff   dialogue and boss title cards (thcrap's box-numbering rules)
//   th06/<end>.end.jdiff  ending and staff-roll text, by line number
//   th06/spells.js        spell card names by spell id
//   th06/stages.js        stage titles; themes.js: music titles; th06/musiccmt.js: Music Room
//   stringdefs.js         a few of the game's own strings (bomb names, the "♪" caption)
// Translated strings go through the text pipeline as UTF-8 marked with kUtf8TextMarker;
// markup and translator notes are reduced to plain text.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Json.hpp"

namespace th06 {

// `utf8` as game text (the UTF-8 marker, then the bytes).
std::vector<std::uint8_t> utf8GameText(std::string_view utf8);

class Translation {
  public:
    // Patch folders (each ending in '/'), lowest priority first. Empty: the original text.
    void setStack(std::vector<std::string> folders);
    bool active() const { return !folders_.empty(); }
    const std::vector<std::string>& folders() const { return folders_; }

    // A patch file merged over the stack (null: no patch has it). Cached.
    const Json* json(const std::string& path) const;

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

    // A patch line as plain text: markup (<b$..>, <c$text$ref>, ...) reduced to its text,
    // translator notes (after U+0014) dropped, and "%%" back to "%" for files thcrap's
    // server escapes for the game's printf.
    static std::string clean(std::string_view line, bool percentEscaped);

  private:
    std::vector<std::string> folders_;
    mutable std::map<std::string, std::optional<Json>> cache_;
};

}  // namespace th06
