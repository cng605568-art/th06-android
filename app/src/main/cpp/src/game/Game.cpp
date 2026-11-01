#include "game/Game.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "gfx/ProceduralArt.hpp"

namespace th06 {
namespace {

constexpr int kPhaseTicks = 600;  // each pattern runs 10 s
constexpr float kSpeedNormal = 4.0f;
constexpr float kSpeedFocused = 2.0f;
constexpr float kDiagonal = 0.70710678f;
constexpr float kHitRadius = 1.5f;
constexpr float kGrazeRadius = 16.0f;
constexpr int kInvulnerableAfterHit = 180;
constexpr int kBombDuration = 150;

constexpr const char* kPatternNames[] = {"SPIRAL", "RINGS + AIMED", "FLOWER", "DENSITY TEST"};

}  // namespace

Game::Game(GameArt art, SoundSink* sound) : art_(std::move(art)), sound_(sound) {}

const char* Game::patternName() const {
    return kPatternNames[(stats_.ticks / kPhaseTicks) % 4];
}

void Game::play(Sfx sfx, int minGapTicks) {
    auto& last = lastPlayed_[static_cast<std::size_t>(sfx)];
    if (!sound_ || (last != 0 && stats_.ticks - last < static_cast<std::uint64_t>(minGapTicks))) return;
    last = stats_.ticks;
    sound_->play(sfx);
}

void Game::tick(const InputState& in) {
    if (in.pause && !previous_.pause) paused_ = !paused_;
    if (paused_) {
        previous_ = in;
        return;
    }
    ++stats_.ticks;

    updatePlayer(in);

    if (in.bomb && !previous_.bomb && bombCooldown_ == 0) {
        bullets_.clear();
        invulnerable_ = std::max(invulnerable_, kBombDuration);
        bombCooldown_ = kBombDuration;
        ++stats_.bombs;
        play(Sfx::Bomb, 0);
    }

    firePatterns();

    const auto result = bullets_.tick(player_, kHitRadius, kGrazeRadius, invulnerable_ > 0);
    if (result.grazes) {
        stats_.grazes += static_cast<std::uint32_t>(result.grazes);
        play(Sfx::Graze, 4);
    }
    if (result.hit) {
        ++stats_.hits;
        play(Sfx::Hit, 0);
        bullets_.clear();
        invulnerable_ = kInvulnerableAfterHit;
        player_ = {kFieldWidth * 0.5f, kFieldHeight - 64.0f};
    }

    if (invulnerable_ > 0) --invulnerable_;
    if (bombCooldown_ > 0) --bombCooldown_;
    stats_.bullets = bullets_.size();
    stats_.peakBullets = std::max(stats_.peakBullets, stats_.bullets);
    stats_.dropped = bullets_.dropped();
    previous_ = in;
}

void Game::updatePlayer(const InputState& in) {
    focused_ = in.focus;
    float dx = static_cast<float>(in.right) - static_cast<float>(in.left);
    float dy = static_cast<float>(in.down) - static_cast<float>(in.up);
    if (dx != 0 && dy != 0) {
        dx *= kDiagonal;
        dy *= kDiagonal;
    }
    const float speed = focused_ ? kSpeedFocused : kSpeedNormal;
    player_.x = std::clamp(player_.x + dx * speed, 8.0f, kFieldWidth - 8.0f);
    player_.y = std::clamp(player_.y + dy * speed, 16.0f, kFieldHeight - 16.0f);

    if (in.shoot && stats_.ticks % 4 == 0) {
        const float off = focused_ ? 6.0f : 10.0f;
        shots_.spawn(Shot{{player_.x - off, player_.y - 12}, {0, -14}});
        shots_.spawn(Shot{{player_.x + off, player_.y - 12}, {0, -14}});
        if (!focused_) {
            shots_.spawn(Shot{{player_.x - 4, player_.y - 8}, Vec2::fromAngle(-kPi / 2 - 0.18f, 13)});
            shots_.spawn(Shot{{player_.x + 4, player_.y - 8}, Vec2::fromAngle(-kPi / 2 + 0.18f, 13)});
        }
        play(Sfx::Shot, 8);
    }
    shots_.removeIf([](Shot& s) {
        s.pos += s.vel;
        return s.pos.y < -24 || s.pos.x < -24 || s.pos.x > kFieldWidth + 24;
    });
}

void Game::firePatterns() {
    const auto t = static_cast<int>(stats_.ticks % kPhaseTicks);
    const auto phase = (stats_.ticks / kPhaseTicks) % 4;
    const float time = static_cast<float>(stats_.ticks);
    spawner_ = {kFieldWidth * 0.5f + 110.0f * std::sin(time * 0.011f),
                100.0f + 24.0f * std::sin(time * 0.023f)};
    if (t < 30) return;  // short breather at every pattern change

    const auto palette = static_cast<std::uint16_t>(art_.bullets.size());
    const auto pick = [&](int i) { return static_cast<std::uint16_t>(i % std::max<int>(1, palette)); };
    const float aim = (player_ - spawner_).angle();
    bool fired = false;

    switch (phase) {
        case 0:  // six-arm spiral
            if (t % 3 == 0) {
                for (int arm = 0; arm < 6; ++arm) {
                    bullets_.spawn(spawner_, spin_ + arm * (kTau / 6), 2.2f, pick(arm));
                }
                spin_ += 0.17f;
                fired = true;
            }
            break;
        case 1:  // rings + aimed fans
            if (t % 24 == 0) {
                bullets_.ring(spawner_, 36, rng_.range(0, kTau), 1.8f, pick(1));
                fired = true;
            }
            if (t % 45 == 0) {
                bullets_.fan(spawner_, 7, aim, 0.6f, 3.2f, pick(4));
                fired = true;
            }
            break;
        case 2:  // counter-rotating accelerating flower
            if (t % 5 == 0) {
                for (int arm = 0; arm < 10; ++arm) {
                    const float a = spin_ + arm * (kTau / 10);
                    bullets_.spawn(spawner_, a, 0.6f, pick(2), 0.025f, 2.6f, 0.012f);
                    bullets_.spawn(spawner_, -a, 0.6f, pick(5), 0.025f, 2.6f, -0.012f);
                }
                spin_ += 0.09f;
                fired = true;
            }
            break;
        default:  // density test: 96-bullet rings every 5 ticks (thousands live at once)
            if (t % 5 == 0) {
                const float speed = (t / 5) % 2 ? 1.4f : 2.1f;
                bullets_.ring(spawner_, 96, time * 0.05f, speed, pick(t / 5));
                fired = true;
            }
            break;
    }
    if (fired) play(Sfx::Fire, 8);
}

void Game::render(SpriteSink& s, const HudInfo& hud) const {
    const UiArt& ui = art_.ui;
    const float time = static_cast<float>(stats_.ticks);

    // Playfield and a slow star scroll so motion is visible even without game data.
    s.fillRect(ui.white, kFieldX, kFieldY, kFieldWidth, kFieldHeight, rgba(10, 8, 26));
    for (int i = 0; i < 48; ++i) {
        const auto h = static_cast<std::uint32_t>(i) * 2654435761u;
        const float speed = 0.3f + static_cast<float>(h >> 28) * 0.08f;
        const float x = kFieldX + static_cast<float>(h % 384);
        const float y = kFieldY + std::fmod(static_cast<float>((h >> 9) % 448) + time * speed, kFieldHeight);
        s.drawRegion(ui.glow, x, y, 0.06f + speed * 0.04f, rgba(150, 140, 255, 140), 0, Blend::Additive);
    }

    // Spawner (stand-in for an enemy/boss).
    const float sx = kFieldX + spawner_.x, sy = kFieldY + spawner_.y;
    s.drawRegion(ui.glow, sx, sy, 1.1f, rgba(255, 60, 120, 90), 0, Blend::Additive);
    s.drawRegion(art_.spawner, sx, sy, 1.0f, rgba(255, 120, 170), time * 0.03f, Blend::Additive);

    bullets_.render(s, art_.bullets);

    for (const Shot& shot : shots_) {
        s.drawRegion(art_.shot, kFieldX + shot.pos.x, kFieldY + shot.pos.y, 1.0f,
                     rgba(255, 255, 255, 200), shot.vel.angle() + kPi * 0.5f, Blend::Additive);
    }

    const bool blink = invulnerable_ > 0 && (stats_.ticks / 3) % 2 == 0;
    const float px = kFieldX + player_.x, py = kFieldY + player_.y;
    if (!blink) s.drawRegion(art_.player, px, py);
    if (focused_) {
        s.drawRegion(ui.ring, px, py, 0.55f, rgba(255, 80, 110, 220), time * 0.05f);
        s.drawRegion(ui.disc, px, py, 0.09f, rgba(255, 255, 255));
    }

    // Frame drawn over the field edges (doubles as clipping), then the HUD.
    const auto frame = rgba(46, 18, 40);
    s.fillRect(ui.white, 0, 0, kScreenWidth, kFieldY, frame);
    s.fillRect(ui.white, 0, kFieldY + kFieldHeight, kScreenWidth, kScreenHeight - kFieldY - kFieldHeight, frame);
    s.fillRect(ui.white, 0, kFieldY, kFieldX, kFieldHeight, frame);
    s.fillRect(ui.white, kFieldX + kFieldWidth, kFieldY, kScreenWidth - kFieldX - kFieldWidth, kFieldHeight, frame);

    const auto label = rgba(255, 170, 200);
    const auto value = rgba(255, 255, 255);
    float y = 40;
    const float x = 440;
    auto line = [&](std::string_view name, const std::string& v) {
        drawText(s, ui, x, y, name, 1.0f, label);
        drawText(s, ui, x + 84, y, v, 1.0f, value);
        y += 14;
    };
    drawText(s, ui, x, y, "TH06 PORT", 2.0f, value);
    y += 28;
    drawText(s, ui, x, y, art_.dataLabel, 1.0f, rgba(170, 220, 255));
    y += 22;
    line("PATTERN", patternName());
    line("BULLETS", std::format("{}", stats_.bullets));
    line("PEAK", std::format("{} / {}", stats_.peakBullets, BulletManager::kCapacity));
    line("GRAZE", std::format("{}", stats_.grazes));
    line("HITS", std::format("{}", stats_.hits));
    line("BOMBS", std::format("{}", stats_.bombs));
    y += 8;
    line("FPS", std::format("{:.1f}", hud.fps));
    line("DRAWS", std::format("{}", hud.drawCalls));
    y += 8;
    // Why an asset fell back to generated art, word-wrapped to the panel (29 chars, 3 lines).
    auto note = [&](std::string_view text) {
        constexpr std::size_t kWidth = 29;
        const auto warn = rgba(255, 210, 120);
        for (int lines = 0; !text.empty() && lines < 3; ++lines) {
            std::size_t cut = text.size();
            if (cut > kWidth) {
                cut = text.rfind(' ', kWidth);
                if (cut == std::string_view::npos || cut == 0) cut = kWidth;
            }
            drawText(s, ui, x + 6, y, text.substr(0, cut), 1.0f, warn);
            y += 10;
            text.remove_prefix(cut);
            while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        }
    };
    drawText(s, ui, x, y, art_.bulletsFromGameData ? "BULLETS: ETAMA3.ANM" : "BULLETS: GENERATED", 1.0f, label);
    y += 12;
    note(art_.bulletsNote);
    drawText(s, ui, x, y, art_.playerFromGameData ? "PLAYER: PLAYER00.ANM" : "PLAYER: GENERATED", 1.0f, label);
    y += 12;
    note(art_.playerNote);
#if !defined(__ANDROID__)
    drawText(s, ui, x, 440, "KEYS: ARROWS Z X SHIFT ESC", 1.0f, rgba(140, 120, 150));
#endif

    if (paused_) {
        s.fillRect(ui.white, kFieldX, kFieldY, kFieldWidth, kFieldHeight, rgba(0, 0, 0, 150));
        const char* title = "PAUSED";
        drawText(s, ui, kFieldX + (kFieldWidth - textWidth(title, 3)) * 0.5f, 200, title, 3.0f, value);
        const char* hint = "PAUSE TO RESUME - BACK TO QUIT";
        drawText(s, ui, kFieldX + (kFieldWidth - textWidth(hint, 1)) * 0.5f, 240, hint, 1.0f, label);
    }
}

}  // namespace th06
