#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace touhou {

// Read-only view of a whole file. Regular files are memory-mapped, so a multi-gigabyte
// archive costs no RAM until members are actually read. Non-mappable descriptors (pipes
// from some cloud document providers) fall back to reading into memory.
class MappedFile {
  public:
    static std::unique_ptr<MappedFile> open(const std::string& path, std::string* error);
    // Takes ownership of fd (always closed before returning).
    static std::unique_ptr<MappedFile> adoptFd(int fd, std::string* error);
    static std::unique_ptr<MappedFile> fromMemory(std::vector<std::uint8_t> bytes);

    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const std::uint8_t> bytes() const { return {data_, size_}; }
    bool isMapped() const { return mapping_ != nullptr; }

  private:
    MappedFile() = default;

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    void* mapping_ = nullptr;
    std::vector<std::uint8_t> owned_;
};

}  // namespace touhou
