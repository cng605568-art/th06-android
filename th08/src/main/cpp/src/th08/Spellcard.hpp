#pragma once

// Imperishable Night's spell-card object (claude/th08-specs/spec_spellcard_hud.md §1-§7):
// Begin (ECL op 122) and End (op 123), the per-frame update at calc priority 12 (the capture
// value's drain, the circle, the capture award with its time orbs), the failures (bomb, death,
// timeout) and the card's own UI: the portraits, the decor strips, the bomb and card names in
// their boxes, the bonus and history digits (draw priority 15). The object is zeroed at every
// stage start, so a bonus still pending then is lost [QUIRK].

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "th08/Anm.hpp"
#include "th08/ScoreFile.hpp"

namespace touhou {
class SpriteSink;
class TextRenderer;
class TextureLibrary;
}  // namespace touhou

namespace touhou::th08 {

struct Effect;
struct Enemy;
struct World;

class Spellcard {
  public:
    // Flag bits (+0x00, spec 1.4).
    enum Flag : std::uint32_t {
        Active = 1u << 0,
        Capturable = 1u << 2,
        TimedOut = 1u << 3,
        Bit4 = 1u << 4,
        LastSpellLook = 1u << 5,  // the circle spins the other way; "Last Spell Failed"
        CircleFrozen = 1u << 6,   // ECL op 164
        Bombed = 1u << 7,         // a bomb during this card (bosses then take bomb damage)
        Pending = 1u << 8,        // the capture bonus is still to be paid
        LastCaptured = 1u << 9,
        NameOnly = 1u << 10,      // the fake name (ECL ex 22)
        NoDrain = 1u << 11,       // ECL op 184
    };
    static constexpr std::uint32_t kSurvivalCap = 99999990u;

    struct Context {
        AnmLibrary* anm = nullptr;
        TextureLibrary* textures = nullptr;
        TextRenderer* text = nullptr;
        // The game's card records (CATK, spec 2.2 step 18), kept by the game.
        std::array<CardRecord, kCardCount>* cards = nullptr;
    };

    // SpellcardRegister / LoadFaces (spec 1.1-1.2): the object zeroed, the UI VMs bound to
    // their files and frozen. The face files are loaded by the game (slots 15-19).
    void stageStart(const World& w, const Context& ctx);

    // ECL op 122 (spec 2): `ins` is the instruction.
    void begin(const World& w, Enemy& boss, const std::uint8_t* ins);
    // ECL op 123 (spec 6).
    void end(const World& w);
    // The failures (spec 4.1); none of them tests "active".
    void timeout();       // 0x42BC50: the boss timer ran out (not a survival card)
    void bombFailure();   // 0x44CBA0: a bomb or a Last Spell started
    void deathFailure();  // 0x44D150: the player's death was committed
    // ECL ex 22 (spec 2.7) and ex 23 (the card name hidden, 0x416160).
    void fakeName(const World& w, std::string_view sjis);
    void hideName();
    // 0x416B10: a time orb was collected (+8000 up to the cap; else the drain grows).
    void orbBonus(std::uint32_t v);
    // ECL op 155 (the cap of a survival card), op 164 (bit 6), op 184 (bit 11).
    void setSurvivalCap() { cap_ = kSurvivalCap; }
    void setCircleFrozen(bool on) { setFlag(CircleFrozen, on); }
    void setNoDrain(bool on) { setFlag(NoDrain, on); }

    // The bomb banner (0x415D60, spec 2.5): `side` 0 human, 1 youkai; decor sprite 1 for Last
    // Spells. Hidden again by bombBannerOff (0x416130) when the bomb ends.
    void bombBanner(const World& w, int side, std::string_view sjis, int decor);
    void bombBannerOff();

    // OnUpdate, calc priority 12 (spec 3, 5).
    void update(const World& w);
    // OnDraw, draw priority 15 (spec 7), inside the playfield viewport.
    void draw(SpriteSink& sink);

    // ---- inspection (ECL variables 10098-10100, the HUD, the bombs) ----
    bool active() const { return has(Active); }
    bool capturable() const { return has(Capturable); }
    bool has(Flag f) const { return (flags_ & f) != 0; }
    std::uint32_t flags() const { return flags_; }
    std::int32_t id() const { return id_; }
    std::uint32_t capture() const { return capture_; }
    std::uint32_t cap() const { return cap_; }
    std::uint32_t drain() const { return drain_; }
    std::uint32_t awarded() const { return awarded_; }
    std::int32_t pendingTime() const { return pendingTime_; }
    const Timer& timer() const { return timer_; }
    const std::string& name() const { return name_; }
    Enemy* boss() const { return boss_; }
    const Effect* circle() const { return circle_; }
    const Effect* endEffect() const { return endEffect_; }

  private:
    void setFlag(Flag f, bool on) { flags_ = on ? (flags_ | f) : (flags_ & ~static_cast<std::uint32_t>(f)); }
    void close();                       // 0x416AF0: active off, name hidden
    void nameStart(const World& w, int portrait, std::string_view sjis, int decor);  // 0x415F00
    void award(const World& w);         // spec 5.3
    void orbBurst(const World& w, std::int32_t t);  // spec 5.4
    void startVm(const World& w, AnmVm& vm, int slot, int script);
    void drawDigits(SpriteSink& sink, const AnmDrawState& state);

    Context ctx_;
    std::uint32_t flags_ = 0;
    Enemy* boss_ = nullptr;
    std::int32_t id_ = 0, bossSlot_ = 0;
    std::int32_t pendingTime_ = 0;  // P (+0x10)
    std::string name_;
    std::array<std::uint8_t, 64> comment1_{}, comment2_{};
    Effect* circle_ = nullptr;
    Effect* endEffect_ = nullptr;
    std::uint32_t capture_ = 0, drain_ = 0, awarded_ = 0, cap_ = 0;
    Timer timer_ = Timer{0, 0.0f, 0};
    std::int32_t duration_ = 0;
    std::uint8_t boxAlpha_ = 0x80;  // +0x2623: the name block's colour multiplier alpha
    int character_ = 0;             // for the history digits
    bool practice_ = false;

    // The UI VMs (spec 1.3).
    AnmVm playerFace_, bossFace_, decorA_, decorB_, bombName_, cardName_, ghost1_, ghost2_, bombBox_, cardBox_, digit_,
        indicator_;
    bool loaded_ = false;
};

}  // namespace touhou::th08
