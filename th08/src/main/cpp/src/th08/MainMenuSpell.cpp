// Spell Card Practice in the main menu (claude/th08-specs/spec_flow_menus.md §4.6-4.7,
// claude/th08-specs/spec_spell_practice.md §2, §4): the per-stage capture statistics (state 16),
// the card list with its detail panel (state 17), and the automatic return after a game
// (state 15). The card lists, names, conditions and Shift-JIS texts are the player's exe's.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "core/ZunMath.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "th08/Bgm.hpp"
#include "th08/ExeTables.hpp"
#include "th08/GameConfig.hpp"
#include "th08/MainMenu.hpp"
#include "th08/ScoreFile.hpp"

namespace touhou::th08 {
namespace {

constexpr std::uint32_t kRowDim = 0xFF606060u, kWhite = 0xFFFFFFFFu;

int wrapAround(int v, int n) { return n > 0 ? ((v % n) + n) % n : 0; }

std::string cardText(const std::array<std::uint8_t, 48>& a) {
    std::size_t n = 0;
    while (n < a.size() && a[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(a.data()), n);
}
std::string commentText(const std::array<std::uint8_t, 64>& a) {
    std::size_t n = 0;
    while (n < a.size() && a[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(a.data()), n);
}

}  // namespace

int MainMenu::cardDifficulty(const ExeTables* tables, int card) {
    if (!tables) return card >= 205 ? 5 : card >= 191 ? 4 : 1;
    for (int d = 0; d < 5; ++d) {
        const auto& ids = tables->cardCategories[static_cast<std::size_t>(d)];
        if (std::find(ids.begin(), ids.end(), card) != ids.end()) return d;
    }
    return 5;
}

int MainMenu::lastWordStage(int card) {
    static constexpr int kStage[11] = {0, 1, 2, 5, 6, 7, 8, 5, 8, 3, 4};
    const int k = card - 205;
    return k >= 0 && k < 11 ? kStage[k] : 3;
}

// Teams need their own Final A clear (the reached words); solo characters always pass.
bool MainMenu::spellCharacterAllowed(int character) const {
    if (character >= 4) return true;
    return ctx_.scores && ctx_.scores->teamHasFinalA(character);
}

int MainMenu::spellStep(int character, int step) const {
    const int columns = ctx_.solosUnlocked ? 12 : 4;
    int c = wrapAround(character, columns);
    for (int guard = 0; guard < columns && !spellCharacterAllowed(c); ++guard) c = wrapAround(c + step, columns);
    return c;
}

const std::vector<std::int32_t>* MainMenu::spellList() const {
    if (!ctx_.tables) return nullptr;
    return &ctx_.tables->stageCards[static_cast<std::size_t>(std::clamp(ctx_.stage, 0, 9))];
}

void MainMenu::renderText(AnmVm& vm, std::string& line, const std::string& sjis) {
    renderSpriteText(ctx_.textures, ctx_.text, vm, sjis, 0xFFFFFFu, TextAlign::Left);
    line = sjis;
}

// ---- state 15: the automatic return's character step --------------------------------------------

void MainMenu::spellCharacter(const MenuInput& in) {
    loadBackground("title/select00.png");
    ctx_.character = spellStep(ctx_.character, 1);
    cursor_ = ctx_.character;
    sendAll(8);
    showCards(false, ctx_.character);
    setState(State::SpellStage);
    spellStage(in);
}

// ---- state 16: the stages and their capture statistics ------------------------------------------

void MainMenu::spellStage(const MenuInput& in) {
    AnmLibrary& lib = *ctx_.anm;
    if (sub_ == 0) {
        sendAll(18);
        for (const int k : {138, 139}) {
            if (has_[static_cast<std::size_t>(k)]) vm_[static_cast<std::size_t>(k)].interrupt(27);
        }
        if (prevState_ != State::SpellCards) loadBackground("title/select00.png");
        ctx_.character = spellStep(ctx_.character, 1);
        showCards(false, ctx_.character);
        cursor_ = std::clamp(ctx_.stage, 0, 9);
        for (int i = 0; i < 2; ++i) {
            AnmVm& v = listText_[static_cast<std::size_t>(i)];
            v = AnmVm{};
            if (resultTextLoaded_) v.initialiseWithSprite(lib, kSlotResultText, 2 + i);
            v.flags |= 0x1800u;
            v.fontWidth = v.fontHeight = 15;
            if (ctx_.tables) {
                renderText(v, listLines_[static_cast<std::size_t>(i)],
                           i == 0 ? ctx_.tables->practiceText.statsHeader : ctx_.tables->practiceText.rateHeader);
            }
        }
        rateDeco_ = AnmVm{};
        rateDeco_.initialiseWithSprite(lib, kSlotTitle, 144);
        rateDeco_.flags |= 0x1800u;
        ctx_.practice = ctx_.spellPractice = true;
        rates_.fill(0.0f);
        sub_ = 1;
        if (autoReturn_ == 2) {  // on to the card list, on the card just played
            cursor_ = 0;
            if (ctx_.card >= 205) ctx_.stage = 9;
            setState(State::SpellCards);
            spellCards(in);
            return;
        }
    }
    if (sub_ == 3) {  // backing out to the title
        if (stateTimer_ >= 20) {
            ctx_.practice = false;
            titleCursor_ = SpellPractice;
            setState(State::Title);
        }
        return;
    }
    if (inputTimer_ < 8) return;
    int d = 0;
    if (in.pressedOrRepeat(BtnUp)) d = -1;
    else if (in.pressedOrRepeat(BtnDown)) d = 1;
    if (d != 0) {
        cursor_ = wrapAround(cursor_ + d, 10);
        ctx_.playSound(kSndMove);
    }
    const int lr = in.pressedOrRepeat(BtnLeft) ? -1 : in.pressedOrRepeat(BtnRight) ? 1 : 0;
    if (lr != 0) {
        const int columns = ctx_.solosUnlocked ? 12 : 4;
        ctx_.character = spellStep(wrapAround(ctx_.character + lr, columns), lr);
        ctx_.playSound(kSndMove);
        showCards(true, ctx_.character);
        inputTimer_ = 0;  // (the rates ease again)
        return;
    }
    if (in.pressed(kSelect)) {
        ctx_.stage = cursor_;
        ctx_.playSound(kSndSelect);
        ctx_.playSound(kSndSelect);  // (pushed twice [QUIRK])
        cursor_ = 0;
        setState(State::SpellCards);
        spellCards(in);
        return;
    }
    if (in.pressed(kCancel)) {
        ctx_.playSound(kSndBack);
        ctx_.stage = cursor_;
        sub_ = 3;
        stateTimer_ = 0;
        sendAll(6);
    }
}

// ---- state 17: the card list ----------------------------------------------------------------------

void MainMenu::renderSpellRows() {
    const auto* list = spellList();
    if (!list || !ctx_.tables) return;
    AnmLibrary& lib = *ctx_.anm;
    const ExeTables::PracticeText& t = ctx_.tables->practiceText;
    const int n = static_cast<int>(list->size());
    for (int i = 0; i < 15; ++i) {
        AnmVm& v = listText_[static_cast<std::size_t>(i)];
        const int index = page_ * 15 + i;
        if (index >= n) {
            v.set(AnmVm::Visible, false);
            listLines_[static_cast<std::size_t>(i)].clear();
            continue;
        }
        const int k = (*list)[static_cast<std::size_t>(index)];
        v = AnmVm{};
        if (resultTextLoaded_) v.initialiseWithSprite(lib, kSlotResultText, 2 + i);
        v.flags |= 0x1800u;
        v.fontWidth = v.fontHeight = 15;
        std::string text = t.unknown8;
        if (ctx_.scores && k >= 0 && k < kCardCount) {
            if (ctx_.scores->everAttempted(k)) text = cardText(ctx_.scores->cards[static_cast<std::size_t>(k)].name);
            else if (cardDifficulty(ctx_.tables, k) == 5 && ctx_.scores->lastWordUnlocked(k)) text = t.welcome;
        }
        renderText(v, listLines_[static_cast<std::size_t>(i)], text);
        v.color = kRowDim;
    }
    AnmVm& h = listText_[15];
    h = AnmVm{};
    if (resultTextLoaded_) h.initialiseWithSprite(lib, kSlotResultText, 17);
    h.flags |= 0x1800u;
    h.fontWidth = h.fontHeight = 15;
    renderText(h, listLines_[15], t.listHeader);
    h.color = kWhite;
    const int row = cursor_ - page_ * 15;
    if (row >= 0 && row < 15) listText_[static_cast<std::size_t>(row)].color = kWhite;
}

// A cursor change: rows dim, the cursor row white, the panel blank for 21 passes.
void MainMenu::spellCursorMoved() {
    for (int i = 0; i < 15; ++i) listText_[static_cast<std::size_t>(i)].color = kRowDim;
    const int row = cursor_ - page_ * 15;
    if (row >= 0 && row < 15) listText_[static_cast<std::size_t>(row)].color = kWhite;
    for (AnmVm& v : panel_) v.color &= 0x00FFFFFFu;
    detail_ = 21;
}

// The detail panel (spec_spell_practice 2.4): each line rendered at its counter value while the
// panel is blank, then all shown at once at counter 3.
void MainMenu::detailPanel(bool entry) {
    if (!entry && detail_ == 0) return;
    const auto* list = spellList();
    if (!list || list->empty() || !ctx_.tables || !ctx_.scores) return;
    const ExeTables& tb = *ctx_.tables;
    const ExeTables::PracticeText& t = tb.practiceText;
    const ScoreData& sd = *ctx_.scores;
    const int k = (*list)[static_cast<std::size_t>(std::clamp(cursor_, 0, static_cast<int>(list->size()) - 1))];
    if (k < 0 || k >= kCardCount) return;
    const CardRecord& r = sd.cards[static_cast<std::size_t>(k)];
    const bool tried = sd.everAttempted(k);
    const int c = std::clamp(ctx_.character, 0, 11);
    const auto cc = static_cast<std::size_t>(c);
    const int diff = cardDifficulty(ctx_.tables, k);
    panel_[5].position.x = panel_[6].position.x = 96.0f;
    auto due = [&](int value) { return entry || detail_ == value; };
    auto line = [&](int i, const std::string& text) {
        renderText(panel_[static_cast<std::size_t>(i)], panelLines_[static_cast<std::size_t>(i)], text);
    };
    if (due(11)) {
        std::string number;
        const int shown = k + 1;
        for (const int digit : {shown / 100 % 10, shown / 10 % 10, shown % 10}) number += tb.wideDigits[static_cast<std::size_t>(digit)];
        line(0, sjisPrintf(t.panelNumber.c_str(), number.c_str(), tried ? cardText(r.name).c_str() : t.unknown8.c_str()));
    }
    if (due(9)) {
        const bool last = std::find(tb.bossFinal.begin(), tb.bossFinal.end(), k) != tb.bossFinal.end();
        line(1, sjisPrintf(t.panelOwner.c_str(), tried ? cardText(r.owner).c_str() : t.unknown8.c_str(),
                           tb.difficultyNames[static_cast<std::size_t>(std::clamp(diff, 0, 5))].c_str(), last ? "Last" : " "));
    }
    if (entry) {
        line(2, sjisPrintf(t.panelColumns.c_str(), tb.fullNames[cc].c_str()));
        line(4, t.panelNote);
    }
    if (due(7)) {
        if (!tried) {
            line(3, t.statsUntried);
        } else if (diff <= 4) {
            line(3, sjisPrintf(t.statsCard.c_str(), r.practiceCaptures[cc], r.practiceAttempts[cc], r.captures[cc], r.attempts[cc], r.practiceBest[cc],
                               r.practiceCaptures[12], r.practiceAttempts[12], r.captures[12], r.attempts[12], r.practiceBest[12]));
        } else {
            line(3, sjisPrintf(t.statsLastWord.c_str(), r.practiceCaptures[cc], r.practiceAttempts[cc], r.practiceBest[cc], r.practiceCaptures[12],
                               r.practiceAttempts[12], r.practiceBest[12]));
        }
    }
    const bool condition = k >= 204 && k <= 221 && !tried && !sd.lastWordUnlocked(k);
    if (due(5) || due(3)) {
        if (condition) {
            const ExeTables::Condition& cond = tb.lastWordConditions[static_cast<std::size_t>(k - 204)];
            auto ids = [](const std::array<std::int32_t, 5>& a) { return std::array<int, 5>{a[0] + 1, a[1] + 1, a[2] + 1, a[3] + 1, a[4] + 1}; };
            const auto a = ids(cond.ids1), b = ids(cond.ids2);
            if (due(5)) line(5, sjisPrintf(cond.format1.c_str(), a[0], a[1], a[2], a[3], a[4]));
            if (due(3)) line(6, sjisPrintf(cond.format2.c_str(), b[0], b[1], b[2], b[3], b[4]));
        } else {
            const bool shown = r.practiceCaptures[12] != 0;
            if (due(5)) line(5, shown ? commentText(r.comment1) : t.unknown9);
            if (due(3)) line(6, shown ? commentText(r.comment2) : t.unknown9);
        }
    }
    if (entry || detail_ == 3) {
        for (AnmVm& v : panel_) v.color |= 0xFF000000u;
    }
    if (detail_ != 0) --detail_;
}

void MainMenu::spellCards(const MenuInput& in) {
    AnmLibrary& lib = *ctx_.anm;
    const auto* list = spellList();
    const int n = list ? static_cast<int>(list->size()) : 0;
    if (sub_ == 0) {
        sendAll(26);
        autoReturn_ = 0;
        ctx_.practice = ctx_.spellPractice = true;
        if (ctx_.records && ctx_.tables) ctx_.records->updateLastWords(ctx_.tables->bossFinal, ctx_.tables->cardCategories[1]);
        cursor_ = 0;
        if (list) {
            const auto it = std::find(list->begin(), list->end(), ctx_.card);
            if (it != list->end()) cursor_ = static_cast<int>(it - list->begin());
        }
        page_ = cursor_ / 15;
        for (int i = 0; i < 7; ++i) {
            AnmVm& v = panel_[static_cast<std::size_t>(i)];
            v = AnmVm{};
            v.initialiseWithSprite(lib, kSlotText, 21 + i);
            v.flags |= 0x1800u;
            v.fontWidth = v.fontHeight = 15;
            v.color = kWhite;
            v.position = {64.0f, 344.0f + 16.0f * static_cast<float>(i) + (i >= 4 ? 8.0f : 0.0f), 0.0f};
            panelLines_[static_cast<std::size_t>(i)].clear();
        }
        renderSpellRows();
        detail_ = 0;
        detailPanel(true);
        sub_ = 1;
        return;
    }
    detailPanel(false);
    if (inputTimer_ < 8 || n == 0) return;
    const int before = cursor_;
    if (n > 15 && in.pressedOrRepeat(BtnLeft)) {
        cursor_ -= 15;
        if (cursor_ < 0) cursor_ = n - 1;
        ctx_.playSound(kSndMove);
    } else if (n > 15 && in.pressedOrRepeat(BtnRight)) {
        if (cursor_ >= n - n % 15) cursor_ %= 15;
        else cursor_ = std::min(cursor_ + 15, n - 1);
        ctx_.playSound(kSndMove);
    } else if (in.pressedOrRepeat(BtnUp)) {
        cursor_ = wrapAround(cursor_ - 1, n);
        ctx_.playSound(kSndMove);
    } else if (in.pressedOrRepeat(BtnDown)) {
        cursor_ = wrapAround(cursor_ + 1, n);
        ctx_.playSound(kSndMove);
    }
    if (cursor_ != before) {
        if (cursor_ / 15 != page_) {
            page_ = cursor_ / 15;
            renderSpellRows();
        }
        spellCursorMoved();
    }
    const int k = (*list)[static_cast<std::size_t>(cursor_)];
    if (in.pressed(kSelect)) {
        const bool playable = ctx_.scores && (ctx_.scores->everAttempted(k) || (k >= 205 && ctx_.scores->lastWordUnlocked(k)));
        if (!playable) {
            ctx_.playSound(kSndInvalid);
            return;
        }
        ctx_.playSound(kSndSelect);
        ctx_.spellPractice = ctx_.practice = true;
        ctx_.card = k;
        const int diff = cardDifficulty(ctx_.tables, k);
        if (diff == 5) {
            ctx_.stage = lastWordStage(k);
            ctx_.spellDifficulty = 1;
        } else {
            ctx_.spellDifficulty = diff;
        }
        ctx_.replayFlag = false;
        if (ctx_.music) ctx_.music->stop();
        removed_ = true;
        if (ctx_.request) ctx_.request(Scene::Game);
        return;
    }
    if (in.pressed(kCancel)) {
        ctx_.card = k;
        ctx_.playSound(kSndBack);
        cursor_ = ctx_.stage;
        setState(State::SpellStage);
        spellStage(in);
    }
}

// ---- drawing --------------------------------------------------------------------------------------

// A pie (0x46FDC2): an untextured fan of 64 vertices, clockwise from 12 o'clock over 2 pi f.
void MainMenu::drawPie(SpriteSink& sink, float cx, float cy, std::uint32_t argb, float fraction, float size) {
    if (!ctx_.ui || !(fraction > 0.0f)) return;
    const SpriteRegion& r = ctx_.ui->white;
    const float u = (r.u0 + r.u1) * 0.5f, v = (r.v0 + r.v1) * 0.5f;
    const std::uint32_t colour = argbToRgbaBytes(argb);
    std::array<float, 64 * 2> pts{};
    pts[0] = cx, pts[1] = cy;
    float phi = -kZunPi / 2.0f;
    const float step = 0.10134169f;  // 2 pi / 62
    for (int i = 1; i < 64; ++i) {
        pts[static_cast<std::size_t>(2 * i)] = cx + std::cos(phi) * size / 2.0f;
        pts[static_cast<std::size_t>(2 * i + 1)] = cy + std::sin(phi) * size / 2.0f;
        phi = phi + step * fraction;
        if (phi > kZunPi) phi -= kZunTwoPi;
    }
    Draw3DState st;
    st.depthWrite = false;
    st.clipX = 0.0f, st.clipY = 0.0f, st.clipW = 640.0f, st.clipH = 480.0f;
    auto vertex = [&](int i, Vertex3D& out) {
        out.x = (pts[static_cast<std::size_t>(2 * i)] + 0.5f) / 640.0f * 2.0f - 1.0f;
        out.y = 1.0f - (pts[static_cast<std::size_t>(2 * i + 1)] + 0.5f) / 480.0f * 2.0f;
        out.z = -1.0f;
        out.w = 1.0f;
        out.u = u;
        out.v = v;
        out.color = colour;
    };
    for (int i = 1; i < 63; ++i) {
        Vertex3D q[4];
        vertex(0, q[0]);
        vertex(i, q[1]);
        vertex(i + 1, q[2]);
        vertex(i + 1, q[3]);
        sink.drawQuad3D(r.texture, Blend::Alpha, q, st);
    }
}

// The percentage (0x405E10): the ascii digit stamp, e.g. "12.34%", centred on x.
void MainMenu::drawPercent(SpriteSink& sink, float x, float y, float value, std::uint32_t argb) {
    if (!ctx_.anm) return;
    const auto v = static_cast<std::int32_t>(value * 10000.0f);
    const std::int32_t a = std::abs(v);
    const int n = 4 + (a >= 10000 ? 3 : a >= 1000 ? 2 : 1);
    x -= static_cast<float>(n) * 3.5f - 3.5f - 4.0f;
    AnmVm g;
    g.file = ctx_.anm->file(kSlotAscii);
    const AnmDrawState none;
    auto glyph = [&](int sprite, float gx, float gy, float scale) {
        if (!g.selectSprite(sprite)) return;
        g.flags = AnmVm::Visible | AnmVm::Active | AnmVm::AnchorLeft | AnmVm::AnchorTop;
        g.color = argb;
        g.scale = {scale, scale};
        g.position = {gx, gy, 0.0f};
        drawVm(sink, g, none);
    };
    const std::int32_t whole = a >= 10000 ? 100 : a / 100;
    for (const char c : std::to_string(whole)) {
        glyph(136 + (c - '0'), x, y, 1.0f);
        x += 7.0f;
    }
    glyph(147, x, y, 1.0f);
    x += 7.0f;
    glyph(136 + (a % 100) / 10, x, y + 2.0f, 0.8f);
    x += 5.0f;
    glyph(136 + a % 10, x, y + 2.0f, 0.8f);
    x += 5.0f;
    glyph(146, x, y, 1.0f);
}

// State 16 (0x46F5CB): the table anchored to VM 141, the totals, the rate pies.
void MainMenu::drawSpellStage(SpriteSink& sink) {
    if (!ctx_.tables || !ctx_.scores || !has_[141]) return;
    const ExeTables& tb = *ctx_.tables;
    const ScoreData& sd = *ctx_.scores;
    const AnmDrawState none;
    float px = vm_[141].position.x - 32.0f, py = vm_[141].position.y - 152.0f;
    listText_[0].position = {px, py, 0.0f};
    drawVm(sink, listText_[0], none);
    py += 16.0f;
    const auto c = static_cast<std::size_t>(std::clamp(ctx_.character, 0, 11));
    int ta = 0, tb2 = 0, tc = 0, te = 0, tf = 0;
    for (int s = 0; s < 10; ++s) {
        const auto& ids = tb.stageCards[static_cast<std::size_t>(s)];
        int a = 0, b = 0, cc = 0, e = 0, f = 0;
        for (const std::int32_t k : ids) {
            if (k < 0 || k >= kCardCount) continue;
            const CardRecord& r = sd.cards[static_cast<std::size_t>(k)];
            a += r.practiceCaptures[c] > 0 ? 1 : 0;
            b += r.captures[c] > 0 ? 1 : 0;
            e += r.practiceCaptures[12] > 0 ? 1 : 0;
            cc += r.captures[12] > 0 ? 1 : 0;
            f += (sd.everAttempted(k) || (s == 9 && sd.lastWordUnlocked(k))) ? 1 : 0;
        }
        ta += a, tb2 += b, tc += cc, te += e, tf += f;
        const int count = static_cast<int>(ids.size());
        const bool selected = s == cursor_;
        const std::uint32_t colour = selected ? kWhite : 0xFFA0A0A0u;
        const char* prefix = a >= count ? "@" : e >= count ? "*" : " ";
        ascii_.add(px, py, std::string(prefix) + tb.stageLabels[static_cast<std::size_t>(s)], colour, 1.0f, 13.0f, -1.0f, selected);
        ascii_.add(px + 182.0f, py, std::format("{:3d}({:3d})/{:3d}/{:3d}", a, e, f, count), colour, 1.0f, 13.0f * 0.75f, 0.75f, selected);
        py += 16.0f;
    }
    py += 5.0f;
    const char* prefix = ta >= 222 ? "@" : te >= 222 ? "*" : " ";
    ascii_.add(px, py, std::string(prefix) + "Total", 0xFFD06060u);
    ascii_.add(px + 182.0f, py, std::format("{:3d}({:3d})/{:3d}/{:3d}", ta, te, tf, 222), 0xFFD06060u, 1.0f, 13.0f * 0.75f, 0.75f);
    listText_[1].position = {400.0f, 376.0f, 0.0f};
    drawVm(sink, listText_[1], none);
    rateDeco_.position = {552.0f, 426.0f, 0.0f};
    drawVm(sink, rateDeco_, none);
    // The rates, eased over the first 50 input passes after entry or a character change.
    const float targets[4] = {static_cast<float>(ta) / 222.0f, static_cast<float>(tb2) / 205.0f, static_cast<float>(te) / 222.0f,
                              static_cast<float>(tc) / 205.0f};
    for (int i = 0; i < 4; ++i) {
        float r = targets[i];
        float& shown = rates_[static_cast<std::size_t>(i)];
        if (inputTimer_ < 50 && shown != r) r = (r - shown) * static_cast<float>(inputTimer_) / 50.0f + shown;
        shown = r;
    }
    drawPie(sink, 530.0f, 420.0f, 0x60C0C0F0u, rates_[0], 48.0f);
    drawPie(sink, 530.0f, 420.0f, 0x80C0C0F0u, rates_[1], 24.0f);
    drawPie(sink, 562.0f, 438.0f, 0x6080C0C0u, rates_[2], 32.0f);
    drawPie(sink, 562.0f, 438.0f, 0x8080C0C0u, rates_[3], 16.0f);
    drawPercent(sink, 514.0f, 404.0f, rates_[0], 0xFFFFFFFFu);
    drawPercent(sink, 514.0f, 412.0f, rates_[1], 0x80C0C080u);
    drawPercent(sink, 580.0f, 451.0f, rates_[2], 0xFFFFFFFFu);
    drawPercent(sink, 580.0f, 443.0f, rates_[3], 0x80C0C080u);
}

// State 17 (0x46FFC0): the rows, the header, the detail panel.
void MainMenu::drawSpellCards(SpriteSink& sink) {
    const auto* list = spellList();
    if (!list || !ctx_.scores) return;
    const ScoreData& sd = *ctx_.scores;
    const AnmDrawState none;
    listText_[15].position = {334.0f, 78.0f, 0.0f};
    drawVm(sink, listText_[15], none);
    const auto c = static_cast<std::size_t>(std::clamp(ctx_.character, 0, 11));
    const int n = static_cast<int>(list->size());
    for (int i = 0; i < 15 && page_ * 15 + i < n; ++i) {
        const int k = (*list)[static_cast<std::size_t>(page_ * 15 + i)];
        if (k < 0 || k >= kCardCount) continue;
        const CardRecord& r = sd.cards[static_cast<std::size_t>(k)];
        const float y = 94.0f + 16.0f * static_cast<float>(i);
        AnmVm& v = listText_[static_cast<std::size_t>(i)];
        const std::uint32_t colour = v.color;
        const char* prefix = r.practiceCaptures[c] > 0 ? "@" : r.practiceCaptures[12] > 0 ? "*" : " ";
        ascii_.add(16.0f, y, std::format("{}No.{:03d}", prefix, k + 1), colour);
        ascii_.add(430.0f, y, std::format("{:3d}/{:3d}({:3d}/{:3d})", r.practiceCaptures[c], r.practiceAttempts[c], r.captures[c], r.attempts[c]),
                   colour, 1.0f, 13.0f * 0.8f, 0.8f);
        v.position = {118.0f, y, 0.0f};
        drawVm(sink, v, none);
    }
    for (const AnmVm& p : panel_) drawVm(sink, p, none);
}

}  // namespace touhou::th08
