#pragma once

// What the stage subsystems of a running Imperishable Night game share, passed to each of
// them instead of the original's globals: the game state, the RNG and ANM library, and
// pointers to the other subsystems (null while a system is not ported or not loaded).

#include <cstdint>

#include "core/ZunMath.hpp"

namespace touhou::th08 {

class AnmLibrary;
class Background;
class BulletManager;
class EnemyManager;
class Effects;
class Player;
class ScreenEffects;
class SoundPlayer;
struct ExeTables;
struct GameState;

inline constexpr float kPlayfieldX = 32.0f, kPlayfieldY = 16.0f;
inline constexpr float kPlayfieldW = 384.0f, kPlayfieldH = 448.0f;

// GameManager mode flags (GM+0x3DBAC) used by the stage systems.
enum ModeFlag : std::uint32_t {
    ModePractice = 1u << 0,
    ModeReplay = 1u << 3,
    ModeDissolve = 3u << 7,     // bits 7-8: ECL opcode 176
    ModeSpellFreeze = 1u << 10, // a hit during a spell card (everything but the stage waits)
    ModeFinalB = 3u << 11,      // bits 11-12: the Final route chosen in the Stage 5 dialogue
    ModeBit13 = 1u << 13,       // stops the doll laser (meaning not traced)
    ModeSpellPractice = 1u << 14,
};

struct World {
    GameState* state = nullptr;
    ZunRng* rng = nullptr;
    AnmLibrary* anm = nullptr;
    const ExeTables* tables = nullptr;
    SoundPlayer* sound = nullptr;
    Effects* effects = nullptr;
    Player* player = nullptr;
    Background* background = nullptr;
    BulletManager* bullets = nullptr;
    EnemyManager* enemies = nullptr;
    ScreenEffects* screen = nullptr;

    // Game-wide flags any object may change (the objects get the world as const).
    mutable std::uint32_t mode = 0;  // ModeFlag bits
    std::int32_t stageFrame = 0;     // GM+0x3DDC0
    bool dialogue = false;           // a dialogue script is running
    bool spellCardActive = false;    // Spellcard flag bit 0
    mutable bool timeStop = false;   // GM+0x2C (Sakuya's time stop)

    void playSound(int id) const;
    // AddTimePoints (0x418220, spec_player_bullets_items 12.1): a loss larger than what is
    // left zeroes the points without a draw; anything else adds, regenerates the anti-tamper
    // values (4 steps) and, for a gain, raises the maximum point value.
    void addTimePoints(std::int32_t n) const;
};

// The anti-tamper regeneration (spec_player 18.1 "R"): two u32 draws (4 steps) whenever a
// guarded value (lives, bombs, power, misses, bombs used, time points) changes.
inline void antiTamper(ZunRng& rng) {
    (void)rng.u32();
    (void)rng.u32();
}

}  // namespace touhou::th08
