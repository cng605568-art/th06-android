#include "io/Vfs.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

#include "core/Log.hpp"

namespace touhou {

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

namespace {
std::string loosePath(std::string_view p) {
    std::string s(p);
    for (char& c : s) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
}  // namespace

void Vfs::addLooseFile(std::string relativePath, std::unique_ptr<MappedFile> file) {
    if (file) loose_[loosePath(relativePath)] = std::move(file);
}

std::span<const std::uint8_t> Vfs::looseFile(std::string_view relativePath) const {
    const auto it = loose_.find(loosePath(relativePath));
    return it == loose_.end() ? std::span<const std::uint8_t>() : it->second->bytes();
}

std::size_t Vfs::mountDirectory(const std::string& dir, bool preferNewClassic) {
    namespace fs = std::filesystem;
    struct Candidate {
        fs::path path;
        bool newClassic;
    };
    std::vector<Candidate> found;

    // The 2002 game's music: loose WAV files in bgm/.
    {
        std::error_code ec;
        const fs::path bgm = fs::path(dir) / "bgm";
        if (fs::is_directory(bgm, ec)) {
            for (const auto& item : fs::directory_iterator(bgm, ec)) {
                std::string name = item.path().filename().string();
                std::string lower = loosePath(name);
                if (!item.is_regular_file(ec) || !lower.ends_with(".wav")) continue;
                std::string err;
                if (auto f = MappedFile::open(item.path().string(), &err)) addLooseFile("bgm/" + name, std::move(f));
            }
        }
    }

    for (const fs::path& base : {fs::path(dir), fs::path(dir) / "data"}) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) continue;
        for (const auto& item : fs::directory_iterator(base, ec)) {
            if (!item.is_regular_file(ec)) continue;
            std::string ext = item.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".exe") {
                std::string err;
                auto exe = MappedFile::open(item.path().string(), &err);
                if (exe && looksLikeGameExecutable(exe->bytes())) addExecutable(item.path().filename().string(), std::move(exe));
                continue;
            }
            if (ext != ".dat" || item.file_size(ec) < 8) continue;  // magic decides, not size
            if (base == fs::path(dir) && loosePath(item.path().filename().string()) == "score.dat") {
                std::string err;  // the PC game's high scores (read until the port has its own)
                if (auto f = MappedFile::open(item.path().string(), &err)) addLooseFile("score.dat", std::move(f));
                continue;
            }
            std::string err;
            auto file = MappedFile::open(item.path().string(), &err);
            if (!file || file->bytes().size() < 4) continue;
            const auto magic = file->bytes().first(4);
            const bool pbg3 = std::equal(magic.begin(), magic.end(), "PBG3");
            const bool pbg4 = std::equal(magic.begin(), magic.end(), "PBG4");
            const bool pkgl = std::equal(magic.begin(), magic.end(), "PKGL");
            if (std::equal(magic.begin(), magic.end(), "ZWAV")) {  // th07's music (thbgm.dat): a loose file
                addLooseFile(item.path().filename().string(), std::move(file));
                continue;
            }
            if (pbg3 || pbg4 || pkgl) found.push_back({item.path(), pkgl});
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
    const std::string name = normalize(path);
    const auto it = index_.find(name);
    std::optional<std::vector<std::uint8_t>> out;
    if (it != index_.end()) {
        out.emplace();
        std::string err;
        if (!it->second.archive->extract(it->second.index, *out, &err)) {
            log::error("Failed to read {}: {}", path, err);
            out.reset();
        }
    }
    if (filter_) return filter_(name, std::move(out));
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

void Vfs::addExecutable(std::string name, std::unique_ptr<MappedFile> file) {
    if (!file) return;
    log::info("Found program {} ({} KB)", name, file->bytes().size() / 1024);
    executables_.emplace_back(std::move(name), std::move(file));
}

std::vector<std::span<const std::uint8_t>> Vfs::executables() const {
    std::vector<std::span<const std::uint8_t>> out;
    for (const auto& [name, file] : executables_) out.push_back(file->bytes());
    return out;
}

bool Vfs::looksLikeGameExecutable(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 256 * 1024 && bytes.size() <= 4 * 1024 * 1024 && bytes[0] == 'M' && bytes[1] == 'Z';
}

}  // namespace touhou
