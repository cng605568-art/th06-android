#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "io/MappedFile.hpp"

namespace touhou {

struct ArchiveEntry {
    std::string name;              // as stored, e.g. "etama3.anm"
    std::uint64_t offset = 0;      // absolute offset of the stored data
    std::uint64_t storedSize = 0;  // bytes on disk
    std::uint64_t size = 0;        // bytes after decoding
    std::uint32_t key = 0;         // PBG3: byte-sum checksum; PKGL: per-member XOR seed
    std::uint16_t flags = 0;       // PKGL: 1 = Zstandard frame
};

// One game archive, opened read-only over a MappedFile. extract() is const and keeps no
// per-call state, so members can be decoded from several threads at once.
class Archive {
  public:
    virtual ~Archive() = default;

    virtual std::string_view format() const = 0;
    virtual bool extract(std::size_t index, std::vector<std::uint8_t>& out,
                         std::string* error) const = 0;

    const std::string& label() const { return label_; }
    const std::vector<ArchiveEntry>& entries() const { return entries_; }

  protected:
    std::string label_;
    std::vector<ArchiveEntry> entries_;
    std::unique_ptr<MappedFile> file_;
};

// Sniffs the 4-byte magic and opens the matching format:
//   "PBG3" - the 2002 release (紅魔郷CM.DAT / KOUMAKYO_CM.dat ...): bit-packed table + LZSS.
//   "PKGL" - New Classic (2026, th06CM.dat ...): XOR-keystream table and payloads + zstd.
//   "PBG4" - Perfect Cherry Blossom (th07.dat): LZSS-compressed table + LZSS members.
// fileName is the archive's own file name; New Classic derives its table key from it.
std::unique_ptr<Archive> openArchive(std::string fileName, std::unique_ptr<MappedFile> file,
                                     std::string* error);

std::unique_ptr<Archive> openPbg3(std::string fileName, std::unique_ptr<MappedFile> file,
                                  std::string* error);
std::unique_ptr<Archive> openPkgl(std::string fileName, std::unique_ptr<MappedFile> file,
                                  std::string* error);
std::unique_ptr<Archive> openPbg4(std::string fileName, std::unique_ptr<MappedFile> file,
                                  std::string* error);

}  // namespace touhou
