#pragma once

// What the player's game folder holds for Imperishable Night: th08.dat (the PBGZ archive,
// mounted by the engine's Vfs), th08.exe 1.00d (recognised by its SHA-256, read for tables
// later) and thbgm.dat (the music, optional).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace touhou {
class Vfs;
}

namespace touhou::th08 {

// th08.exe 1.00d, the only version the port reads.
inline constexpr std::size_t kExeSize = 840704;
inline constexpr const char* kExeSha256 = "330fbdbf58a710829d65277b4f312cfbb38d5448b3df523e79350b879213d924";

bool isExe100d(std::span<const std::uint8_t> exe, std::string* why = nullptr);

struct FolderReport {
    std::size_t files = 0;      // files the Vfs can open (archive members and loose files)
    bool archive = false;       // th08.dat 1.00d mounted (its th08_0100d.ver has the 1.00d line)
    bool exe = false;           // th08.exe 1.00d found
    bool thbgm = false;         // thbgm.dat found (ZWAV header)
    std::span<const std::uint8_t> exeBytes;  // the 1.00d exe, when found
};

FolderReport inspectFolder(const Vfs& vfs);

}  // namespace touhou::th08
