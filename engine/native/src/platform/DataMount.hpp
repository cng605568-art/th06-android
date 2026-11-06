#pragma once

#include <memory>

#include "io/Vfs.hpp"

namespace touhou {

// Mounts the player's own game archives.
//   Android: the archives chosen in the launcher (opened through the Storage Access
//            Framework as file descriptors and memory-mapped in place), then any archive
//            placed in  Android/data/<package>/files/gamedata/  (handy with adb push).
//   Desktop: the directory given as the first argument, or $<envPrefix>_DATA (th06:
//            $TH06_DATA), or ./gamedata; $<envPrefix>_PREFER_NC prefers the New Classic
//            archives where both editions are present.
// Returns an empty Vfs when nothing is found; the game then runs its demo stand-ins.
std::unique_ptr<Vfs> mountGameData(int argc, char** argv, const char* envPrefix);

}  // namespace touhou
