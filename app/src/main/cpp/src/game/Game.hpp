#pragma once

#include <cstdint>
#include <memory>

#include "core/Math.hpp"
#include "core/ObjectPool.hpp"
#include "game/BulletManager.hpp"
#include "game/GameArt.hpp"
#include "game/Input.hpp"
#include "game/Sound.hpp"

namespace th06 {

struct HudInfo {
    float fps = 0;
    std::uint32_t drawCalls = 0;
};

// Fixed-timestep simulation: one tick() = one 1/60 s frame of the original game.
// This is the engine's test stage (player movement and shot, bombs, graze/hit, and four
// bullet patterns that peak at several thousand live bullets), not the original stages:
// running those needs the ECL/STD/ANM script interpreters, which build on this layer.
class Game {
  public:
    struct Stats {
        std::uint64_t ticks = 0;
        std::size_t bullets = 0;
        std::size_t peakBullets = 0;
        std::uint64_t dropped = 0;
        std::uint32_t grazes = 0;
        std::uint32_t hits = 0;
        std::uint32_t bombs = 0;
    };

    Game(GameArt art, SoundSink* sound);

    void tick(const InputState& input);
    void render(SpriteSink& sink, const HudInfo& hud) const;

    const Stats& stats() const { return stats_; }
    bool paused() const { return paused_; }
    void setPaused(bool p) { paused_ = p; }
    const char* patternName() const;

  private:
    struct Shot {
        Vec2 pos;
        Vec2 vel;
    };

    void updatePlayer(const InputState& in);
    void firePatterns();
    void play(Sfx sfx, int minGapTicks);

    GameArt art_;
    SoundSink* sound_;
    BulletManager bullets_;
    ObjectPool<Shot, 512> shots_;
    Rng rng_;

    Vec2 player_{kFieldWidth * 0.5f, kFieldHeight - 64.0f};
    Vec2 spawner_{kFieldWidth * 0.5f, 96.0f};
    bool focused_ = false;
    bool paused_ = false;
    InputState previous_;
    int invulnerable_ = 0;
    int bombCooldown_ = 0;
    float spin_ = 0;
    std::uint64_t lastPlayed_[static_cast<std::size_t>(Sfx::Count)] = {};
    Stats stats_;
};

}  // namespace th06
