#include "gfx/SpriteBatch.hpp"

#include <cstddef>

#include "gfx/GpuTextures.hpp"

namespace touhou {
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

constexpr const char* kVertexShader3D = R"(#version 300 es
layout(location = 0) in vec4 aPos;     // clip space
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aEyeZ;
out vec2 vUv;
out vec4 vColor;
out float vEyeZ;
void main() {
    gl_Position = aPos;
    vUv = aUv;
    vColor = aColor;
    vEyeZ = aEyeZ;
}
)";

constexpr const char* kFragmentShader3D = R"(#version 300 es
precision highp float;
in vec2 vUv;
in vec4 vColor;
in float vEyeZ;
uniform sampler2D uTexture;
uniform float uColorAdd;
uniform vec2 uFog;        // start, end
uniform vec3 uFogColor;
out vec4 fragColor;
void main() {
    vec4 tex = texture(uTexture, vUv);
    float a = tex.a * vColor.a;
    if (a < 4.0 / 255.0) discard;  // the original's alpha test
    vec3 rgb = mix(tex.rgb * vColor.rgb, min(tex.rgb + vColor.rgb, vec3(1.0)), uColorAdd);
    float f = vEyeZ <= 0.0 ? 1.0 : clamp((uFog.y - vEyeZ) / (uFog.y - uFog.x), 0.0, 1.0);
    fragColor = vec4(mix(uFogColor, rgb, f), a);
}
)";

}  // namespace

bool SpriteBatch::create(std::string* error) {
    program3D_ = gl::compileProgram(kVertexShader3D, kFragmentShader3D, error);
    if (!program3D_) return false;
    u3Texture_ = glGetUniformLocation(program3D_, "uTexture");
    u3ColorAdd_ = glGetUniformLocation(program3D_, "uColorAdd");
    u3Fog_ = glGetUniformLocation(program3D_, "uFog");
    u3FogColor_ = glGetUniformLocation(program3D_, "uFogColor");
    glGenVertexArrays(1, &vao3D_);
    glGenBuffers(1, &vbo3D_);
    glBindVertexArray(vao3D_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo3D_);
    glBufferData(GL_ARRAY_BUFFER, kMax3DVertices * sizeof(Vertex3D), nullptr, GL_STREAM_DRAW);
    {
        constexpr GLsizei stride = sizeof(Vertex3D);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, color)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, eyeZ)));
    }
    glBindVertexArray(0);
    glGenSamplers(1, &sampler3D_);  // background art wraps and is filtered bilinearly
    glSamplerParameteri(sampler3D_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler3D_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler3D_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(sampler3D_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    pending3D_.reserve(kMax3DVertices);

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
    if (vbo3D_) glDeleteBuffers(1, &vbo3D_);
    if (vao3D_) glDeleteVertexArrays(1, &vao3D_);
    if (program3D_) glDeleteProgram(program3D_);
    if (sampler3D_) glDeleteSamplers(1, &sampler3D_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    abandon();
}

void SpriteBatch::abandon() {
    vbo_ = vao_ = program_ = 0;
    vbo3D_ = vao3D_ = program3D_ = sampler3D_ = 0;
    pending3D_.clear();
    pending_.clear();
    currentTexture_ = 0;
}

void SpriteBatch::begin(float targetWidth, float targetHeight, const GpuTextures* textures) {
    textures_ = textures;
    pending_.clear();
    pending3D_.clear();
    currentTexture_ = 0;
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;
    clip2D_ = false;
    bind2D();
}

void SpriteBatch::bind2D() {
    glUseProgram(program_);
    glUniform4f(uTransform_, 2.0f / targetWidth_, -2.0f / targetHeight_, -1.0f, 1.0f);
    glUniform1i(uTexture_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (clip2D_) {
        glEnable(GL_SCISSOR_TEST);
        scissor(clipX_, clipY_, clipW_, clipH_);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glEnable(GL_BLEND);
}

// The target is drawn y-down in pixels; GL's scissor origin is bottom-left.
void SpriteBatch::scissor(float x, float y, float w, float h) {
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    const float sx = static_cast<float>(vp[2]) / targetWidth_, sy = static_cast<float>(vp[3]) / targetHeight_;
    glScissor(vp[0] + static_cast<GLint>(x * sx), vp[1] + static_cast<GLint>((targetHeight_ - y - h) * sy),
              static_cast<GLsizei>(w * sx), static_cast<GLsizei>(h * sy));
}

void SpriteBatch::setClip(float x, float y, float w, float h) {
    flush();
    flush3D();
    clip2D_ = true;
    clipX_ = x;
    clipY_ = y;
    clipW_ = w;
    clipH_ = h;
    glEnable(GL_SCISSOR_TEST);
    scissor(x, y, w, h);
}

void SpriteBatch::clearClip() {
    flush();
    flush3D();
    clip2D_ = false;
    glDisable(GL_SCISSOR_TEST);
}

void SpriteBatch::drawQuad3D(std::uint16_t texture, Blend blend, const Vertex3D* c, const Draw3DState& state) {
    flush();  // keep submission order with the 2D sprites
    const GLuint tex = textures_->get(texture);
    if (!pending3D_.empty() && (tex != texture3D_ || blend != blend3D_ || !(state == state3D_) ||
                                pending3D_.size() + 6 > kMax3DVertices)) {
        flush3D();
    }
    texture3D_ = tex;
    blend3D_ = blend;
    state3D_ = state;
    for (int i : {0, 1, 2, 2, 1, 3}) pending3D_.push_back(c[i]);
}

void SpriteBatch::flush3D() {
    if (pending3D_.empty()) return;
    glUseProgram(program3D_);
    glBindVertexArray(vao3D_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo3D_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture3D_);
    glBindSampler(0, sampler3D_);
    glUniform1i(u3Texture_, 0);
    const bool additive = blend3D_ == Blend::Additive || blend3D_ == Blend::AdditiveColorAdd;
    const bool colorAdd = blend3D_ == Blend::AlphaColorAdd || blend3D_ == Blend::AdditiveColorAdd;
    if (additive) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
    else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(u3ColorAdd_, colorAdd ? 1.0f : 0.0f);
    glUniform2f(u3Fog_, state3D_.fogStart, state3D_.fogEnd);
    const std::uint32_t fc = state3D_.fogColor;
    glUniform3f(u3FogColor_, static_cast<float>(fc & 0xff) / 255.0f, static_cast<float>((fc >> 8) & 0xff) / 255.0f,
                static_cast<float>((fc >> 16) & 0xff) / 255.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(state3D_.depthWrite ? GL_TRUE : GL_FALSE);
    glDepthRangef(0.5f, 1.0f);
    glEnable(GL_SCISSOR_TEST);
    scissor(state3D_.clipX, state3D_.clipY, state3D_.clipW, state3D_.clipH);
    glBufferData(GL_ARRAY_BUFFER, kMax3DVertices * sizeof(Vertex3D), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(pending3D_.size() * sizeof(Vertex3D)), pending3D_.data());
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(pending3D_.size()));
    ++drawCalls_;
    pending3D_.clear();
    glBindSampler(0, 0);
    glDepthMask(GL_TRUE);
    glDepthRangef(0.0f, 1.0f);
    bind2D();
}

void SpriteBatch::draw(std::uint16_t texture, Blend blend, const SpriteInstance& sprite) {
    drawTexture(textures_->get(texture), blend, sprite);
}

void SpriteBatch::drawTexture(GLuint texture, Blend blend, const SpriteInstance& sprite) {
    flush3D();
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
    flush3D();
    glBindVertexArray(0);
}

std::uint32_t SpriteBatch::takeDrawCalls() {
    const std::uint32_t n = drawCalls_;
    drawCalls_ = 0;
    return n;
}

}  // namespace touhou
