#pragma once

#include <cstddef>
#include <cstdint>

namespace th06 {

enum class Sfx : std::uint8_t { Shot, Fire, Graze, Hit, Bomb, Count };

// Sound effect files inside the 2002 archives (names from the original engine's table,
// stored under data/wav/). Index matches Sfx.
inline constexpr const char* kSfxFiles[static_cast<std::size_t>(Sfx::Count)] = {
    "plst00.wav",    // player shot
    "tan00.wav",     // enemy bullet fired
    "graze.wav",     // graze
    "pldead00.wav",  // player hit
    "cat00.wav",     // bomb
};

class SoundSink {
  public:
    virtual ~SoundSink() = default;
    virtual void play(Sfx sfx) = 0;
};

}  // namespace th06
