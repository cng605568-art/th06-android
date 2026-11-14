#include "th08/Msg.hpp"

#include <algorithm>
#include <cstring>

#include "core/Log.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextRenderer.hpp"
#include "io/Vfs.hpp"
#include "th08/Bullets.hpp"
#include "th08/Enemies.hpp"
#include "th08/ExeTables.hpp"
#include "th08/Flow.hpp"
#include "th08/Gui.hpp"
#include "th08/Player.hpp"
#include "th08/ScoreFile.hpp"
#include "th08/ShotData.hpp"
#include "th08/World.hpp"

namespace touhou::th08 {

namespace {

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::int16_t rdI16(const std::uint8_t* p) { return static_cast<std::int16_t>(rd16(p)); }
std::int32_t rdI32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
}

constexpr std::uint16_t kShoot = 0x0001, kUp = 0x0010, kDown = 0x0020, kSkip = 0x0100;

}  // namespace

bool Dialogue::load(const Context& ctx, const Vfs& vfs, int stage, int character, std::string* error) {
    release();
    ctx_ = ctx;
    stage_ = std::clamp(stage, 0, 8);
    character_ = std::clamp(character, 0, 11);
    faceSlot_ = {kSlotFaceHuman, kSlotFaceYoukai, kSlotFaceBoss, kSlotFaceBoss2};
    index_ = -1;
    if (!ctx.tables) {
        if (error) *error = "no dialogue table (th08.exe)";
        return false;
    }
    const std::string& name = ctx.tables->msgFiles[static_cast<std::size_t>(stage_)][static_cast<std::size_t>(character_)];
    const auto bytes = vfs.read(name);
    if (!bytes || bytes->size() < 4) {
        if (error) *error = "the dialogue file " + name + " could not be read";
        return false;
    }
    file_.assign(bytes->begin(), bytes->end());
    const std::int32_t count = rdI32(file_.data());
    offsets_.clear();
    for (std::int32_t i = 0; i < count && 4 + 4 * static_cast<std::size_t>(i) + 4 <= file_.size(); ++i) {
        offsets_.push_back(static_cast<std::uint32_t>(rdI32(file_.data() + 4 + 4 * static_cast<std::size_t>(i))));
    }
    return true;
}

void Dialogue::release() {
    file_.clear();
    offsets_.clear();
    index_ = -1;
}

const std::uint8_t* Dialogue::instruction() const {
    if (ip_ + 4 > file_.size()) return nullptr;
    const std::uint8_t* p = file_.data() + ip_;
    if (ip_ + 4 + p[3] > file_.size()) return nullptr;
    return p;
}

AnmVm& Dialogue::vmAt(int t) {
    switch (t & 7) {
        case 0: case 1: case 2: case 3: return faces_[static_cast<std::size_t>(t & 3)];
        case 4: return lines_[0];
        case 5: return lines_[1];
        case 6: return vm6_;
        default: return vm7_;
    }
}

// MsgStart (spec 4).
void Dialogue::start(const World& w, int n) {
    if (file_.empty()) return;
    log::info("th08: msg start {}", n);
    // 1. The block is zeroed (the file kept): every VM stops.
    timer_ = Timer{0, 0.0f, 0};
    waited_ = minimum_ = 0;
    for (AnmVm& vm : faces_) vm = AnmVm{};
    for (AnmVm& vm : lines_) vm = AnmVm{};
    vm6_ = vm7_ = AnmVm{};
    resume_ = 0;
    skippable_ = textSpeaker_ = newBox_ = lineCounter_ = boxVisible_ = choice_ = 0;
    speaker_ = 0;
    // 2. The stage's first script: 6B and Extra swap the two boss face files.
    if (n == 0) {
        if (stage_ == 7 || stage_ == 8) std::swap(faceSlot_[2], faceSlot_[3]);
        if (onScriptZero) onScriptZero(n);
    }
    // 3. Stage 5's post-boss dialogue picks Final A text, Final B text or the choice.
    if (n == 10 && stage_ == 5) {
        int pick = 1;
        if (w.state && w.state->continues > 0) {
            pick = 1;
        } else if (isSolo(character_)) {
            pick = 3;
        } else if (ctx_.scores) {
            const ClearRecord& c = ctx_.scores->clears[static_cast<std::size_t>(character_)];
            bool finalB = false, finalA = false;
            for (int d = 0; d < 4; ++d) {
                finalB = finalB || (c.noContinue[static_cast<std::size_t>(d)] & 0x80) != 0;
                finalA = finalA || (c.reached[static_cast<std::size_t>(d)] & 0x40) != 0;
            }
            pick = finalB ? 3 : finalA ? 2 : 1;
        }
        n = pick;
        choice_ = pick == 1 ? 0 : 1;
        w.mode = (w.mode & ~static_cast<std::uint32_t>(ModeFinalB)) | (static_cast<std::uint32_t>(choice_) << 11);
    }
    // 4. Dawn during Kaguya's last phases: the results script instead.
    if (n >= 6 && stage_ == 7 && w.state && w.state->nightSteps >= 12) n = 5;
    // 5.
    if (n < 0 || n >= static_cast<int>(offsets_.size())) {
        log::warn("th08: dialogue script {} is not in the file", n);
        index_ = -1;
        w.dialogue = false;
        return;
    }
    index_ = n;
    ip_ = offsets_[static_cast<std::size_t>(n)];
    lines_[0].scriptIndex = lines_[1].scriptIndex = -1;  // "not started"
    boxVisible_ = 1;
    fontSize_ = 15;
    if (ctx_.tables) colors_ = ctx_.tables->msgColors[static_cast<std::size_t>(character_)];
    skippable_ = 1;
    minimum_ = 6;
    textSpeaker_ = 0;
    newBox_ = 1;
    lineCounter_ = 0;
    speaker_ = 0xFF;
    w.dialogue = true;
    // 6. Every start: the bullets into items, the non-boss enemies killed, the items collected.
    if (w.bullets) w.bullets->clearAll(w, 1);
    if (w.enemies) (void)w.enemies->killAllScoring(0, 0);
    if (w.bullets) w.bullets->collectAll();
}

// "Render text into VM v": its text.anm script, the font size, the masked string decoded into
// a copy (stopping at the byte that decodes to 0), drawn left-aligned in the speaker's colour.
void Dialogue::renderLine(const World& w, AnmVm& vm, int script, std::string_view masked, int color) {
    if (!ctx_.anm || !w.rng) return;
    vm.start(*ctx_.anm, *w.rng, kSlotText, script);
    vm.fontWidth = vm.fontHeight = static_cast<std::uint8_t>(fontSize_);
    // (A translated line of the English text mode is UTF-8 behind its marker and may be longer
    // than the 64 bytes the original's copy holds.)
    std::string text;
    const bool utf8 = !masked.empty() && (static_cast<std::uint8_t>(masked[0]) ^ 0x77) == kUtf8TextMarker;
    for (std::size_t i = 0; i < masked.size() && (i < 64 || utf8); ++i) {
        const char c = static_cast<char>(static_cast<std::uint8_t>(masked[i]) ^ 0x77);
        if (c == 0) break;
        text.push_back(c);
    }
    renderSpriteText(ctx_.textures, ctx_.text, vm, text, colors_[static_cast<std::size_t>(color & 3)]);
}

void Dialogue::selectFace(int k, int sprite) {
    if (!ctx_.anm || k < 0 || k > 3) return;
    AnmVm& vm = faces_[static_cast<std::size_t>(k)];
    AnmFile* file = ctx_.anm->file(faceSlot_[static_cast<std::size_t>(k)]);
    if (!file || !file->loaded()) return;
    if (vm.file != file) vm.file = file;
    vm.selectSprite(sprite);
}

// The speaker switch (ops 15, 17): on a change, the old speaker 6 (changing sides) or 4, the
// others 4; then always 3 on the new speaker.
void Dialogue::speakerSwitch(int s) {
    if (s != speaker_) {
        for (int k = 0; k < 4; ++k) {
            const int which = k == speaker_ ? (k / 2 != s / 2 ? 6 : 4) : 4;
            faces_[static_cast<std::size_t>(k)].interrupt(which);
        }
    }
    if (s >= 0 && s < 4) faces_[static_cast<std::size_t>(s)].interrupt(3);
    speaker_ = static_cast<std::uint8_t>(s);
}

Dialogue::End Dialogue::execute(const World& w, const std::uint8_t* ins, std::uint16_t input, std::uint16_t previous) {
    const std::uint8_t op = ins[2];
    const std::uint8_t size = ins[3];
    const std::uint8_t* a = ins + 4;
    const auto pressed = [&](std::uint16_t bit) { return (input & bit) != 0 && (previous & bit) == 0; };
    const std::string_view str8(reinterpret_cast<const char*>(a + 4), size > 4 ? size - 4 : 0);
    const std::string_view str0(reinterpret_cast<const char*>(a), size);
    AnmLibrary* lib = ctx_.anm;
    switch (op) {
        case 0:
            index_ = -1;
            w.dialogue = false;
            return End::Return;
        case 1: {
            const int k = rdI16(a) & 3;
            if (lib && w.rng) faces_[static_cast<std::size_t>(k)].start(*lib, *w.rng, faceSlot_[static_cast<std::size_t>(k)], rdI16(a + 2));
            return End::Advance;
        }
        case 2:
            selectFace(rdI16(a) & 3, rdI16(a + 2));
            return End::Advance;
        case 3: {
            const int c = rdI16(a), line = rdI16(a + 2) & 1;
            if (line == 0 && lines_[1].scriptIndex >= 0) renderLine(w, lines_[1], 1, "\x57", c);  // " " masked
            renderLine(w, lines_[static_cast<std::size_t>(line)], line, str8, c);
            waited_ = 0;
            return End::Advance;
        }
        case 4: {
            const std::int32_t limit = rdI32(a);
            if (skippable_ && (input & kSkip)) return End::Advance;  // (new box and minimum unchanged [QUIRK])
            if (pressed(kShoot) && waited_ >= minimum_) {
                newBox_ = 1;
                minimum_ = 8;
                return End::Advance;
            }
            if (waited_ >= limit) {
                newBox_ = 1;
                minimum_ = 30;
                return End::Advance;
            }
            ++waited_;
            return End::Stall;
        }
        case 5:
            vmAt(rdI16(a)).interrupt(a[2]);
            return End::Advance;
        case 6:
            ++resume_;
            return End::Advance;
        case 7:
            if (onMusic) onMusic(rdI32(a));
            return End::Advance;
        case 8:
            if (lib && w.rng) vm6_.start(*lib, *w.rng, faceSlot_[2], 1);
            waited_ = 0;
            return End::Advance;
        case 9:
            if (onResults) onResults();
            return End::Advance;
        case 10:
            return End::Stall;
        case 11:
            if (stage_ >= 6 && onStageEnd) onStageEnd();
            return End::Stall;
        case 12:
            if (onMusicFade) onMusicFade();
            return End::Advance;
        case 13:
            skippable_ = a[0];
            return End::Advance;
        case 14:
            if (onWhiteFade) onWhiteFade();
            return End::Advance;
        case 15: {
            const int s = rdI32(a);
            speakerSwitch(s);
            for (int k = 0; k < 4; ++k) {
                const std::int32_t sprite = rdI32(a + 4 + 4 * static_cast<std::size_t>(k));
                if (sprite >= 0) selectFace(k, sprite);
            }
            textSpeaker_ = static_cast<std::uint8_t>(s);
            newBox_ = 1;
            return End::Advance;
        }
        case 16:
            if (newBox_) {
                if (lines_[1].scriptIndex >= 0) renderLine(w, lines_[1], 1, "\x57", textSpeaker_);
                lineCounter_ = 0;
            }
            // VM 4 + line counter: the line VMs, or VM 6/7 past them [QUIRK: the overflow].
            renderLine(w, vmAt(4 + lineCounter_), lineCounter_, str0, textSpeaker_);
            waited_ = 0;
            newBox_ = 0;
            ++lineCounter_;
            return End::Advance;
        case 17: {
            const int s = rdI32(a);
            const std::int32_t sprite = rdI32(a + 4);
            speakerSwitch(s);
            if (sprite >= 0 && s >= 0 && s < 4) selectFace(s, sprite);
            textSpeaker_ = static_cast<std::uint8_t>(s);
            newBox_ = 1;
            return End::Advance;
        }
        case 18:
            boxVisible_ = a[0];
            return End::Advance;
        case 19:
        case 20: {
            const int line = op == 19 ? 0 : 1;
            renderLine(w, lines_[static_cast<std::size_t>(line)], line, str0, 0);
            waited_ = 0;
            return End::Advance;
        }
        case 21: {  // the route menu (spec 7.3)
            const std::int32_t limit = rdI32(a);
            if (pressed(kUp)) {
                if (choice_ == 1 && onSound) onSound(12);
                choice_ = 0;
            }
            if (pressed(kDown)) {
                if (choice_ == 0 && onSound) onSound(12);
                choice_ = 1;
            }
            lines_[choice_].color = 0xFFFFFFFFu;
            lines_[1u - choice_].color = 0xE0606060u;
            if (pressed(kShoot) && waited_ >= 60) {
                if (onSound) onSound(10);
                return End::Advance;
            }
            if (waited_ >= limit) {
                newBox_ = 1;
                minimum_ = 30;
                return End::Advance;
            }
            ++waited_;
            return End::Stall;
        }
        case 22: {
            const int c = choice_ & 3;
            w.mode = (w.mode & ~static_cast<std::uint32_t>(ModeFinalB)) | (static_cast<std::uint32_t>(c) << 11);
            start(w, c + 1);
            return End::Restart;
        }
        default:
            return End::Advance;
    }
}

// RunMsg (spec 5).
void Dialogue::run(const World& w, std::uint16_t input, std::uint16_t previous) {
    if (index_ < 0) return;
    if (resume_ > 0) --resume_;
    const std::uint8_t* ins = instruction();
    if (skippable_ && (input & kSkip) && ins) timer_.assign(rd16(ins));
    if (w.player && w.player->state() != Player::State::Dead && w.bullets) w.bullets->collectAll();
    bool tick = true;
    for (int guard = 0; guard < 4096; ++guard) {
        ins = instruction();
        if (!ins) {
            index_ = -1;
            w.dialogue = false;
            return;
        }
        if (!(timer_.current >= static_cast<std::int32_t>(rd16(ins)))) break;
        const End end = execute(w, ins, input, previous);
        if (end == End::Advance) {
            ip_ += 4u + ins[3];
            continue;
        }
        if (end == End::Stall) {
            tick = false;
            break;
        }
        if (end == End::Return) return;
        // Restart: the new script from its first instruction, this frame.
        if (index_ < 0) return;
    }
    if (tick) timer_.tick();
    if (w.rng) {
        for (AnmVm& vm : faces_) vm.tick(*w.rng);
        for (AnmVm& vm : lines_) vm.tick(*w.rng);
        vm6_.tick(*w.rng);
        vm7_.tick(*w.rng);
    }
    if (timer_.current < 60 && skippable_ && (input & kSkip)) timer_.assign(60);
}

// The MSG layer (spec 8): faces (the listener behind the speaker), the box, the lines.
void Dialogue::draw(SpriteSink& sink, const UiArt* ui) {
    if (index_ < 0) return;
    const AnmDrawState none;
    auto pair = [&](int a, int b) {
        AnmVm& va = faces_[static_cast<std::size_t>(a)];
        AnmVm& vb = faces_[static_cast<std::size_t>(b)];
        if (va.position.z >= vb.position.z) {
            drawVm(sink, va, none);
            drawVm(sink, vb, none);
        } else {
            drawVm(sink, vb, none);
            drawVm(sink, va, none);
        }
    };
    pair(0, 1);
    pair(2, 3);
    if (boxVisible_ && ui) {
        const float h = timer_.current < 60 ? static_cast<float>(timer_.current * 48 / 60) : 48.0f;
        if (h > 0.0f) {
            // A vertical gradient from 0xD0000000 to 0x90000000, in four bands.
            for (int i = 0; i < 4; ++i) {
                const float y0 = 384.0f + h * static_cast<float>(i) / 4.0f;
                const auto alpha = static_cast<std::uint8_t>(0xD0 - (0xD0 - 0x90) * (2 * i + 1) / 8);
                sink.fillRect(ui->white, 48.0f, y0, 352.0f, h / 4.0f, rgba(0, 0, 0, alpha));
            }
        }
    }
    drawVm(sink, lines_[0], none);
    drawVm(sink, lines_[1], none);
    drawVm(sink, vm6_, none);
    drawVm(sink, vm7_, none);
}

}  // namespace touhou::th08
