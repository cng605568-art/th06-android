#include "io/Vfs.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

#include "core/Log.hpp"

namespace th06 {

std::string Vfs::normalize(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    std::string out(path);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool Vfs::mount(std::unique_ptr<Archive> archive) {
    if (!archive) return false;
    std::size_t added = 0;
    const auto& entries = archive->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (index_.try_emplace(normalize(entries[i].name), Ref{archive.get(), i}).second) ++added;
    }
    log::info("Mounted {} [{}]: {} files ({} new)", archive->label(), archive->format(),
              entries.size(), added);
    const bool useful = !entries.empty();
    archives_.push_back(std::move(archive));
    return useful;
}

std::size_t Vfs::mountDirectory(const std::string& dir, bool preferNewClassic) {
    namespace fs = std::filesystem;
    struct Candidate {
        fs::path path;
        bool newClassic;
    };
    std::vector<Candidate> found;

    for (const fs::path& base : {fs::path(dir), fs::path(dir) / "data"}) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) continue;
        for (const auto& item : fs::directory_iterator(base, ec)) {
            if (!item.is_regular_file(ec)) continue;
            std::string ext = item.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".dat" || item.file_size(ec) < 8) continue;  // magic decides, not size
            std::string err;
            auto file = MappedFile::open(item.path().string(), &err);
            if (!file || file->bytes().size() < 4) continue;
            const auto magic = file->bytes().first(4);
            const bool pbg3 = std::equal(magic.begin(), magic.end(), "PBG3");
            const bool pkgl = std::equal(magic.begin(), magic.end(), "PKGL");
            if (pbg3 || pkgl) found.push_back({item.path(), pkgl});
        }
    }

    std::sort(found.begin(), found.end(), [&](const Candidate& a, const Candidate& b) {
        const bool aFirst = a.newClassic == preferNewClassic;
        const bool bFirst = b.newClassic == preferNewClassic;
        if (aFirst != bFirst) return aFirst;
        return a.path.filename() < b.path.filename();
    });

    std::size_t mounted = 0;
    for (const Candidate& c : found) {
        std::string err;
        auto file = MappedFile::open(c.path.string(), &err);
        auto archive = file ? openArchive(c.path.filename().string(), std::move(file), &err)
                            : nullptr;
        if (!archive) {
            log::warn("Skipping {}: {}", c.path.string(), err);
            continue;
        }
        if (mount(std::move(archive))) ++mounted;
    }
    return mounted;
}

bool Vfs::exists(std::string_view path) const { return index_.contains(normalize(path)); }

std::optional<std::vector<std::uint8_t>> Vfs::read(std::string_view path) const {
    const auto it = index_.find(normalize(path));
    if (it == index_.end()) return std::nullopt;
    std::vector<std::uint8_t> out;
    std::string err;
    if (!it->second.archive->extract(it->second.index, out, &err)) {
        log::error("Failed to read {}: {}", path, err);
        return std::nullopt;
    }
    return out;
}

bool Vfs::hasFormat(std::string_view format) const {
    return std::any_of(archives_.begin(), archives_.end(),
                       [&](const auto& a) { return a->format() == format; });
}

std::vector<std::string> Vfs::describe() const {
    std::vector<std::string> out;
    for (const auto& a : archives_) {
        out.push_back(std::format("{} [{}] {} files", a->label(), a->format(), a->entries().size()));
    }
    return out;
}

}  // namespace th06
