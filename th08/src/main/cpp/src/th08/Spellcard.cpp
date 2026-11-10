#include "th08/Spellcard.hpp"

#include <algorithm>
#include <cstring>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "th08/Background.hpp"
#include "th08/Bullets.hpp"
#include "th08/Ecl.hpp"
#include "th08/Effects.hpp"
#include "th08/Enemies.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"
#include "th08/Msg.hpp"
#include "th08/Player.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {

std::uint16_t rdU16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::int16_t rdI16(const std::uint8_t* p) { return static_cast<std::int16_t>(rdU16(p)); }
std::uint32_t rdU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// The Last-Spell state: GameManager mode bits 7-8 read as a number.
std::uint32_t lastSpellState(const World& w) { return (w.mode >> 7) & 3u; }
void setLastSpellState(const World& w, std::uint32_t s) { w.mode = (w.mode & ~static_cast<std::uint32_t>(ModeDissolve)) | ((s & 3u) << 7); }

// A channel set-up (spec 0.3): progress 0, the duration assigned, the ease mode.
void channel(AnmVm& vm, int c, std::int32_t duration, std::uint8_t ease) {
    vm.progress[static_cast<std::size_t>(c)].assign(0);
    vm.duration[static_cast<std::size_t>(c)].assign(duration);
    vm.ease[static_cast<std::size_t>(c)] = ease;
}

constexpr float kPi = 3.14159265358979f;

}  // namespace

void Spellcard::startVm(const World& w, AnmVm& vm, int slot, int script) {
    if (!ctx_.anm || !w.rng) return;
    vm.start(*ctx_.anm, *w.rng, slot, script);
}

// ---- the stage start (spec 1.1-1.2) -------------------------------------------------------

void Spellcard::stageStart(const World& w, const Context& ctx) {
    ctx_ = ctx;
    flags_ = 0;
    boss_ = nullptr;
    id_ = bossSlot_ = 0;
    pendingTime_ = 0;
    name_.clear();
    comment1_.fill(0);
    comment2_.fill(0);
    circle_ = endEffect_ = nullptr;
    capture_ = drain_ = awarded_ = cap_ = 0;
    timer_ = Timer{0, 0.0f, 0};
    duration_ = 0;
    boxAlpha_ = 0;
    for (AnmVm* vm : {&playerFace_, &bossFace_, &decorA_, &decorB_, &bombName_, &cardName_, &ghost1_, &ghost2_, &bombBox_, &cardBox_,
                      &digit_, &indicator_}) {
        *vm = AnmVm{};
    }
    loaded_ = ctx_.anm != nullptr;
    if (!loaded_) return;
    if (ctx_.anm->loaded(Dialogue::kSlotFaceHuman)) {
        startVm(w, playerFace_, Dialogue::kSlotFaceHuman, 0);
        startVm(w, bossFace_, Dialogue::kSlotFaceHuman, 0);
    }
    startVm(w, bombName_, kSlotText, 4);
    startVm(w, cardName_, kSlotText, 5);
    startVm(w, bombBox_, kSlotAscii, 1);
    startVm(w, cardBox_, kSlotAscii, 0);
    startVm(w, indicator_, kSlotAscii, 2);
    startVm(w, digit_, kSlotAscii, 4);
    for (AnmVm* vm : {&playerFace_, &bossFace_, &decorA_, &decorB_, &bombName_, &cardName_}) {
        vm->current = nullptr;
        vm->set(AnmVm::Visible, false);
    }
    bombName_.fontWidth = bombName_.fontHeight = 15;
    cardName_.fontWidth = cardName_.fontHeight = 15;
}

// ---- Begin (spec 2) ---------------------------------------------------------------------------

void Spellcard::begin(const World& w, Enemy& boss, const std::uint8_t* ins) {
    GameState* g = w.state;
    // 1-2.
    flags_ &= ~static_cast<std::uint32_t>(LastCaptured | Bit4 | TimedOut | CircleFrozen | Bombed | NameOnly);
    flags_ |= Active | Capturable;
    id_ = rdU16(ins + 0x0E);
    boss_ = &boss;
    character_ = g ? g->character : 0;
    practice_ = (w.mode & ModeSpellPractice) != 0;
    bossSlot_ = boss.slot;
    // 3-5. The value, the cap, the drain per second (unsigned; a timer under 60 frames would
    // divide by zero in the original).
    const std::uint32_t s = rdU32(ins + 0x10);
    capture_ = cap_ = s;
    if (boss.survival) cap_ = kSurvivalCap;
    const std::int32_t thr = boss.timerThreshold;
    const std::int32_t seconds = thr / 60;
    if (seconds == 0) {
        log::warn("th08: spell card {} with a timer of {} frames: no drain", id_, thr);
        drain_ = 0;
    } else {
        drain_ = (s - s / 7u) / static_cast<std::uint32_t>(seconds);
    }
    timer_.assign(thr);
    duration_ = thr;
    // 6. The name (48 bytes XOR 0xAA).
    name_.clear();
    for (int i = 0; i < 48; ++i) {
        const auto c = static_cast<char>(ins[0x14 + i] ^ 0xAA);
        if (c == 0) break;
        name_.push_back(c);
    }
    std::memcpy(comment1_.data(), ins + 0x74, comment1_.size());
    std::memcpy(comment2_.data(), ins + 0xB4, comment2_.size());
    // 7-11.
    nameStart(w, rdI16(ins + 0x0C), name_, 0);
    if (w.bullets) w.bullets->clearAll(w, 1);
    if (w.background && w.anm && w.rng) w.background->spellStart(*w.anm, *w.rng);
    boss.rankSpeedLow = -0.5f;
    boss.rankSpeedHigh = 0.5f;
    boss.rankAmounts.fill(0);
    boxAlpha_ = 0x80;
    // 12. A capture bonus still pending is paid now, without the extra 10 x P [QUIRK].
    if (has(Pending)) {
        endEffect_ = nullptr;
        if (w.gui) w.gui->messageC(static_cast<std::int32_t>(awarded_));
        if (g) g->score += awarded_ / 10u;
        setFlag(Pending, false);
        if (pendingTime_ > 0) {
            w.addTimePoints(pendingTime_);
            pendingTime_ = 0;
        }
    }
    // 13-15. The circle: special slot 1, at the boss's base position.
    setFlag(NoDrain, false);
    const std::uint32_t ls = lastSpellState(w);
    if (w.effects) {
        Effect& c = w.effects->spawnSpecial(w, ls != 0 ? 52 : 39, boss.pos, 1, 1, 0xFFFFFFFFu);
        if (!w.effects->isSink(c)) {
            circle_ = &c;
            channel(c.vm, AnmVm::Position, 100, 6);
            c.vm.posStart = {8.0f, 64.0f, c.vm.posStart.z};
            c.vm.posEnd = {256.0f, 0.0f, c.vm.posEnd.z};
            c.vm.position.y = 64.0f;
            c.pos = boss.pos;
            c.segments = 64;
            c.angle = 0.0f;
            c.radius = 256.0f;
            c.width = 15.0f;
            c.waves = 6.0f;
        }
    }
    setFlag(LastSpellLook, (ls & 1u) != 0);
    // 16-17.
    if (w.gui) w.gui->refreshLivesBombs();
    setFlag(CircleFrozen, false);
    // 18. The card's attempts (not in a replay).
    if (ctx_.cards && g && !(w.mode & ModeReplay) && id_ >= 0 && id_ < kCardCount) {
        CardRecord& r = (*ctx_.cards)[static_cast<std::size_t>(id_)];
        r.present = true;
        auto bump = [](std::uint32_t& v) {
            if (v < 9999) ++v;
        };
        const auto ch = static_cast<std::size_t>(std::clamp(g->character, 0, 11));
        if (w.mode & ModeSpellPractice) {
            bump(r.practiceAttempts[ch]);
            bump(r.practiceAttempts[12]);
        } else {
            bump(r.attempts[ch]);
            bump(r.attempts[12]);
        }
    }
}

// The card name and portrait (0x415F00, spec 2.4).
void Spellcard::nameStart(const World& w, int portrait, std::string_view sjis, int decor) {
    if (portrait >= 0) {
        startVm(w, bossFace_, Dialogue::kSlotFaceBoss, 0);
        bossFace_.selectSprite(portrait);
    }
    startVm(w, decorA_, Dialogue::kSlotDecor, 1);
    startVm(w, decorB_, Dialogue::kSlotDecor, 3);
    decorA_.selectSprite(decor);
    decorB_.selectSprite(decor);
    constexpr std::uint32_t kCardColor = 0xFFF0F0u;  // GDI order
    if ((w.mode & ModeSpellPractice) && id_ >= 205 && id_ <= 221) {
        startVm(w, cardName_, kSlotText, 6);
        startVm(w, ghost1_, kSlotText, 7);
        startVm(w, ghost2_, kSlotText, 8);
        renderSpriteText(ctx_.textures, ctx_.text, ghost1_, sjis, kCardColor, TextAlign::Right);
        renderSpriteText(ctx_.textures, ctx_.text, ghost2_, sjis, kCardColor, TextAlign::Right);
    } else {
        startVm(w, cardName_, kSlotText, 5);
    }
    renderSpriteText(ctx_.textures, ctx_.text, cardName_, sjis, kCardColor, TextAlign::Right);
    cardBox_.interrupt(1);
    if (!has(NameOnly)) indicator_.interrupt(1);
    w.playSound(14);
}

void Spellcard::hideName() {
    cardName_.interrupt(1);
    cardBox_.interrupt(2);
    if (!has(NameOnly)) indicator_.interrupt(2);
}

void Spellcard::close() {
    setFlag(Active, false);
    hideName();
}

void Spellcard::fakeName(const World& w, std::string_view sjis) {
    setFlag(NameOnly, true);
    setFlag(Active, false);
    setFlag(Bit4, false);
    nameStart(w, -1, sjis, 1);
}

// ---- the bomb banner (spec 2.5-2.6) ---------------------------------------------------------

void Spellcard::bombBanner(const World& w, int side, std::string_view sjis, int decor) {
    const int slot = side == 1 ? Dialogue::kSlotFaceYoukai : Dialogue::kSlotFaceHuman;
    if ((side == 0 || side == 1) && ctx_.anm && ctx_.anm->loaded(slot)) {  // (side -1: no portrait)
        startVm(w, playerFace_, slot, 0);
        playerFace_.selectSprite(0);
    }
    startVm(w, decorA_, Dialogue::kSlotDecor, 0);
    startVm(w, decorB_, Dialogue::kSlotDecor, 2);
    decorA_.selectSprite(decor);
    decorB_.selectSprite(decor);
    startVm(w, bombName_, kSlotText, 4);
    renderSpriteText(ctx_.textures, ctx_.text, bombName_, sjis, 0xF0F0FFu, TextAlign::Left);
    bombBox_.interrupt(1);
    w.playSound(14);
}

void Spellcard::bombBannerOff() {
    bombName_.interrupt(1);
    bombBox_.interrupt(2);
}

// ---- the failures (spec 4.1) ------------------------------------------------------------------

void Spellcard::timeout() {
    setFlag(Capturable, false);
    setFlag(TimedOut, true);
    capture_ = 0;
}

void Spellcard::bombFailure() {
    setFlag(Capturable, false);
    capture_ = 0;
    setFlag(Bombed, has(Active));
}

void Spellcard::deathFailure() {
    setFlag(Capturable, false);
    capture_ = 0;
}

void Spellcard::orbBonus(std::uint32_t v) {
    if (!has(NoDrain)) capture_ += v;
    if (capture_ >= cap_) capture_ = cap_;
    else drain_ += v / 120u;
}

// ---- End (spec 6) -----------------------------------------------------------------------------

void Spellcard::end(const World& w) {
    GameState* g = w.state;
    if (has(Active)) {
        bool captured = false;
        setFlag(Active, false);
        hideName();
        // 2. The bonus clear and the kill, unless timed out.
        if (!has(TimedOut) && w.bullets) {
            std::uint32_t s = w.bullets->bonusClear(w, 8000);
            if (w.enemies) s = w.enemies->killAllScoring(8000, s);
            if (s != 0) {
                if (g) g->score += s / 10u;
                if (w.gui) w.gui->messageA(static_cast<std::int32_t>(s));
            }
        }
        // 3. The capture.
        if (has(Capturable)) {
            awarded_ = capture_;
            if (boss_ && boss_->survival) {
                pendingTime_ = 700;
            } else {
                const std::int32_t T = duration_, t = timer_.current, t1 = T - T / 7;
                if (t >= t1) pendingTime_ = 1000;
                else if (t >= 180 && t1 != 180) pendingTime_ = (t - 180) * 900 / (t1 - 180) + 100;
                else pendingTime_ = 100;
            }
            setFlag(LastCaptured, true);
            if (ctx_.cards && g && !(w.mode & ModeReplay) && id_ >= 0 && id_ < kCardCount) {
                CardRecord& r = (*ctx_.cards)[static_cast<std::size_t>(id_)];
                const auto ch = static_cast<std::size_t>(std::clamp(g->character, 0, 11));
                const bool sp = (w.mode & ModeSpellPractice) != 0;
                auto& best = sp ? r.practiceBest : r.best;
                auto& caps = sp ? r.practiceCaptures : r.captures;
                best[ch] = std::max(best[ch], capture_);
                best[12] = std::max(best[12], capture_);
                if (caps[ch] < 9999) ++caps[ch];
                if (caps[12] < 9999) ++caps[12];
            }
            if (g) ++g->cardsCaptured;
            captured = true;
            if (w.rng) antiTamper(*w.rng);
        }
        log::info("th08: spell card {} {} (bonus {}, time {})", id_, captured ? "captured" : "not captured", captured ? awarded_ : 0u,
                  captured ? pendingTime_ : 0);
        // 4. The circle: released (with the failure message) or turned into the end effect.
        if (circle_) {
            if (!captured) {
                if (lastSpellState(w) != 0) setLastSpellState(w, 0);
                circle_->inUse = false;
                circle_ = nullptr;
                if (w.gui) w.gui->messageB(0, has(LastSpellLook) ? 6 : 5);
            } else {
                setFlag(Pending, true);
                if (lastSpellState(w) != 0) setLastSpellState(w, 2);
                Effect& c = *circle_;
                channel(c.vm, AnmVm::Position, 30, 6);
                c.vm.posStart = {c.radius, c.radius2, c.vm.posStart.z};
                c.vm.posEnd = {256.0f, 0.0f, c.vm.posEnd.z};
                c.vm.position.x = c.radius;
                c.vm.position.y = c.radius2;
                channel(c.vm, AnmVm::Color, 60, 3);
                c.vm.colorStart = c.vm.color;
                c.vm.colorEnd = 0x20D080A0u;
                c.waves = 6.0f;
                c.timer.assign(0);
                endEffect_ = circle_;
                circle_ = nullptr;
                w.playSound(35);
            }
        }
        // 5-8.
        if (w.player) {
            w.player->setBombLockout(16);
            w.player->makeInvulnerable(70);
        }
        if (w.gui) w.gui->refreshLivesBombs();
        w.playSound(15);
        if (w.background) w.background->spellEnd();
    }
    // The tail (always).
    if (boss_) boss_->survival = false;
    boss_ = nullptr;
    setFlag(NoDrain, false);
}

// ---- OnUpdate (spec 3, 5) ---------------------------------------------------------------------

void Spellcard::update(const World& w) {
    if ((w.mode & ModeSpellFreeze) || w.timeStop) return;
    if (has(Active)) {
        if (!boss_ || !boss_->active || boss_->slot != bossSlot_) close();  // (the pass goes on)
        // The name block fades while the player is under it (field coordinates).
        if (w.player) {
            const Vec3f p = w.player->position();
            if (p.x >= 64.0f && p.y < 64.0f) {
                if (boxAlpha_ > 32) boxAlpha_ = static_cast<std::uint8_t>(boxAlpha_ - 4);
            } else if (boxAlpha_ < 128) {
                boxAlpha_ = static_cast<std::uint8_t>(boxAlpha_ + 4);
            }
        }
        if (has(Capturable)) {
            if (!has(NoDrain) && !(boss_ && boss_->survival)) {
                const float d = static_cast<float>(drain_ / 60u) * g_timing.speed;
                capture_ -= static_cast<std::uint32_t>(x86ToInt(d));  // (unsigned, wraps [QUIRK])
                capture_ -= capture_ % 10u;
            }
        } else if (circle_ && circle_->vm.activeSprite == 221) {
            circle_->vm.selectSprite(222);  // the thin "failed" ring
            circle_->vm.scaleEnd.x = 4.0f;
            circle_->vm.scale.x = 4.0f;
        }
        if (circle_) {
            Effect& c = *circle_;
            // The wave (spec 3.4) and the radius schedule (3.5).
            if (c.waves != 0.0f) {
                c.radius2 = c.vm.position.y;
                if (c.radius2 == 0.0f) c.waves = 0.0f;
            }
            if (c.vm.duration[AnmVm::Position].current == 0) {
                channel(c.vm, AnmVm::Position, duration_ - 100, 0);
                c.vm.posStart = {256.0f, 0.0f, c.vm.posStart.z};
                c.vm.posEnd = {8.0f, 0.0f, c.vm.posEnd.z};
            }
            if (!has(CircleFrozen) && boss_) {
                const Vec3f target{boss_->pos.x + boss_->offset.x, boss_->pos.y + boss_->offset.y, boss_->pos.z + boss_->offset.z};
                const float k = 1.0f / 16.0f;
                c.centre.x = (target.x - c.centre.x) * k + c.centre.x;
                c.centre.y = (target.y - c.centre.y) * k + c.centre.y;
                c.centre.z = 0.0f;
            }
            const float s = has(Active) && has(LastSpellLook) ? -0.031415928f : 0.015707964f;
            c.angle = addNormalizeAngle(c.angle, s);
        }
    } else if (endEffect_) {
        Effect& c = *endEffect_;
        const std::int32_t t = c.timer.current;
        if (t == 30) {
            channel(c.vm, AnmVm::Scale, 20, 1);
            c.vm.scaleStart.x = c.width;
            c.vm.scaleEnd.x = 64.0f;
            channel(c.vm, AnmVm::Position, 100, 4);
            c.vm.posStart.x = c.radius;
            c.vm.posEnd.x = 0.0f;
            c.vm.posStart.y = c.radius2;
            c.vm.posEnd.y = 60.0f;
            c.vm.position.x = c.radius;
            c.vm.position.y = c.radius2;
        } else if (t == 60) {
            channel(c.vm, AnmVm::Scale, 70, 1);
            c.vm.scaleStart.x = c.width;
            c.vm.scaleEnd.x = 0.0f;
        } else if (t == 130) {
            award(w);
            c.inUse = false;
            endEffect_ = nullptr;
        }
        if (endEffect_) {
            const Vec3f p = w.player ? w.player->position() : Vec3f{};
            const float k = t <= 80 ? 1.0f / 16.0f : 1.0f / 4.0f;
            c.centre.x = (p.x - c.centre.x) * k + c.centre.x;
            c.centre.y = (p.y - c.centre.y) * k + c.centre.y;
            c.angle = addNormalizeAngle(c.angle, t <= 80 ? -0.015707964f : -0.05235988f);
            c.centre.z = 0.0f;
            c.radius2 = c.vm.position.y;
            if (t > 8 && pendingTime_ > 0) orbBurst(w, t);
        }
    }
    if (w.rng) {
        for (AnmVm* vm : {&playerFace_, &decorA_, &decorB_, &bombName_, &bombBox_, &bossFace_, &cardName_, &ghost1_, &ghost2_, &cardBox_,
                          &indicator_}) {
            vm->tick(*w.rng);
        }
    }
    timer_.decrement(1);
}

// The award at t = 130 (spec 5.3).
void Spellcard::award(const World& w) {
    if (w.gui) w.gui->messageC(static_cast<std::int32_t>(awarded_));
    if (w.state) w.state->score += awarded_ / 10u;
    setFlag(Pending, false);
    if (pendingTime_ > 0) {
        w.addTimePoints(pendingTime_);
        if (w.state) w.state->pointValue += 10 * pendingTime_;
        pendingTime_ = 0;
    }
}

// The time orbs (spec 5.4): two batches 128 px from the end effect on opposite sides, up to 7
// points each (the second batch always spawns 6 orbs [QUIRK]).
void Spellcard::orbBurst(const World& w, std::int32_t /*t*/) {
    if (!w.bullets || !endEffect_) return;
    const Effect& c = *endEffect_;
    float a = c.timer.asFloat() - 10.0f;
    a = a * 6.2831855f;
    a = a / 40.0f;
    a = a - 1.5707964f;
    a = addNormalizeAngle(a, 0.0f);
    auto point = [&](float angle) {
        return Vec3f{cosTimes(angle, 128.0f) + c.centre.x, sinTimes(angle, 128.0f) + c.centre.y, c.centre.z};
    };
    Vec3f p = point(a);
    std::int32_t n = std::min(pendingTime_, 7);
    for (std::int32_t i = 0; i < n; ++i) w.bullets->spawnItem(w, p, 10, 0);
    pendingTime_ -= n;
    a = addNormalizeAngle(a, kPi);
    p = point(a);
    n = std::min(pendingTime_, 7);
    for (int i = 0; i < 6; ++i) w.bullets->spawnItem(w, p, 10, 0);
    pendingTime_ -= n;
    if (pendingTime_ < 0) pendingTime_ = 0;
}

// ---- OnDraw (spec 7) --------------------------------------------------------------------------

void Spellcard::draw(SpriteSink& sink) {
    if (!loaded_) return;
    const AnmDrawState none;
    if (playerFace_.has(AnmVm::Visible)) {
        drawVm(sink, playerFace_, none);
        drawVm(sink, decorA_, none);
        drawVm(sink, decorB_, none);
    }
    if (bossFace_.has(AnmVm::Visible)) {
        AnmVm v = bossFace_;
        v.position = {v.position.x + v.offset.x, v.position.y + v.offset.y, v.position.z + v.offset.z};
        drawVm(sink, v, none);
    }
    if (bombName_.has(AnmVm::Visible)) {
        bombBox_.position = {bombName_.position.x - 32.0f, bombName_.position.y, bombName_.position.z};
        drawVm(sink, bombBox_, none);
        drawVm(sink, bombName_, none);
    }
    if (cardName_.has(AnmVm::Visible)) {
        AnmDrawState m;
        m.multiply = true;
        m.multiplyColor = (static_cast<std::uint32_t>(boxAlpha_) << 24) | 0x808080u;
        cardBox_.position = cardName_.position;
        drawVm(sink, cardBox_, m);
        drawVm(sink, cardName_, m);
        drawVm(sink, ghost1_, m);
        drawVm(sink, ghost2_, m);
        drawVm(sink, indicator_, m);
        if (!has(NameOnly)) drawDigits(sink, m);
    }
}

// The bonus and history digits (spec 7.4), through the digit stamp.
void Spellcard::drawDigits(SpriteSink& sink, const AnmDrawState& state) {
    AnmVm& d = digit_;
    float x = indicator_.position.x - 40.0f;
    const float y = indicator_.position.y + 1.0f;
    auto stamp = [&](std::uint32_t digit) {
        d.selectSprite(136 + static_cast<int>(digit));
        d.position = {x, y, indicator_.position.z};
        drawVm(sink, d, state);
    };
    std::uint32_t v = has(Capturable) ? capture_ : 0;
    bool leading = false;
    for (std::uint32_t div = 10000000u; div >= 1; div /= 10u) {
        const std::uint32_t q = v / div;
        if (q != 0) leading = true;
        if (leading || div == 1) stamp(q % 10u);
        x += 7.0f;
        v %= div;
        if (div == 1) break;
    }
    // The history: captures / attempts for this character (spell practice: the practice ones).
    std::uint32_t caps = 0, tries = 0;
    if (ctx_.cards && id_ >= 0 && id_ < kCardCount) {
        const CardRecord& r = (*ctx_.cards)[static_cast<std::size_t>(id_)];
        const auto ch = static_cast<std::size_t>(std::clamp(character_, 0, 11));
        caps = practice_ ? r.practiceCaptures[ch] : r.captures[ch];
        tries = practice_ ? r.practiceAttempts[ch] : r.attempts[ch];
    }
    caps = std::min(caps, 999u);
    tries = std::min(tries, 999u);
    auto three = [&](std::uint32_t n) {
        if (n / 100 != 0) {
            stamp(n / 100);
            n %= 100;
            leading = true;
        }
        x += 7.0f;
        if (n / 10 != 0 || leading) {
            stamp(n / 10);
            n %= 10;
        }
        x += 7.0f;
        stamp(n % 10);
    };
    x += 32.0f;
    leading = false;
    three(caps);
    x += 13.0f;
    three(tries);  // (leading is not reset [QUIRK])
}

}  // namespace touhou::th08
