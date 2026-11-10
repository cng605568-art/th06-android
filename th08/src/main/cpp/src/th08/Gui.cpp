#include "th08/Gui.hpp"

#include <algorithm>
#include <format>

#include "core/Log.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/ProceduralArt.hpp"
#include "io/Vfs.hpp"
#include "th08/ExeTables.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {

const char* stageTextAnm(int stage) {
    static const char* const kNames[9] = {"stg1txt.anm", "stg2txt.anm", "stg3txt.anm", "stg4atxt.anm", "stg4btxt.anm",
                                          "stg5txt.anm", "stg6txt.anm", "stg7txt.anm", "stg8txt.anm"};
    return kNames[std::clamp(stage, 0, 8)];
}

// The loading screen by character (table 0x4C72C4): the teams' own, then the solo human
// ("h") or youkai ("a") of each team.
std::string loadingAnm(int character) {
    if (character < 4) return std::format("loading0{}.anm", std::max(0, character));
    const int solo = std::min(character, 11) - 4;
    return std::format("loading0{}{}.anm", solo / 2, solo % 2 == 0 ? 'h' : 'a');
}

std::uint32_t withAlpha(std::uint32_t argb, std::uint32_t alpha) { return (argb & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24); }

}  // namespace

bool Gui::stageStart(const FlowContext& ctx, const GameState& g, bool firstStage, std::string* error) {
    if (!ctx.vfs || !ctx.anm || !ctx.textures || !ctx.rng) {
        if (error) *error = "no game files";
        return false;
    }
    AnmLibrary& lib = *ctx.anm;
    ZunRng& rng = *ctx.rng;
    anm_ = &lib;
    ui_ = ctx.ui;
    spellPractice_ = g.spellPractice;
    if (firstStage) {
        if (!lib.load(*ctx.vfs, *ctx.textures, "front.anm", kSlotFront, error)) return false;
        if (!lib.load(*ctx.vfs, *ctx.textures, "times.anm", kSlotTimes, error)) return false;
        if (!lib.load(*ctx.vfs, *ctx.textures, loadingAnm(g.character), kSlotLoading, error)) return false;
        lastSpellMarker_ = AnmVm{};
        lastSpellMarker_.start(lib, rng, kSlotAscii, 26);
        difficultyLabel_ = AnmVm{};
        difficultyLabel_.start(lib, rng, kSlotAscii, 25);
        difficultyLabel_.selectSprite(283 + std::clamp(g.difficulty, 0, 4));
    }
    loaded_ = true;
    // The clocks: the in-stage one starts hidden (ECL starts it); the title one shows the night.
    stageClock_ = AnmVm{};
    stageClock_.color &= 0x00FFFFFFu;
    titleClock_ = AnmVm{};
    titleClock_.startFresh(lib, rng, kSlotTimes, 0);
    titleClock_.selectSprite(g.nightSteps);
    if (!lib.load(*ctx.vfs, *ctx.textures, stageTextAnm(g.stage), kSlotStageText, error)) return false;
    if (firstStage) {
        for (int i = 0; i < kFrontVms; ++i) {
            front_[static_cast<std::size_t>(i)] = AnmVm{};
            front_[static_cast<std::size_t>(i)].start(lib, rng, kSlotFront, i);
        }
    }
    frame_ = 0;
    const AnmFile* text = lib.file(kSlotStageText);
    for (int i = 0; i < 4; ++i) {
        const auto k = static_cast<std::size_t>(i);
        stageText_[k] = AnmVm{};
        hasStageText_[k] = !g.spellPractice && text && text->script(i) != nullptr;
        if (!hasStageText_[k]) continue;
        stageText_[k].start(lib, rng, kSlotStageText, i);
        stageText_[k].baseSprite = stageText_[k].activeSprite;
    }
    nightCaption_ = AnmVm{};
    nightCaption_.start(lib, rng, kSlotAscii, 3);
    refresh_.fill(2);
    fullRedraw_ = 16;
    // The results of the previous stage go (the total is kept: frame F shows it [QUIRK]).
    resultsState_ = 0;
    displayed_ = 0;
    stage_ = g.stage;
    practice_ = g.practice;
    picture_ = AnmVm{};
    hasPicture_ = false;
    clockShown_ = false;
    return true;
}

// ---- the night clock and the music caption ---------------------------------------------------

void Gui::showClock(ZunRng& rng, int steps) {
    if (!anm_) return;
    stageClock_ = AnmVm{};
    stageClock_.start(*anm_, rng, kSlotTimes, 2);
    stageClock_.selectSprite(steps);
    clockShown_ = true;
}

void Gui::hideClock() {
    stageClock_ = AnmVm{};
    stageClock_.color &= 0x00FFFFFFu;
    clockShown_ = false;
}

void Gui::clockStep(int steps) {
    stageClock_.selectSprite(steps);
    stageClock_.interrupt(steps >= 12 ? 2 : 1);
}

void Gui::musicCaption(ZunRng& rng, int track) {
    if (!anm_ || !anm_->loaded(kSlotStageText)) return;
    AnmVm& vm = stageText_[3];
    vm = AnmVm{};
    vm.start(*anm_, rng, kSlotStageText, 3);
    vm.selectSprite(3 + track);
    hasStageText_[3] = true;
}

// ---- results ---------------------------------------------------------------------------------

void Gui::stageResults(const World& w, GameState& g) {
    points_ = g.stagePointItems;
    time_ = g.timePoints;
    graze_ = g.stageGraze;
    before_ = 660 + 30 * g.nightSteps;
    // The night advance (0x43C35F): stages 1-5 one step with the quota met, else two; the
    // finals none; Extra four. A plain add (no cap, no RNG).
    if (g.stage <= 5) advance_ = g.timePoints >= g.timeQuota ? 1 : 2;
    else if (g.stage <= 7) advance_ = 0;
    else advance_ = 4;
    g.nightSteps += advance_;
    base_ = w.tables ? w.tables->clearBase[static_cast<std::size_t>(std::clamp(g.stage, 0, 8))] : 1000000;
    target_ = 660 + 30 * g.nightSteps;
    displayed_ = before_;
    delay_ = 0;
    resultsState_ = 1;
    w.mode |= ModeResults;
    if (anm_ && w.rng) {
        if (g.stage < 6) {
            nightCaption_ = AnmVm{};
            nightCaption_.start(*anm_, *w.rng, kSlotAscii, 3);
            nightCaption_.selectSprite(128 + advance_);
            picture_ = AnmVm{};
            picture_.start(*anm_, *w.rng, kSlotLoading, 0);
            hasPicture_ = true;
        } else {
            nightCaption_.current = nullptr;
        }
        nightCaption_.interrupt(1);
    }
    if (g.stage >= 6) g.extends = -1;  // no extend after the final results
    // The Ghost team's bomb (characters 3, 10, 11), stages 1-5 only.
    if (g.stage < 6 && g.bombs < 3.0f && (g.character == 3 || g.character == 10 || g.character == 11)) {
        g.bombs += 1.0f;
        if (w.rng) antiTamper(*w.rng);
        w.playSound(35);
        refresh_[1] = 2;
    }
}

bool Gui::resultsStep(GameState& g, std::uint16_t gameInput, int configuredLives, bool stageEndIdle) {
    configuredLives_ = configuredLives;
    if (hasPicture_ && anm_) {
        // (ticked by the update; nothing simulation-relevant)
    }
    // The bonus (0x435F21), the first update after op 9.
    if (resultsState_ == 1) {
        std::int32_t b = base_ + graze_ * 50 + points_ * 5000 + time_ * 100;
        if (g.stage >= 6 && !g.practice) b += g.livesI() * 2500000 + g.bombsI() * 500000;
        if (g.stage == 7) b += (12 - g.nightSteps) * 2000000;
        switch (g.difficulty) {
            case 0: b /= 2; break;
            case 2: b = b * 12 / 10; break;
            case 3: b = b * 15 / 10; break;
            case 4: b *= 2; break;
            default: break;
        }
        switch (configuredLives) {
            case 3: b = b * 5 / 10; break;
            case 4: b = b * 2 / 10; break;
            case 5: b /= 10; break;
            case 6: b /= 20; break;
            default: break;
        }
        total_ = b;
        log::info("th08: stage {} clear bonus {} (score {})", g.stage + 1, b, g.score);
        for (int i = 0; i < 10; ++i) g.score += static_cast<std::uint32_t>(b / 10);
        resultsState_ = 2;
    }
    // Stages 1-5 end one frame after the clock reaches its target.
    bool end = false;
    if (g.stage < 6 && displayed_ != 0 && displayed_ >= target_ && stageEndIdle) end = true;
    // The count-up.
    if (displayed_ != 0 && displayed_ != target_) {
        if (delay_ < 60) {
            ++delay_;
        } else if (displayed_ < target_) {
            ++displayed_;
            if (gameInput & (BtnShoot | BtnSkip)) displayed_ += 3;
            if (displayed_ > target_) displayed_ = target_;
        } else {
            ++delay_;
        }
    }
    return end;
}

bool Gui::stagePictureFinished() const { return hasPicture_ && picture_.sprite && picture_.has(AnmVm::Stopped); }

namespace {

std::string clockText(std::int32_t m) {
    const int h = m / 60;
    return std::format("{}{:2d}:{:02d}", h < 12 ? "PM" : "AM", h % 12, m % 60);
}

}  // namespace

// The tally (0x43826B; spec_stage_gui 3.10), every Gui draw while the results are up.
void Gui::drawResults(const GameState& g, AsciiText& ascii) {
    const bool finals = g.stage >= 6;
    float y = (g.stage == 7 || g.stage == 8) ? 80.0f : 96.0f;
    const float x = 120.0f;
    ascii.add(x, y, finals ? "All Clear!" : "Stage Clear", 0xFFFFFF40u);
    y += 32.0f;
    ascii.add(x, y, std::format("Clear = {:8d}0", base_), 0xFFFFFFFFu);
    y += 16.0f;
    ascii.add(x, y, std::format("Point = {:8d}0", points_ * 5000), 0xFFE0E0FFu);
    y += 16.0f;
    ascii.add(x, y, std::format("Graze = {:8d}0", graze_ * 50), 0xFFD0D0FFu);
    y += 16.0f;
    ascii.add(x, y, std::format("Time  = {:8d}0", time_ * 100), 0xFFD0D0FFu);
    const std::int32_t frames = std::max(1, g.stageFrames);
    const std::int32_t human = static_cast<std::int32_t>(static_cast<std::int64_t>(g.stageHuman80) * 10000 / frames);
    const std::int32_t youkai = static_cast<std::int32_t>(static_cast<std::int64_t>(g.stageYoukai80) * 10000 / frames);
    y += 16.0f;
    ascii.add(x, y, std::format("over-80% = {:3d}.{:02d}%", human / 100, human % 100), 0xFFD0D0FFu);
    y += 16.0f;
    ascii.add(x, y, std::format("over 80% = {:3d}.{:02d}%", youkai / 100, youkai % 100), 0xFFD0D0FFu);
    if (finals && !g.practice) {
        y += 16.0f;
        ascii.add(x, y, std::format("Player ={:8d}0", g.livesI() * 2500000), 0xFFFFFF80u);
        y += 16.0f;
        ascii.add(x, y, std::format("Bomb   = {:7d}0", g.bombsI() * 500000), 0xFFFFFF80u);
        if (g.stage == 7) {
            y += 16.0f;
            ascii.add(x, y, std::format("Last Time = {:2d}:{:02d}", (target_ / 60) % 12, target_ % 60), 0xFFFFFF80u);
            y += 16.0f;
            ascii.add(x, y, "Night Bonus", 0xFFFFFF80u);
            y += 16.0f;
            ascii.add(x, y, std::format("        {:8d}0", (12 - g.nightSteps) * 2000000), 0xFFFFFF80u);
        }
    }
    static constexpr const char* kRank[5] = {"Easy    Rank*0.5", "Normal  Rank*1.0", "Hard    Rank*1.2", "Lunatic Rank*1.5", "Extra   Rank*2.0"};
    y += 32.0f;
    ascii.add(x, y, kRank[std::clamp(g.difficulty, 0, 4)], 0xFFFF8080u);
    // The lives setting's penalty (main game only).
    static constexpr const char* kPenalty[4] = {"Player Penalty*0.5", "Player Penalty*0.2", "Player Penalty*0.1", "Player Penalty*0.05"};
    if (g.difficulty < 4 && !g.practice && configuredLives_ >= 3 && configuredLives_ <= 6) {
        y += 16.0f;
        ascii.add(x, y, kPenalty[configuredLives_ - 3], 0xFFFF8080u);
    }
    y += 16.0f;
    ascii.add(x, y, std::format("Total = {:8d}0", total_), 0xFFFFFFFFu);
    if (!finals) {
        y += 40.0f;
        ascii.add(x, y, clockText(before_), 0xFFDFDFDFu);
        ascii.add(x + 99.0f, y, ">>", 0xFFAFAFAFu);
        ascii.add(x + 133.0f, y, clockText(displayed_), 0xFFFF8F8Fu);
    }
}

void Gui::release(AnmLibrary& anm) {
    if (!loaded_) return;
    for (int slot : {kSlotFront, kSlotTimes, kSlotLoading, kSlotStageText}) anm.release(slot);
    loaded_ = false;
}

void Gui::update(ZunRng& rng, const GameState& g, const Vec3f& playerPos, bool frozen, bool dialogueRunning) {
    (void)g;
    if (!loaded_ || frozen) return;
    msgRunning_ = dialogueRunning;
    // 1. The boss HUD state machine (spec_spellcard_hud 8.2), only while no dialogue runs. The
    // label (front VM 12) parks on a stop after its intro, so a boss leaving after it hides the
    // whole HUD in that frame.
    if (!dialogueRunning) {
        AnmVm& label = front_[12];
        const bool stopped = label.has(AnmVm::Stopped);
        if (bossPresent_) {
            if (bossState_ == 0) {
                label.interrupt(1);
                bossState_ = 1;
                bossAlpha_ = 0;
            } else {
                if (stopped) bossState_ = 2;
                bossAlpha_ = bossAlpha_ < 252 ? bossAlpha_ + 4 : 255;
            }
        } else if (bossState_ != 0) {
            if (bossState_ <= 2) {
                label.interrupt(2);
                bossState_ = 3;
            }
            bossAlpha_ = bossAlpha_ > 0 ? bossAlpha_ - 4 : 0;  // (unsigned)
            if (stopped) {
                bossState_ = 0;
                bossFill_ = 0.0f;
                bossAlpha_ = 0;
            }
        }
        if (bossState_ >= 2) {
            if (bossFraction_ > bossFill_) {
                bossFill_ += 0.01f;
                if (bossFill_ > bossFraction_) bossFill_ = bossFraction_;
            } else if (bossFraction_ < bossFill_) {
                bossFill_ -= 0.02f;
                if (bossFill_ < bossFraction_) bossFill_ = bossFraction_;
            }
        }
    }
    for (AnmVm& vm : front_) vm.tick(rng);
    for (int i = 0; i < 4; ++i) {
        if (hasStageText_[static_cast<std::size_t>(i)]) stageText_[static_cast<std::size_t>(i)].tick(rng);
    }
    if (!spellPractice_ && hasStageText_[0] && (stageText_[0].color >> 24) != 0) titleClock_.tick(rng);
    nightCaption_.tick(rng);
    stageClock_.tick(rng);
    if (hasPicture_ && picture_.tick(rng)) hasPicture_ = false;
    // The in-stage clock fades to 64 while the player is under it.
    std::uint32_t a = stageClock_.color >> 24;
    if (playerPos.x >= 64.0f && playerPos.y < 128.0f) {
        if (a > 64) a = a - 4 < 64 ? 64 : a - 4;
    } else {
        a = std::min<std::uint32_t>(255, a + 4);
    }
    stageClock_.color = withAlpha(stageClock_.color, a);
    lastSpellMarker_.tick(rng);
    difficultyLabel_.tick(rng);
    // 7. The timed messages (spec_spellcard_hud 9.2): A and B slide in from the right.
    static constexpr std::int32_t kLife[3] = {250, 180, 280};
    for (std::size_t k = 0; k < messages_.size(); ++k) {
        Message& m = messages_[k];
        if (!m.on) continue;
        if (k < 2) m.x = m.timer.current < 30 ? m.timer.asFloat() * -312.0f / 30.0f + 416.0f : 104.0f;
        if (m.timer.current >= kLife[k]) m.on = false;
        m.timer.tick();
    }
    ++frame_;
}

// ---- the boss HUD and the messages (spec_spellcard_hud §8-§9) -------------------------------

void Gui::setBossSegment(int i, float start, float end, std::uint32_t color) {
    if (i < 0 || i >= 8) return;
    segStart_[static_cast<std::size_t>(i)] = start;
    segEnd_[static_cast<std::size_t>(i)] = end;
    segColor_[static_cast<std::size_t>(i)] = color;
}

void Gui::clearBossSegments() {
    segStart_.fill(0.0f);
    segEnd_.fill(0.0f);
}

void Gui::messageA(std::int32_t value) {
    messages_[0] = Message{true, 416.0f, 48.0f, value, 0, Timer{0, 0.0f, 0}};
    requestRedraw(2);
}

void Gui::messageB(std::int32_t value, int type) {
    messages_[1] = Message{true, 416.0f, 168.0f, value, type, Timer{0, 0.0f, 0}};
    requestRedraw(2);
}

void Gui::messageC(std::int32_t value) {
    messages_[2] = Message{true, 224.0f, 16.0f, value, 0, Timer{0, 0.0f, 0}};
    requestRedraw(2);
}

void Gui::bossTimerSounds(const World& w) {
    if (!bossPresent_ || bossState_ == 0 || msgRunning_) return;
    const int v = std::min(bossSeconds_, 99);
    if (bossSeconds_ != lastSeconds_) {
        if (v < 3) w.playSound(38);
        else if (v < 10) w.playSound(29);
    }
    lastSeconds_ = bossSeconds_;
}

// DrawHud2's boss part (spec 8.3): the bar, its segments, the name label, the life markers and
// the timer.
void Gui::drawBossHud(SpriteSink& sink, AsciiText& ascii) {
    if (msgRunning_ || !bossPresent_ || bossState_ == 0) return;
    const std::uint32_t a = (bossAlpha_ & 0xFFu) << 24;
    gradientRect(sink, ui_, 64.0f, 19.0f, 64.0f + bossFill_ * 320.0f, 23.0f, a | 0xFFFFFFu, a | 0x202060u);
    for (std::size_t i = 0; i < 8; ++i) {
        if (segEnd_[i] == 0.0f || !(segStart_[i] < bossFill_)) continue;
        const float e = std::min(segEnd_[i], bossFill_);
        const std::uint32_t c = segColor_[i];
        const std::uint32_t dark = static_cast<std::uint32_t>(static_cast<std::int32_t>(c) >> 2) & 0x3F3F3Fu;
        gradientRect(sink, ui_, 64.0f + segStart_[i] * 320.0f, 19.0f, 64.0f + e * 320.0f, 23.0f, a | (c & 0xFFFFFFu), a | dark);
    }
    const AnmDrawState none;
    drawVm(sink, front_[12], none);
    const int n = lifeMarkers_;
    const int gap = n <= 5 ? 2 : 1;
    for (int i = 0; i < n; ++i) {
        const float x0 = static_cast<float>(static_cast<std::int32_t>(static_cast<float>(i) * 26.0f) / n) + 35.0f;
        const float x1 = static_cast<float>(static_cast<std::int32_t>(static_cast<float>(i + 1) * 26.0f) / n) + 35.0f - static_cast<float>(gap);
        const auto top = static_cast<std::uint32_t>(0xFFFFFF - i * 255 / 9);
        gradientRect(sink, ui_, x0, 19.0f, x1, 23.0f, a | top, a | 0x202020u);
    }
    static constexpr std::uint32_t kTimer[4] = {0xA0D0FFu, 0xA080FFu, 0xE080C0u, 0xFF4040u};
    const int v = std::min(bossSeconds_, 99);
    const std::uint32_t c = bossSeconds_ >= 20 ? kTimer[0] : bossSeconds_ >= 10 ? kTimer[1] : bossSeconds_ >= 5 ? kTimer[2] : kTimer[3];
    ascii.add(384.0f, 16.0f, std::format("{:02d}", std::max(v, 0)), a | c);
}

void Gui::drawMessages(SpriteSink& sink, AnmLibrary& anm) {
    const Message& ma = messages_[0];
    if (ma.on) messageText_.add(ma.x, 48.0f, std::format(" BONUS {:8d}", ma.value), 0xFFFFFF80u);
    const Message& mb = messages_[1];
    if (mb.on) {
        if (mb.type == 1) messageText_.add(mb.x, 168.0f, "Full Power Mode!", 0xFFC0B0FFu);
        else if (mb.type == 5 || mb.type == 6)
            messageText_.add(mb.x, 168.0f, mb.type == 5 ? "Spell Bonus Failed" : "Last Spell Failed", 0xFFE0B0FFu, 1.0f, 11.0f, 0.9f);
    }
    const Message& mc = messages_[2];
    if (mc.on) {
        messageText_.add((384.0f - 17.0f * 14.0f) / 2.0f + 32.0f, 80.0f, "Spell Card Bonus!", 0xFFFF0000u);
        const std::string v = std::format("+{}", mc.value);
        messageText_.add((384.0f - static_cast<float>(v.size()) * 28.0f) / 2.0f + 32.0f, 96.0f, v, 0xFFFF8080u, 2.0f);
    }
    sink.setClip(kPlayfieldX, kPlayfieldY, kPlayfieldW, kPlayfieldH);
    messageText_.draw(sink, anm);
    messageText_.clear();
    sink.clearClip();
}

void Gui::draw(SpriteSink& sink, const GameState& g, AsciiText& ascii) {
    if (!loaded_) return;
    const AnmDrawState none;
    if (hasPicture_) drawVm(sink, picture_, none);
    if (resultsState_ != 0) drawResults(g, ascii);
    for (int i = 0; i < 4; ++i) {
        if (hasStageText_[static_cast<std::size_t>(i)]) drawVm(sink, stageText_[static_cast<std::size_t>(i)], none);
    }
    drawVm(sink, titleClock_, none);
    drawVm(sink, stageClock_, none);
    drawVm(sink, nightCaption_, none);
    drawBossHud(sink, ascii);
    drawPanel(sink, g, ascii);
}

// The side panel (spec 3.4). The original redraws its parts only when they change, over what
// the previous frames left; the port draws every part every frame in the same order.
void Gui::drawPanel(SpriteSink& sink, const GameState& g, AsciiText& ascii) {
    const AnmDrawState none;
    AnmVm& tile = front_[13];
    for (float y = 0; y <= 448.0f; y += 32.0f) {
        tile.position = {0.0f, y, 0.49f};
        drawVm(sink, tile, none);
    }
    for (float x = 416.0f; x <= 608.0f; x += 32.0f) {
        for (float y = 16.0f; y <= 448.0f; y += 32.0f) {
            tile.position = {x, y, 0.49f};
            drawVm(sink, tile, none);
        }
    }
    AnmVm& strip = front_[14];
    for (float x = 0; x <= 512.0f; x += 128.0f) {
        for (float y : {0.0f, 464.0f}) {
            strip.position = {x, y, 0.49f};
            drawVm(sink, strip, none);
        }
    }
    for (int k = 0; k <= 9; ++k) drawVm(sink, front_[static_cast<std::size_t>(k)], none);
    drawVm(sink, difficultyLabel_, none);
    // The row erasers (VM 15) under every value.
    AnmVm& eraser = front_[15];
    for (float y : {40.0f, 56.0f, 88.0f, 104.0f, 136.0f, 152.0f, 168.0f, 184.0f}) {
        eraser.position = {480.0f, y, y < 88.0f ? 0.49f : 0.48f};
        drawVm(sink, eraser, none);
    }
    eraser.position = {512.0f, 464.0f, 0.49f};
    drawVm(sink, eraser, none);
    for (int i = 0; i < g.livesI(); ++i) {
        front_[10].position = {488.0f + 16.0f * static_cast<float>(i), 88.0f, 0.46f};
        drawVm(sink, front_[10], none);
    }
    for (int i = 0; i < g.bombsI(); ++i) {
        front_[11].position = {488.0f + 16.0f * static_cast<float>(i), 104.0f, 0.46f};
        drawVm(sink, front_[11], none);
    }
    // The "Nullity" marker over the lives and bombs rows during a boss Last Spell (spec_spellcard_hud
    // 8.5; the original paints it on the rows' redraw and leaves it there).
    if (nullity_) drawVm(sink, lastSpellMarker_, none);
    for (float x : {32.0f, 160.0f, 288.0f}) {
        strip.position = {x, 464.0f, 0.49f};
        drawVm(sink, strip, none);
    }
    ascii.add(488.0f, 56.0f, std::format("{:09d}", g.guiScore));
    ascii.add(488.0f + 117.0f, 56.0f, std::format("{}", std::min(g.continues, 9)));
    ascii.add(488.0f, 40.0f, std::format("{:09d}", g.hiScore));
    ascii.add(488.0f + 117.0f, 40.0f, std::format("{}", std::min(g.hiScoreContinues, 9)));
    ascii.add(488.0f, 152.0f, std::format("{}", g.graze));
    auto ratio = [&](float y, std::int32_t a, std::int32_t b, std::uint32_t color) {
        const std::string first = std::format("{}", a);
        float x = 488.0f;
        ascii.add(x, y, first, color);
        x += 13.0f * static_cast<float>(first.size());
        ascii.add(x, y, "/", color, 1.0f, 13.0f, 0.5f);
        x += 6.0f;
        ascii.add(x, y, std::format("{}", b), color);
    };
    ratio(168.0f, g.pointItems, g.nextExtend, 0xFFFFFFFFu);
    ratio(184.0f, g.timePoints, g.timeQuota, g.timePoints >= g.timeQuota ? 0xFFFFF0C0u : 0xFFFFFFFFu);
    const int power = g.powerI();
    if (power > 0 && ui_) {
        // The gradient bar: left 0xE0E0E0FF, right 0x80E0E0FF, in 4-pixel steps.
        for (int x = 0; x < power; x += 4) {
            const int w = std::min(4, power - x);
            const float t = static_cast<float>(x) / 128.0f;
            const auto alpha = static_cast<std::uint8_t>(0xE0 + (0x80 - 0xE0) * t);
            sink.fillRect(ui_->white, 488.0f + static_cast<float>(x), 136.0f, static_cast<float>(w), 16.0f, rgba(0xE0, 0xE0, 0xFF, alpha));
        }
    }
    ascii.add(488.0f, 136.0f, power < 128 ? std::format("{}", power) : std::string("MAX"));
    for (int& r : refresh_) r = r > 0 ? r - 1 : 0;
    if (fullRedraw_ > 0) --fullRedraw_;
}

}  // namespace touhou::th08
