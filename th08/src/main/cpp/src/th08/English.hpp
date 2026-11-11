#pragma once

// The optional English text mode: the player's own copy of the community English patch for
// Imperishable Night (Touhou Patch Center's lang_en for thcrap, with the patches it depends
// on), read at run time from the files the launcher imports from the game folder
// (thcrap/repos/...) or downloads from thpatch, or on the desktop from the game folder itself.
// Nothing of the patch ships with the port. (Adapted from the TH07 port's English mode.)
//
// How each part of the patch is applied, following thcrap (public domain, the Unlicense;
// the places it is adapted from are named at each function):
//   th08/<file>             replaces the game's file of that name (base_tsa's wider
//                           text.anm, the info pictures)
//   th08/msgN.dat.jdiff     dialogue: thcrap's msg08 format (hard lines, auto lines, auto
//                           box ends), the box keys and line replacement, insertion and
//                           deletion at the file level, the text masked with 0x77 again
//   th08/endNN.end.jdiff    ending text by line number; layout markup (<l$Reimu:$12345678>
//                           columns, <i$...> italics) is laid out when a line is drawn
//   th08/data/...png        pictures over the THTX textures of the ANM entries of that name
//                           (stage titles, menus, side panel, result screens), blitted the
//                           way thcrap's ANM patcher does
//   th08/spells.js, th08/spellcomments.js
//                           spell card names (banner, Spell Card Practice, Player Data) and
//                           the practice page's owner and comment lines; looked up at display
//                           time, never stored (score.dat keeps the Shift-JIS names)
//   themes.js, th08/musiccmt.js
//                           music titles and the Music Room's comments
//   th08/stringlocs.js, stringdefs.js
//                           the program's own strings by address (bomb names, menu and
//                           practice texts, Player Data labels)
// Translated text goes through the text pipeline as UTF-8 behind kUtf8TextMarker. A
// translator's note (after U+0014 in a dialogue line) is kept as "\x14<index>"; it is not
// shown yet.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/Json.hpp"
#include "gfx/TextRenderer.hpp"
#include "text/PatchStack.hpp"

namespace touhou {
struct Image;
}  // namespace touhou

namespace touhou::th08 {

class English {
  public:
    // thcrap's patch stack for lang_en under `thcrapRoot` (the folder holding repos/): the
    // patches its patch.js depends on, depth-first as thcrap resolves them, then lang_en.
    // Patches that are not there are left out. Empty: no English patch in that folder.
    static std::vector<std::string> discoverStack(const std::string& thcrapRoot);

    // `stack`: patch folders, lowest priority first.
    explicit English(std::vector<std::string> stack);
    const std::vector<std::string>& stack() const { return patches_.folders(); }
    bool active() const { return patches_.active(); }

    // The game's files as read through the Vfs (installed as its read filter): a
    // replacement from the stack, then the jdiff / PNG patches for that kind of file, and
    // Thread-safe.
    std::optional<std::vector<std::uint8_t>> filterFile(const std::string& name, std::optional<std::vector<std::uint8_t>> bytes) const;

    // The pieces (public for the tests). Each returns false when nothing changed.
    // msg08 dialogue (thcrap_tsa/src/th06_msg.cpp, patch_msg with the MSG_TH08 format).
    bool patchMsg(std::vector<std::uint8_t>& msg, const Json& diff) const;
    // .end text by line number (thcrap_tsa/src/th06_msg.cpp, patch_end_th06).
    static bool patchEnd(std::vector<std::uint8_t>& end, const Json& diff);
    // PNG replacements over the THTX textures of an .anm file (thcrap_tsa/src/anm.cpp).
    bool patchAnm(std::vector<std::uint8_t>& anm, const std::string& anmName) const;
    // One PNG over one THTX pixel block (format 1, 3 or 5, `w` x `h`), sprite by sprite
    // (`sprites`: {x, y, w, h} rectangles of the entry; empty: the whole texture).
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
    };
    static bool blitPng(std::uint8_t* thtx, int format, int w, int h, const std::vector<Rect>& sprites, const Image& png, int offsetX,
                        int offsetY);

    // Display-time lookups (UTF-8, markup kept, translator notes dropped).
    std::optional<std::string> spellName(int card) const;   // 0-based, as the ECL numbers them
    // Spell Card Practice's owner line and comment lines for a card (spellcomments.js).
    struct SpellComment {
        std::optional<std::string> owner;
        std::vector<std::string> lines;
    };
    std::optional<SpellComment> spellComment(int card) const;
    std::optional<std::string> musicTitle(int track) const;  // 1-based (th08_01 .. th08_21)
    // A Music Room comment: index 0 is the numbered title line; entries may be empty (an
    // empty line) or missing (std::nullopt: keep the game's line).
    std::optional<std::vector<std::optional<std::string>>> musicComment(int track) const;
    // A string of th08.exe 1.00d by virtual address (stringlocs.js and stringdefs.js).
    std::optional<std::string> exeString(std::uint32_t address) const;
    std::optional<std::string> stringDef(const std::string& id) const;
    // A translator's note kept while patching a dialogue file ("\x14<index>" in the line).
    std::string note(int index) const;

    // `utf8` as game text: the UTF-8 marker, then the bytes.
    static std::string gameText(std::string_view utf8);

  private:
    // The highest-priority file for `rel` (with thcrap's build variant), or "".
    std::string resolve(const std::string& rel) const;
    // Every stack file for `rel`, lowest priority first.
    std::vector<std::string> resolveAll(const std::string& rel) const;
    const Json* json(const std::string& path) const;
    std::string keepNote(std::string_view line) const;  // "text\x14note" -> "text\x14<n>"

    mutable std::recursive_mutex mutex_;
    mutable PatchStack patches_;
    std::map<std::string, std::string> files_;  // "<folder>|th08/..." (lower-cased) -> the file's path
    mutable std::vector<std::string> notes_;
    mutable std::map<std::string, int> noteIndex_;
    // The last few patched ANM files (the texture loaders read a file again for each of its
    // entries) and which patches were logged.
    mutable std::vector<std::pair<std::string, std::vector<std::uint8_t>>> anmCache_;
    mutable std::set<std::string> logged_;
};

// ---- thcrap's layout markup ------------------------------------------------------------------

// One piece of a laid-out line: its text, where it starts and how wide it is (texels).
struct TextRun {
    std::string text;
    float x = 0, width = 0;
    bool italic = false;
};

// Lays out a line with thcrap's markup (thcrap_tsa/src/layout.cpp, layout_process): plain
// text, and commands <cmd$text$measure...> where the letters b/i (bold, italic) style the
// text and c/r/l/s place it in a column (centred, right, left, measured only) as wide as the
// width of `measure`, or up to the next tab stop (<t$...> defines one). `measure` gives a
// string's natural width. Returns the runs to draw; `total` is the laid-out width.
std::vector<TextRun> layoutMarkup(std::string_view line, const std::function<float(std::string_view, bool italic)>& measure,
                                  float bandWidth, float* total);

// Draws translated text (UTF-8, markup allowed, no marker) into `band`, laid out within
// [left, left + width) with `align`, narrowed as a whole when it is wider. Returns the width
// drawn (0: nothing).
float renderTranslated(TextRenderer& renderer, std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                       Image& band);

// Breaks text into lines no wider than `width` at spaces (and at '\n'); a word wider than
// that gets a line of its own.
std::vector<std::string> wrapText(std::string_view text, float width, const std::function<float(std::string_view)>& measure);

// printf for the patch's format strings (for example "#%2d - %s"): %d with a width, %s, %%.
std::string formatPatchString(std::string_view format, int number, std::string_view text);

}  // namespace touhou::th08
