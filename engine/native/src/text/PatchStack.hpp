#pragma once

// A game's text in another language, from Touhou Patch Center (thpatch.net) translation
// patches: a stack of patch folders (lowest priority first) whose JSON files merge the way
// thcrap merges them, with thcrap's build-specific variants (spells.v1.02h.js next to
// spells.js). Which files a game reads, and how it applies them, is the game's part.
// Translated strings go through the text pipeline as UTF-8 marked with kUtf8TextMarker.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Json.hpp"

namespace touhou {

// `utf8` as game text (the UTF-8 marker, then the bytes).
std::vector<std::uint8_t> utf8GameText(std::string_view utf8);

class PatchStack {
  public:
    // `build`: the game build thcrap's variant files are named for ("v1.02h").
    explicit PatchStack(std::string build) : build_(std::move(build)) {}

    // Patch folders (each ending in '/'), lowest priority first. Empty: the original text.
    void setStack(std::vector<std::string> folders);
    bool active() const { return !folders_.empty(); }
    const std::vector<std::string>& folders() const { return folders_; }

    // A patch file merged over the stack (null: no patch has it). Cached.
    const Json* json(const std::string& path) const;

    // A patch line as plain text: markup (<b$..>, <c$text$ref>, ...) reduced to its text,
    // translator notes (after U+0014) dropped, and "%%" back to "%" for files thcrap's
    // server escapes for the game's printf.
    static std::string clean(std::string_view line, bool percentEscaped);

  private:
    std::string build_;
    std::vector<std::string> folders_;
    mutable std::map<std::string, std::optional<Json>> cache_;
};

}  // namespace touhou
