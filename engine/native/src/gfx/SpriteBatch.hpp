#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/Gl.hpp"
#include "gfx/SpriteSink.hpp"

namespace touhou {

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
    void drawQuad3D(std::uint16_t texture, Blend blend, const Vertex3D* corners, const Draw3DState& state) override;
    void drawTexture(GLuint texture, Blend blend, const SpriteInstance& sprite);
    void setClip(float x, float y, float w, float h) override;
    void clearClip() override;
    void end();

    std::uint32_t takeDrawCalls();

  private:
    void flush();
    void flush3D();
    void bind2D();
    void scissor(float x, float y, float w, float h);

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
    float targetWidth_ = 1, targetHeight_ = 1;
    bool clip2D_ = false;
    float clipX_ = 0, clipY_ = 0, clipW_ = 0, clipH_ = 0;

    // 3D quads (the stage background).
    static constexpr std::size_t kMax3DVertices = 6 * 4096;
    GLuint program3D_ = 0, vao3D_ = 0, vbo3D_ = 0, sampler3D_ = 0;
    GLint u3Texture_ = -1, u3ColorAdd_ = -1, u3Fog_ = -1, u3FogColor_ = -1;
    std::vector<Vertex3D> pending3D_;
    GLuint texture3D_ = 0;
    Blend blend3D_ = Blend::Alpha;
    Draw3DState state3D_;
};

}  // namespace touhou
