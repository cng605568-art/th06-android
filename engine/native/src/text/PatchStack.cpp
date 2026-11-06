#include "text/PatchStack.hpp"

#include <cctype>
#include <fstream>
#include <iterator>

#include "core/Log.hpp"
#include "gfx/TextRenderer.hpp"

namespace touhou {
namespace {

std::optional<std::string> readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// thcrap's build-specific variant: ".<build>" before the first dot of the file name.
std::string buildVariant(const std::string& path, const std::string& build) {
    const std::size_t slash = path.rfind('/');
    const std::size_t base = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.find('.', base);
    if (dot == std::string::npos) return path + "." + build;
    return path.substr(0, dot) + "." + build + path.substr(dot);
}

std::string cleanMarkup(std::string_view s, int depth) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<' && depth < 8) {
            std::size_t j = i + 1;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            if (j > i + 1 && j < s.size() && s[j] == '$') {
                std::vector<std::string_view> params;
                int nest = 1;
                std::size_t k = j + 1, start = j + 1;
                for (; k < s.size(); ++k) {
                    if (s[k] == '<') ++nest;
                    else if (s[k] == '>' && --nest == 0) break;
                    else if (s[k] == '$' && nest == 1) {
                        params.push_back(s.substr(start, k - start));
                        start = k + 1;
                    }
                }
                if (k < s.size()) {
                    params.push_back(s.substr(start, k - start));
                    const std::string_view cmd = s.substr(i + 1, j - i - 1);
                    // s (measure only) and ts (tab header) draw nothing; the others draw their text.
                    if (cmd != "s" && cmd != "ts") out += cleanMarkup(params.front(), depth + 1);
                    i = k + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> utf8GameText(std::string_view utf8) {
    std::vector<std::uint8_t> out;
    out.reserve(utf8.size() + 1);
    out.push_back(kUtf8TextMarker);
    out.insert(out.end(), utf8.begin(), utf8.end());
    return out;
}

void PatchStack::setStack(std::vector<std::string> folders) {
    folders_ = std::move(folders);
    for (std::string& f : folders_) {
        if (!f.empty() && f.back() != '/') f.push_back('/');
    }
    cache_.clear();
    if (!folders_.empty()) log::info("Game text: {} translation patch(es), top: {}", folders_.size(), folders_.back());
}

const Json* PatchStack::json(const std::string& path) const {
    if (folders_.empty()) return nullptr;
    auto it = cache_.find(path);
    if (it == cache_.end()) {
        std::optional<Json> merged;
        for (const std::string& folder : folders_) {
            for (const std::string& p : {path, buildVariant(path, build_)}) {
                const auto text = readText(folder + p);
                if (!text) continue;
                std::string why;
                auto j = Json::parse(*text, &why);
                if (!j) {
                    log::warn("Translation: {}{}: {}", folder, p, why);
                    continue;
                }
                if (merged) merged->merge(*j);
                else merged = std::move(*j);
            }
        }
        it = cache_.emplace(path, std::move(merged)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::string PatchStack::clean(std::string_view line, bool percentEscaped) {
    const std::size_t note = line.find('\x14');
    if (note != std::string_view::npos) line = line.substr(0, note);
    std::string s = cleanMarkup(line, 0);
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x12') continue;
        if (percentEscaped && s[i] == '%' && i + 1 < s.size() && s[i + 1] == '%') ++i;
        out.push_back(s[i]);
    }
    return out;
}

}  // namespace touhou
