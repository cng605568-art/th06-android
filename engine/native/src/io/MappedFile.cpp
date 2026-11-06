#include "io/MappedFile.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace touhou {

std::unique_ptr<MappedFile> MappedFile::open(const std::string& path, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = path + ": " + std::strerror(errno);
        return nullptr;
    }
    return adoptFd(fd, error);
}

std::unique_ptr<MappedFile> MappedFile::adoptFd(int fd, std::string* error) {
    std::unique_ptr<MappedFile> file(new MappedFile());

    struct stat st {};
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        const auto size = static_cast<std::size_t>(st.st_size);
        void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            file->mapping_ = map;
            file->data_ = static_cast<const std::uint8_t*>(map);
            file->size_ = size;
            ::close(fd);
            return file;
        }
    }

    // Fallback: read everything (pipes and providers that don't hand out real files).
    std::vector<std::uint8_t> bytes;
    std::uint8_t chunk[1 << 16];
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("read failed: ") + std::strerror(errno);
            ::close(fd);
            return nullptr;
        }
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    ::close(fd);
    file->owned_ = std::move(bytes);
    file->data_ = file->owned_.data();
    file->size_ = file->owned_.size();
    return file;
}

std::unique_ptr<MappedFile> MappedFile::fromMemory(std::vector<std::uint8_t> bytes) {
    std::unique_ptr<MappedFile> file(new MappedFile());
    file->owned_ = std::move(bytes);
    file->data_ = file->owned_.data();
    file->size_ = file->owned_.size();
    return file;
}

MappedFile::~MappedFile() {
    if (mapping_) ::munmap(mapping_, size_);
}

}  // namespace touhou
