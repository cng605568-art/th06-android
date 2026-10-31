#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/Anm.hpp"
#include "gfx/Image.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {

class Vfs;

enum class TextureFilter : std::uint8_t { Nearest, Linear };

// CPU-side registry of every texture the game uses. Each entry keeps a producer that
// (re)builds its pixels: decoding from the player's archives on demand, or generating
// them. The GPU backend uploads from here at start-up and again if Android destroys the
// GL context, so nothing has to keep a second copy of the pixels in RAM.
class TextureLibrary {
  public:
    using Producer = std::function<std::optional<Image>()>;

    // width/height are the dimensions sprite UVs are normalised against. Game art stays
    // pixel-exact (Nearest); generated UI shapes that get scaled use Linear.
    std::uint16_t add(std::string name, int width, int height, Producer producer,
                      TextureFilter filter = TextureFilter::Nearest);

    std::size_t size() const { return items_.size(); }
    const std::string& name(std::uint16_t id) const { return items_.at(id).name; }
    int width(std::uint16_t id) const { return items_.at(id).width; }
    int height(std::uint16_t id) const { return items_.at(id).height; }
    TextureFilter filter(std::uint16_t id) const { return items_.at(id).filter; }
    std::optional<Image> build(std::uint16_t id) const;

    SpriteRegion region(std::uint16_t id, float x, float y, float w, float h) const;

  private:
    struct Item {
        std::string name;
        int width;
        int height;
        Producer producer;
        TextureFilter filter;
    };
    std::vector<Item> items_;
};

// An ANM file whose texture has been registered with a TextureLibrary.
struct LoadedAnm {
    std::uint16_t texture = 0;
    AnmFile anm;

    // Region for the i-th sprite of the file.
    SpriteRegion sprite(const TextureLibrary& lib, std::size_t i) const;
};

// Reads an ANM from the archives, resolves its texture (+ optional alpha mask) and
// registers a producer that decodes it with the original engine's rules: colour key,
// mask alpha from the blue channel, transparent padding to the header size.
// New Classic ships DDS textures; if the named image is absent, "<stem>.dds" is tried.
std::optional<LoadedAnm> loadAnm(const Vfs& vfs, TextureLibrary& lib, std::string_view path,
                                 std::string* error);

}  // namespace th06
