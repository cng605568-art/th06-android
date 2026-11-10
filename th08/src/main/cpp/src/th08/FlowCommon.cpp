#include <algorithm>
#include <cstring>

#include "gfx/Image.hpp"
#include "gfx/ProceduralArt.hpp"
#include "gfx/SpriteSink.hpp"
#include "gfx/TextureLibrary.hpp"
#include "io/Vfs.hpp"
#include "th08/Flow.hpp"
#include "th08/SoundPlayer.hpp"

namespace touhou::th08 {

// ---- input ----------------------------------------------------------------------------------

void MenuInput::sample(std::uint16_t raw) {
    previous_ = current_;
    current_ = raw;
    repeat_ = false;
    if (current_ != previous_) {
        counter_ = 0;
        return;
    }
    if (counter_ >= 30) {
        if (counter_ % 8 == 0) repeat_ = true;
        if (counter_ >= 38) counter_ = 30;
    }
    ++counter_;
}

void FlowContext::playSound(int id) const {
    if (sound && id >= 0) sound->playSound(id);
}

// ---- backgrounds ----------------------------------------------------------------------------

bool Surface::load(const Vfs& vfs, TextureLibrary& textures, const std::string& path) {
    texture_ = -1;
    path_ = path;
    const auto bytes = vfs.read(path);
    if (!bytes) return false;
    std::string err;
    const auto img = decodeImage(*bytes, &err);
    if (!img) return false;
    width_ = img->width;
    height_ = img->height;
    const Vfs* v = &vfs;
    texture_ = textures.add("th08-surface:" + path, width_, height_, [v, path]() -> std::optional<Image> {
        std::string e;
        const auto b = v->read(path);
        return b ? decodeImage(*b, &e) : std::nullopt;
    });
    return true;
}

void Surface::blit(SpriteSink& sink, float x, float y) const {
    if (texture_ < 0 || width_ <= 0 || height_ <= 0) return;
    SpriteInstance s;
    s.x = x + static_cast<float>(width_) * 0.5f;
    s.y = y + static_cast<float>(height_) * 0.5f;
    s.w = static_cast<float>(width_);
    s.h = static_cast<float>(height_);
    sink.draw(static_cast<std::uint16_t>(texture_), Blend::Alpha, s);
}

// ---- ASCII strings --------------------------------------------------------------------------

void AsciiText::add(float x, float y, std::string_view text, std::uint32_t color, float scale, float advance, float xScale) {
    if (items_.size() >= 256) return;
    items_.push_back({x, y, std::string(text.substr(0, 63)), color, scale, advance, xScale < 0.0f ? scale : xScale});
}

void AsciiText::draw(SpriteSink& sink, AnmLibrary& anm) {
    AnmVm glyph;
    glyph.file = anm.file(kSlotAscii);
    const AnmDrawState none;
    for (const Item& t : items_) {
        float x = t.x, y = t.y;
        for (unsigned char c : t.text) {
            if (c == '\n') {
                y += 16.0f * t.scale;
                x = t.x;
                continue;
            }
            if (c != ' ' && c > 0 && glyph.selectSprite(c - 1)) {
                glyph.flags = AnmVm::Visible | AnmVm::Active | AnmVm::AnchorLeft | AnmVm::AnchorTop;
                glyph.color = t.color;
                glyph.scale = {t.xScale, t.scale};
                glyph.position = {x, y, 0.0f};
                drawVm(sink, glyph, none);
            }
            x += t.advance * t.scale;
        }
    }
    items_.clear();
}

// ---- text into sprites ----------------------------------------------------------------------

void renderSpriteText(TextureLibrary* textures, TextRenderer* text, AnmVm& vm, std::string_view sjis, std::uint32_t colorRef,
                      TextAlign align) {
    vm.set(AnmVm::Visible, true);
    const AnmSprite* sp = vm.sprite;
    if (!sp || !textures) return;
    Image* img = textures->pixels(sp->texture);
    if (!img) return;
    const int size = vm.fontWidth != 0 ? vm.fontWidth : 15;
    const int top = static_cast<int>(sp->startY);
    Image band(img->width, 16);
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(sjis.data()), sjis.size());
    const float len = static_cast<float>(sjis.size()), fw = static_cast<float>(size);
    const float width = sp->pixelWidth * sp->cols;
    float x = sp->startX;
    if (align == TextAlign::Right) x = width + sp->startX - len * fw * sp->cols / 2.0f;
    if (align == TextAlign::Centre) x = width / 2.0f + sp->startX - len * fw * sp->cols / 4.0f;
    if (text && !sjis.empty()) {
        TextStyle style;
        style.fontSize = size;
        style.rgb = ((colorRef & 0xFF) << 16) | (colorRef & 0xFF00) | ((colorRef >> 16) & 0xFF);
        text->render(bytes, style, std::max(0.0f, x), band);
        finishTextBand(band);
    }
    for (int y = 0; y < 16 && top + y < img->height; ++y) {
        if (top + y < 0) continue;
        std::memcpy(img->pixel(0, top + y), band.pixel(0, y), static_cast<std::size_t>(img->width) * 4);
    }
    textures->touch(sp->texture);
}

void fillScreen(SpriteSink& sink, const UiArt* ui, std::uint32_t argb) {
    if (!ui || (argb >> 24) == 0) return;
    sink.fillRect(ui->white, 0.0f, 0.0f, 640.0f, 480.0f, argbToRgbaBytes(argb));
}

}  // namespace touhou::th08
