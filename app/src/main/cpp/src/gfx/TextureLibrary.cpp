#include "gfx/TextureLibrary.hpp"

#include "core/Log.hpp"
#include "io/Vfs.hpp"

namespace th06 {

std::uint16_t TextureLibrary::add(std::string name, int width, int height, Producer producer,
                                  TextureFilter filter) {
    items_.push_back(Item{std::move(name), width, height, std::move(producer), filter});
    return static_cast<std::uint16_t>(items_.size() - 1);
}

std::optional<Image> TextureLibrary::build(std::uint16_t id) const {
    const Item& item = items_.at(id);
    auto img = item.producer();
    if (!img) log::error("Texture {} could not be built", item.name);
    return img;
}

SpriteRegion TextureLibrary::region(std::uint16_t id, float x, float y, float w, float h) const {
    const float tw = static_cast<float>(width(id));
    const float th = static_cast<float>(height(id));
    return SpriteRegion{id, x / tw, y / th, (x + w) / tw, (y + h) / th, w, h};
}

SpriteRegion LoadedAnm::sprite(const TextureLibrary& lib, std::size_t i) const {
    const AnmSprite& s = anm.sprites.at(i);
    return lib.region(texture, s.x, s.y, s.w, s.h);
}

namespace {

std::string resolveImage(const Vfs& vfs, const std::string& name) {
    if (vfs.exists(name)) return name;
    const auto dot = name.find_last_of('.');
    const std::string dds = (dot == std::string::npos ? name : name.substr(0, dot)) + ".dds";
    if (vfs.exists(dds)) return dds;
    return {};
}

std::optional<Image> decodeFromVfs(const Vfs& vfs, const std::string& path) {
    auto bytes = vfs.read(path);
    if (!bytes) return std::nullopt;
    std::string err;
    auto img = decodeImage(*bytes, &err);
    if (!img) log::error("{}: {}", path, err);
    return img;
}

}  // namespace

std::optional<LoadedAnm> loadAnm(const Vfs& vfs, TextureLibrary& lib, std::string_view path,
                                 std::string* error) {
    auto bytes = vfs.read(path);
    if (!bytes) {
        if (error) *error = std::string(path) + " not found";
        return std::nullopt;
    }
    auto anm = parseAnm(*bytes, error);
    if (!anm) return std::nullopt;
    if (anm->imageName.starts_with('@')) {
        if (error) *error = std::string(path) + " draws into a render target, not an image";
        return std::nullopt;
    }

    const std::string image = resolveImage(vfs, anm->imageName);
    if (image.empty()) {
        if (error) *error = std::string(path) + ": texture " + anm->imageName + " not found";
        return std::nullopt;
    }
    const std::string alpha = anm->alphaName.empty() ? std::string() : resolveImage(vfs, anm->alphaName);

    int w = anm->textureWidth;
    int h = anm->textureHeight;
    if (w <= 0 || h <= 0) {  // header without a size: take it from the image
        auto img = decodeFromVfs(vfs, image);
        if (!img) {
            if (error) *error = std::string(path) + ": cannot decode " + image;
            return std::nullopt;
        }
        w = img->width;
        h = img->height;
    }

    const std::uint32_t colorKey = anm->colorKey;
    const Vfs* v = &vfs;
    LoadedAnm out;
    out.texture = lib.add(image, w, h, [v, image, alpha, colorKey, w, h]() -> std::optional<Image> {
        auto img = decodeFromVfs(*v, image);
        if (!img) return std::nullopt;
        if (!alpha.empty()) {
            if (auto mask = decodeFromVfs(*v, alpha)) applyAlphaFromMask(*img, *mask);
        }
        applyColorKey(*img, colorKey);
        // Smaller than the header says: the original placed it top-left in a padded
        // texture. Equal or larger (New Classic high-res art): normalised UVs still hold.
        if (img->width < w || img->height < h) *img = padTo(*img, w, h);
        return img;
    });
    out.anm = std::move(*anm);
    log::info("Loaded {}: {} sprites on {} ({}x{})", path, out.anm.sprites.size(), image, w, h);
    return out;
}

}  // namespace th06
