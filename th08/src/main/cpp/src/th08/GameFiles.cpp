#include "th08/GameFiles.hpp"

#include <cstring>
#include <format>

#include "core/Sha256.hpp"
#include "io/Vfs.hpp"

namespace touhou::th08 {

bool isExe100d(std::span<const std::uint8_t> exe, std::string* why) {
    if (exe.size() != kExeSize) {
        if (why) *why = std::format("{} bytes, not {}", exe.size(), kExeSize);
        return false;
    }
    if (sha256Hex(exe) != kExeSha256) {
        if (why) *why = "another build (SHA-256 differs from 1.00d)";
        return false;
    }
    return true;
}

FolderReport inspectFolder(const Vfs& vfs) {
    FolderReport r;
    r.files = vfs.fileCount();
    r.archive = vfs.hasFormat("PBGZ");
    for (const auto exe : vfs.executables()) {
        if (isExe100d(exe)) {
            r.exe = true;
            r.exeBytes = exe;
            break;
        }
    }
    const std::span<const std::uint8_t> bgm = vfs.looseFile("thbgm.dat");
    r.thbgm = bgm.size() >= 4 && std::memcmp(bgm.data(), "ZWAV", 4) == 0;
    return r;
}

}  // namespace touhou::th08
