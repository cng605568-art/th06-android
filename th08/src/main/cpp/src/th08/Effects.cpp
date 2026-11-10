#include "th08/Effects.hpp"

#include "gfx/SpriteSink.hpp"
#include "th08/Background.hpp"
#include "th08/ExeTables.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {
enum List { ListA, ListB, ListC, ListD, ListE };
}  // namespace

void Effects::reset() {
    for (Effect& e : slots_) e = Effect{};
    sink_ = Effect{};
    cursor_ = 0;
    active_ = 0;
    frames_ = 0;
    listCount_.fill(0);
}

void Effects::start(const World& w, Effect& e, int type, const Vec3f& pos, std::uint32_t color) {
    e = Effect{};
    e.inUse = true;
    e.type = static_cast<std::uint8_t>(type);
    e.pos = pos;
    if (w.tables && w.anm && w.rng && type >= 0 && type < ExeTables::kEffectTypes) {
        const ExeTables::Effect& info = w.tables->effects[static_cast<std::size_t>(type)];
        e.vm.start(*w.anm, *w.rng, kSlotEtama, info.script);  // its first tick runs now (RNG)
        e.update = info.update;
    }
    e.vm.set(AnmVm::ZWriteDisable, true);
    e.vm.color = color;
    e.vm.offset = {};
}

Effect& Effects::spawn(const World& w, int type, const Vec3f& pos, int count, std::uint32_t color) {
    Effect* p = &slots_[static_cast<std::size_t>(cursor_)];
    Effect* last = &sink_;
    for (int step = 0; step < kRing; ++step) {
        cursor_ = (cursor_ + 1) % kRing;
        if (p->inUse) {
            p = &slots_[static_cast<std::size_t>(cursor_)];
            continue;
        }
        start(w, *p, type, pos, color);
        if (!runInit(w, *p)) p->inUse = false;
        last = p;
        if (--count <= 0) break;
        p = &slots_[static_cast<std::size_t>(cursor_)];
    }
    return *last;
}

Effect& Effects::spawnSpecial(const World& w, int type, const Vec3f& pos, int k, int count, std::uint32_t color) {
    if (k < 0 || k >= kSpecial) return sink_;
    Effect& e = special(k);
    for (int i = 0; i < std::max(1, count); ++i) {
        start(w, e, type, pos, color);
        if (!runInit(w, e)) e.inUse = false;
    }
    return e;
}

// The per-type init routines (spec_effects, to come): none ported yet, so every spawn keeps
// only its script's draws.
bool Effects::runInit(const World& w, Effect& e) {
    (void)w, (void)e;
    return true;
}

// The per-type update routines (spec_effects, to come): an effect lives until its script ends.
bool Effects::runUpdate(const World& w, Effect& e) {
    (void)w, (void)e;
    return true;
}

void Effects::update(const World& w) {
    listCount_.fill(0);
    active_ = 0;
    auto list = [&](int l, int slot) { lists_[static_cast<std::size_t>(l)][static_cast<std::size_t>(listCount_[static_cast<std::size_t>(l)]++)] = static_cast<std::int16_t>(slot); };
    for (int i = 0; i < kSlots; ++i) {
        Effect& e = slots_[static_cast<std::size_t>(i)];
        if (!e.inUse) continue;
        ++active_;
        if (!(w.mode & ModeSpellFreeze) || e.ignoreFreeze) {
            if (!runUpdate(w, e)) {
                e.inUse = false;
                continue;
            }
            if (w.rng && e.vm.tick(*w.rng)) {
                e.inUse = false;
                continue;
            }
            e.timer.tick();
        }
        if (e.type == 64) continue;  // the 6B moon: drawn by the stage itself
        if (e.layer == 1 || e.layer >= 3) list(ListB, i);
        else if (e.layer == 2) list(ListC, i);
        else if (e.listD) list(ListD, i);
        else if (e.vm.blend() == 1) list(ListE, i);
        else list(ListA, i);
    }
    ++frames_;
}

void Effects::draw(SpriteSink& sink, const AnmDrawState& state, Background* bg) {
    auto draw2D = [&](int l, float z) {
        for (int k = 0; k < listCount_[static_cast<std::size_t>(l)]; ++k) {
            Effect& e = slots_[static_cast<std::size_t>(lists_[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)])];
            e.vm.position = {e.pos.x + kPlayfieldX, e.pos.y + kPlayfieldY, z};
            drawVm(sink, e.vm, state);
        }
    };
    draw2D(ListA, 0.07f);
    for (int k = 0; k < listCount_[ListC]; ++k) {
        Effect& e = slots_[static_cast<std::size_t>(lists_[ListC][static_cast<std::size_t>(k)])];
        e.vm.position = e.pos;
        if (bg) bg->drawPlayfieldBillboard(sink, e.vm, state);
    }
    draw2D(ListE, 0.07f);
}

void Effects::drawListD(SpriteSink& sink, const AnmDrawState& state) {
    for (int k = 0; k < listCount_[ListD]; ++k) {
        Effect& e = slots_[static_cast<std::size_t>(lists_[ListD][static_cast<std::size_t>(k)])];
        e.vm.position = {e.pos.x + kPlayfieldX, e.pos.y + kPlayfieldY, 0.04f};
        drawVm(sink, e.vm, state);
    }
}

void Effects::drawListB(SpriteSink& sink, const AnmDrawState& state, Background& bg) {
    if (quality_ <= 0) return;
    for (int k = 0; k < listCount_[ListB]; ++k) {
        if (quality_ == 1 && (k & 1)) break;  // medium quality stops at the first odd one [QUIRK]
        Effect& e = slots_[static_cast<std::size_t>(lists_[ListB][static_cast<std::size_t>(k)])];
        if (e.layer == 4) {  // the lantern glows: screen positions
            e.vm.position = e.pos;
            drawVm(sink, e.vm, state);
        } else if (e.layer == 1) {
            e.vm.position = e.pos;
            bg.drawBillboard(sink, e.vm, state);
        } else {
            e.vm.position = e.pos;
            bg.drawProjected(sink, e.vm, state);
        }
    }
}

}  // namespace touhou::th08
