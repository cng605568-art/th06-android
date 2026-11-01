#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "io/Archive.hpp"
#include "io/MappedFile.hpp"

namespace th06 {

// Virtual file system over the player's mounted archives.
//
// Lookups use the lower-cased base name only, like the original engine (which strips the
// directory from "data/etama3.anm" and searches every archive for "etama3.anm").
// The first archive mounted that contains a name wins.
class Vfs {
  public:
    // Returns false if the archive was empty; ownership is taken either way.
    bool mount(std::unique_ptr<Archive> archive);

    // Mounts every PBG3/PKGL archive found directly in `dir` and in `dir/data`.
    // The 2002 format is mounted first unless preferNewClassic is set.
    std::size_t mountDirectory(const std::string& dir, bool preferNewClassic);

    bool exists(std::string_view path) const;
    std::optional<std::vector<std::uint8_t>> read(std::string_view path) const;

    std::size_t archiveCount() const { return archives_.size(); }
    std::size_t fileCount() const { return index_.size(); }
    bool hasFormat(std::string_view format) const;
    std::vector<std::string> describe() const;

    static std::string normalize(std::string_view path);

    // The player's game executable(s), kept as files of their own (not archive members): the
    // engine reads a few data tables from the 1.02h executable. Candidates are Windows
    // programs of plausible size; which one is the game is decided by its checksum.
    void addExecutable(std::string name, std::unique_ptr<MappedFile> file);
    std::vector<std::span<const std::uint8_t>> executables() const;
    static bool looksLikeGameExecutable(std::span<const std::uint8_t> bytes);

  private:
    struct Ref {
        const Archive* archive;
        std::size_t index;
    };
    std::vector<std::unique_ptr<Archive>> archives_;
    std::vector<std::pair<std::string, std::unique_ptr<MappedFile>>> executables_;
    std::unordered_map<std::string, Ref> index_;
};

}  // namespace th06
