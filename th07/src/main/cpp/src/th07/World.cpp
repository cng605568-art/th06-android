#include "th07/World.hpp"

#include <algorithm>

#include "th07/Player.hpp"

namespace touhou::th07 {

void GameState::setDifficulty(int d) {
    static constexpr std::int32_t kRank[6][3] = {{16, 12, 20}, {16, 10, 32}, {16, 10, 32}, {16, 10, 32}, {16, 15, 16}, {16, 15, 16}};
    difficulty = std::clamp(d, 0, 5);
    rank = kRank[difficulty][0];
    minRank = kRank[difficulty][1];
    maxRank = kRank[difficulty][2];
    subRank = 0;
}

void GameState::increaseSubrank(std::int32_t amount) {
    subRank += amount;
    while (subRank >= 100) {
        ++rank;
        subRank -= 100;
    }
    if (rank > maxRank) rank = maxRank;
}

void GameState::decreaseSubrank(std::int32_t amount) {
    subRank -= amount;
    while (subRank < 0) {
        --rank;
        subRank += 100;
    }
    if (rank < minRank) rank = minRank;
}

std::int32_t GameState::extendThreshold() const {
    const int n = extendsGranted;
    if (difficulty >= Extra) {
        if (n == 0) return 200;
        if (n == 1) return 500;
        return 800 + 500 * (n - 2);
    }
    if (n < 3) return 50 + 75 * n;
    if (n < 5) return 300 + 150 * (n - 3);
    return 800 + 200 * (n - 5);
}

void GameState::rollScore() {
    if (score > 999999999u) score = 999999999u;
    if (guiScore == score) return;
    if (score < guiScore) score = guiScore;
    std::uint32_t step = (score - guiScore) >> 5;
    step = std::clamp<std::uint32_t>(step, 1u, 578910u);
    if (rollSpeed < step) rollSpeed = step;
    if (guiScore + rollSpeed > score) rollSpeed = score - guiScore;
    guiScore += rollSpeed;
    if (guiScore >= score) {
        rollSpeed = 0;
        guiScore = score;
    }
    if (guiScore > hiScore) {
        hiScore = guiScore;
        hiScoreContinues = continues;
    }
}

// ---- the anti-tamper bookkeeping draws ------------------------------------------------------

void World::regenerate() const {
    rng->u32InRange(100000);
    rng->u32InRange(100000);
}

void World::reroll() const {
    for (int i = 0; i < 5; ++i) rng->u32InRange(100000);
    for (int i = 0; i < 3; ++i) rng->f32ZeroToOne();
}

void World::setLives(float v) const {
    state->lives = v;
    regenerate();
}
void World::addLives(float d) const {
    state->lives += d;
    regenerate();
}
void World::setPower(float v) const {
    state->power = v;
    regenerate();
}
void World::addPower(float d) const {
    state->power += d;
    regenerate();
}
void World::addBombs(float d) const {
    state->bombs += d;
    regenerate();
}
void World::addMisses(float d) const {
    state->misses += d;
    regenerate();
}
void World::addBombsUsed(float d) const {
    state->bombsUsed += d;
    regenerate();
}
void World::setMisses(float v) const {
    state->misses = v;
    regenerate();
}
void World::setBombsUsed(float v) const {
    state->bombsUsed = v;
    regenerate();
}

// ---- cherry ---------------------------------------------------------------------------------

void World::cherryAddWithGauge(std::int32_t a) const {
    GameState& g = *state;
    const std::int32_t before = g.cherry;
    g.cherry = std::min(g.cherry + a, g.cherryMax);
    if (a > 0 && (!player || player->borderFlag() == Player::BorderFlag::None)) {
        g.cherryPlus += a;
        if (g.cherryPlus >= GameState::kCherryPlusFull) {
            g.cherryPlus = GameState::kCherryPlusFull;
            if (player) player->requestBorder(*this);
        }
    }
    if (g.cherry >= g.cherryMax && g.cherry != before && popups) popups->status(PopupSink::CherryMax, 0);
}

void World::cherryAdd(std::int32_t a) const {
    GameState& g = *state;
    const std::int32_t before = g.cherry;
    g.cherry = std::min(g.cherry + a, g.cherryMax);
    if (g.cherry >= g.cherryMax && g.cherry != before && popups) popups->status(PopupSink::CherryMax, 0);
}

void World::cherryRaise(std::int32_t a) const { state->cherry = std::min(state->cherry + a, state->cherryMax); }

void World::cherryRaiseMax(std::int32_t a) const { state->cherryMax = std::min(state->cherryMax + a, GameState::kCherryMaxCap); }

// ---- extends --------------------------------------------------------------------------------

bool World::grantExtend() const {
    GameState& g = *state;
    if (g.livesI() < GameState::kMaxLives) addLives(1.0f);
    else if (g.bombsI() < GameState::kMaxBombs) addBombs(1.0f);
    else return false;
    playSound(28);
    g.increaseSubrank(200);
    return true;
}

void World::checkPointExtends() const {
    GameState& g = *state;
    while (g.extendsGranted >= 0) {
        g.nextExtend = g.extendThreshold();
        if (static_cast<std::int32_t>(g.extendPoints) < g.nextExtend) break;
        grantExtend();
        ++g.extendsGranted;
    }
}

}  // namespace touhou::th07
