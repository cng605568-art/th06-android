#include "th08/Version.hpp"

#include <charconv>

namespace touhou::th08 {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// A decimal number taken modulo 2^32 (checksums above 2^31 are written unsigned).
std::optional<std::int32_t> readInt(std::string_view& s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    std::int64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{}) return std::nullopt;
    s.remove_prefix(static_cast<std::size_t>(end - s.data()));
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
}

}  // namespace

std::vector<ExeIdentity> findVersionLines(std::span<const std::uint8_t> verFile, std::string_view tag) {
    std::vector<ExeIdentity> out;
    const std::string_view text(reinterpret_cast<const char*>(verFile.data()), verFile.size());
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t nl = text.find('\n', at);
        std::string_view line = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? text.size() : nl + 1;
        if (line.size() < tag.size() + 1 || line.substr(0, tag.size()) != tag) continue;
        line.remove_prefix(tag.size() + 1);  // the tag and its separator
        const auto size = readInt(line);
        const auto checksum = size ? readInt(line) : std::nullopt;
        if (size && checksum) out.push_back({*size, *checksum});
    }
    return out;
}

bool versionMatches(std::span<const std::uint8_t> verFile, std::string_view tag, const ExeIdentity& id) {
    for (const ExeIdentity& line : findVersionLines(verFile, tag)) {
        if (line == id) return true;
    }
    return false;
}

std::int32_t exeChecksum(std::span<const std::uint8_t> exe) {
    const std::size_t words = exe.size() / 4;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < words; ++i) {
        sum += static_cast<std::uint32_t>(exe[i * 4]) | static_cast<std::uint32_t>(exe[i * 4 + 1]) << 8 |
               static_cast<std::uint32_t>(exe[i * 4 + 2]) << 16 | static_cast<std::uint32_t>(exe[i * 4 + 3]) << 24;
    }
    return static_cast<std::int32_t>(sum);
}

}  // namespace touhou::th08
