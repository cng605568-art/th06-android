#include "gfx/SpriteBatch.hpp"

#include <cstddef>

#include "gfx/GpuTextures.hpp"

namespace th06 {
namespace {

constexpr const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec4 aRect;    // centre x, y, width, height (pixels)
layout(location = 1) in vec4 aUv;      // u0, v0, u1, v1
layout(location = 2) in vec4 aColor;   // normalised RGBA8
layout(location = 3) in float aAngle;  // radians
uniform vec4 uTransform;               // pixel -> clip: scale.xy, offset.xy
out vec2 vUv;
out vec4 vColor;
void main() {
    // Triangle-strip corners 0..3 = (0,0) (1,0) (0,1) (1,1).
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 local = (corner - 0.5) * aRect.zw;
    float c = cos(aAngle);
    float s = sin(aAngle);
    vec2 pos = aRect.xy + vec2(local.x * c - local.y * s, local.x * s + local.y * c);
    gl_Position = vec4(pos * uTransform.xy + uTransform.zw, 0.0, 1.0);
    vUv = mix(aUv.xy, aUv.zw, corner);
    vColor = aColor;
}
)";

constexpr const char* kFragmentShader = R"(#version 300 es
precision mediump float;
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTexture;
uniform float uColorAdd;  // 0: D3DTOP_MODULATE, 1: D3DTOP_ADD
out vec4 fragColor;
void main() {
    vec4 tex = texture(uTexture, vUv);
    vec3 rgb = mix(tex.rgb * vColor.rgb, min(tex.rgb + vColor.rgb, vec3(1.0)), uColorAdd);
    fragColor = vec4(rgb, tex.a * vColor.a);
}
)";

}  // namespace

bool SpriteBatch::create(std::string* error) {
    program_ = gl::compileProgram(kVertexShader, kFragmentShader, error);
    if (!program_) return false;
    uTransform_ = glGetUniformLocation(program_, "uTransform");
    uTexture_ = glGetUniformLocation(program_, "uTexture");
    uColorAdd_ = glGetUniformLocation(program_, "uColorAdd");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, kMaxInstances * sizeof(SpriteInstance), nullptr, GL_STREAM_DRAW);

    constexpr GLsizei stride = sizeof(SpriteInstance);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(SpriteInstance, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(SpriteInstance, u0)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                          reinterpret_cast<const void*>(offsetof(SpriteInstance, color)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(SpriteInstance, angle)));
    for (GLuint i = 0; i < 4; ++i) glVertexAttribDivisor(i, 1);
    glBindVertexArray(0);

    pending_.reserve(kMaxInstances);
    return gl::check("SpriteBatch::create");
}

void SpriteBatch::destroy() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    abandon();
}

void SpriteBatch::abandon() {
    vbo_ = vao_ = program_ = 0;
    pending_.clear();
    currentTexture_ = 0;
}

void SpriteBatch::begin(float targetWidth, float targetHeight, const GpuTextures* textures) {
    textures_ = textures;
    pending_.clear();
    currentTexture_ = 0;
    glUseProgram(program_);
    glUniform4f(uTransform_, 2.0f / targetWidth, -2.0f / targetHeight, -1.0f, 1.0f);
    glUniform1i(uTexture_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
}

void SpriteBatch::draw(std::uint16_t texture, Blend blend, const SpriteInstance& sprite) {
    drawTexture(textures_->get(texture), blend, sprite);
}

void SpriteBatch::drawTexture(GLuint texture, Blend blend, const SpriteInstance& sprite) {
    if (!pending_.empty() &&
        (texture != currentTexture_ || blend != currentBlend_ || pending_.size() == kMaxInstances)) {
        flush();
    }
    currentTexture_ = texture;
    currentBlend_ = blend;
    pending_.push_back(sprite);
}

void SpriteBatch::flush() {
    if (pending_.empty()) return;
    glBindTexture(GL_TEXTURE_2D, currentTexture_);
    const bool additive = currentBlend_ == Blend::Additive || currentBlend_ == Blend::AdditiveColorAdd;
    const bool colorAdd = currentBlend_ == Blend::AlphaColorAdd || currentBlend_ == Blend::AdditiveColorAdd;
    if (additive) {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
    } else {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    glUniform1f(uColorAdd_, colorAdd ? 1.0f : 0.0f);
    const auto bytes = static_cast<GLsizeiptr>(pending_.size() * sizeof(SpriteInstance));
    // Orphan the buffer so the driver never stalls on a draw still reading the old data.
    glBufferData(GL_ARRAY_BUFFER, kMaxInstances * sizeof(SpriteInstance), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, pending_.data());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(pending_.size()));
    ++drawCalls_;
    pending_.clear();
}

void SpriteBatch::end() {
    flush();
    glBindVertexArray(0);
}

std::uint32_t SpriteBatch::takeDrawCalls() {
    const std::uint32_t n = drawCalls_;
    drawCalls_ = 0;
    return n;
}

}  // namespace th06
