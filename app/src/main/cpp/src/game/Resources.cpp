#include "game/Resources.hpp"

#include "game/EffectManager.hpp"

#include <algorithm>

namespace th06 {
namespace {

constexpr int kSoundGraze = 30;
constexpr int kSoundExtend = 28;
constexpr std::uint32_t kExtendScores[5] = {10'000'000, 20'000'000, 40'000'000, 60'000'000, 1'900'000'000};

std::int64_t roundDownTo10(std::int64_t v) { return v - v % 10; }

}  // namespace

void addGraze(World& world, const Vec3f& sparkPos) {
    GameState& gs = *world.state;
    if (!gs.bombActive) {
        gs.grazes = std::min<std::uint32_t>(gs.grazes + 1, 9999);
        gs.totalGrazes = std::min<std::uint32_t>(gs.totalGrazes + 1, 999999);
    }
    spawnEffect(world, EffectParticleSmall, sparkPos, 1);
    gs.score += 500;
    gs.increaseSubrank(6);
    world.playSound(kSoundGraze);
}

void rollScore(World& world) {
    GameState& gs = *world.state;
    if (gs.score >= 1'000'000'000u) gs.score = 999'999'990u;
    if (gs.displayedScore != gs.score) {
        if (gs.score < gs.displayedScore) gs.score = gs.displayedScore;
        std::uint32_t step = (gs.score - gs.displayedScore) >> 5;
        step = std::clamp<std::uint32_t>(step, 10, 78910);
        step -= step % 10;
        gs.scoreStep = std::max(gs.scoreStep, step);
        if (gs.displayedScore + gs.scoreStep > gs.score) gs.scoreStep = gs.score - gs.displayedScore;
        gs.displayedScore += gs.scoreStep;
        if (gs.displayedScore >= gs.score) {
            gs.scoreStep = 0;
            gs.displayedScore = gs.score;
        }
    }
    if (gs.extendIndex >= 0 && gs.extendIndex < 5 && gs.displayedScore >= kExtendScores[gs.extendIndex]) {
        if (gs.livesRemaining < GameState::kMaxLives) {
            ++gs.livesRemaining;
            world.playSound(kSoundExtend);
        }
        ++gs.extendIndex;
        gs.increaseSubrank(200);
    }
}

std::uint32_t stageClearBonus(const GameState& gs, int stage, int startingLivesSetting) {
    std::int64_t bonus = (static_cast<std::int64_t>(stage) * 1000 + static_cast<std::int64_t>(gs.grazes) * 10 +
                          static_cast<std::int64_t>(gs.power) * 100) *
                         static_cast<std::int64_t>(gs.stagePointItems);
    if (stage >= 6) bonus += static_cast<std::int64_t>(gs.livesRemaining) * 3'000'000 + static_cast<std::int64_t>(gs.bombs) * 1'000'000;
    switch (gs.difficulty) {
        case Easy: bonus = roundDownTo10(bonus / 2); break;
        case Hard: bonus = roundDownTo10(bonus * 12 / 10); break;
        case Lunatic: bonus = roundDownTo10(bonus * 15 / 10); break;
        case Extra: bonus = roundDownTo10(bonus * 2); break;
        default: break;
    }
    if (startingLivesSetting == 3) bonus = roundDownTo10(bonus * 5 / 10);
    else if (startingLivesSetting == 4) bonus = roundDownTo10(bonus * 2 / 10);
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(bonus, 0, 999'999'990));
}

}  // namespace th06
