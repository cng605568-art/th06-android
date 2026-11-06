#include "io/Archive.hpp"

#include <cstring>

namespace touhou {

std::unique_ptr<Archive> openArchive(std::string fileName, std::unique_ptr<MappedFile> file,
                                     std::string* error) {
    if (!file) {
        if (error) *error = fileName + ": no data";
        return nullptr;
    }
    const auto bytes = file->bytes();
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "PBG3", 4) == 0) {
        return openPbg3(std::move(fileName), std::move(file), error);
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "PKGL", 4) == 0) {
        return openPkgl(std::move(fileName), std::move(file), error);
    }
    if (error) *error = fileName + ": not a PBG3 or PKGL archive";
    return nullptr;
}

}  // namespace touhou
