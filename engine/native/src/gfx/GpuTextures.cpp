#include "gfx/GpuTextures.hpp"

#include "core/Log.hpp"
#include "gfx/TextureLibrary.hpp"

namespace touhou {
namespace {

GLuint upload(int w, int h, const void* rgba, TextureFilter filter) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint f = filter == TextureFilter::Linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    // Game art wraps, as Direct3D's default texture addressing did: scrolling ANM sprites
    // (spell-card backgrounds, laser beams) push their UVs past 1 and must tile, not smear
    // the edge texels. The port's own smoothly scaled UI art keeps its edges.
    const GLint wrap = filter == TextureFilter::Linear ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

}  // namespace

void GpuTextures::sync(const TextureLibrary& library) {
    GLint maxSize = 2048;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    while (textures_.size() < library.size()) {
        const auto id = static_cast<std::uint16_t>(textures_.size());
        auto image = library.build(id);
        GLuint tex = 0;
        if (image && image->width <= maxSize && image->height <= maxSize) {
            tex = upload(image->width, image->height, image->rgba.data(), library.filter(id));
            log::debug("Uploaded texture {} ({}x{})", library.name(id), image->width, image->height);
        } else if (image) {
            log::error("Texture {} is {}x{}, above this GPU's limit of {}", library.name(id),
                       image->width, image->height, maxSize);
        }
        textures_.push_back(tex);
        versions_.push_back(library.version(id));
    }
    // Textures the game draws into (rendered text) are sent again when they change.
    for (std::size_t id = 0; id < textures_.size(); ++id) {
        const auto tid = static_cast<std::uint16_t>(id);
        if (versions_[id] == library.version(tid) || !textures_[id]) continue;
        versions_[id] = library.version(tid);
        if (auto image = library.build(tid)) {
            glBindTexture(GL_TEXTURE_2D, textures_[id]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->width, image->height, GL_RGBA, GL_UNSIGNED_BYTE,
                            image->rgba.data());
        }
    }
    gl::check("GpuTextures::sync");
}

GLuint GpuTextures::get(std::uint16_t id) const {
    if (id < textures_.size() && textures_[id] != 0) return textures_[id];
    return const_cast<GpuTextures*>(this)->placeholder();
}

GLuint GpuTextures::placeholder() {
    if (!placeholder_) {
        const std::uint8_t magenta[4] = {255, 0, 255, 255};
        placeholder_ = upload(1, 1, magenta, TextureFilter::Nearest);
    }
    return placeholder_;
}

void GpuTextures::destroy() {
    for (GLuint t : textures_) {
        if (t) glDeleteTextures(1, &t);
    }
    if (placeholder_) glDeleteTextures(1, &placeholder_);
    abandon();
}

void GpuTextures::abandon() {
    textures_.clear();
    versions_.clear();
    placeholder_ = 0;
}

}  // namespace touhou
