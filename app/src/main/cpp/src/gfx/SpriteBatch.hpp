#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/Gl.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {

class GpuTextures;

// Instanced sprite renderer. Every sprite is one 40-byte instance; the quad corners come
// from gl_VertexID, so there is no per-vertex buffer at all. Consecutive sprites that
// share a texture and blend mode go out in a single glDrawArraysInstanced call: a screen
// with thousands of bullets from one sheet costs one draw call.
class SpriteBatch final : public SpriteSink {
  public:
    static constexpr std::size_t kMaxInstances = 16384;

    bool create(std::string* error);
    void destroy();
    void abandon();

    // Starts a pass into a target of the given pixel size (y down, origin top-left).
    void begin(float targetWidth, float targetHeight, const GpuTextures* textures);
    void draw(std::uint16_t texture, Blend blend, const SpriteInstance& sprite) override;
    void drawTexture(GLuint texture, Blend blend, const SpriteInstance& sprite);
    void end();

    std::uint32_t takeDrawCalls();

  private:
    void flush();

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLint uTransform_ = -1;
    GLint uTexture_ = -1;
    GLint uColorAdd_ = -1;
    std::vector<SpriteInstance> pending_;
    GLuint currentTexture_ = 0;
    Blend currentBlend_ = Blend::Alpha;
    const GpuTextures* textures_ = nullptr;
    std::uint32_t drawCalls_ = 0;
};

}  // namespace th06
