#pragma once

// Score, graze and extend rules shared by the subsystems (the original keeps these in its
// GameManager and Player).

#include <cstdint>

#include "th06/World.hpp"

namespace touhou::th06 {

// One graze (a bullet's or laser's graze box touched the player); the graze spark appears
// at `sparkPos`.
void addGraze(World& world, const Vec3f& sparkPos);

// Once per frame, before anything else: clamps the score, rolls the displayed score towards
// it and grants at most one score extend (10M, 20M, 40M, 60M, compared with the displayed
// score).
void rollScore(World& world);

// Bonus awarded when a stage is cleared (stage 1..7).
std::uint32_t stageClearBonus(const GameState& state, int stage, int startingLivesSetting = 2);
// A replay entering a stage: the extend counter moves past every threshold the displayed
// score has already reached (it is not stored in the file).
void skipPassedExtends(GameState& state);

}  // namespace touhou::th06
