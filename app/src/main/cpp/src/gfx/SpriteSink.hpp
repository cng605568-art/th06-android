#pragma once

#include <cstdint>

namespace th06 {

// GPU-agnostic drawing interface. Game code emits SpriteInstances; the GLES backend
// (SpriteBatch) turns runs of them into instanced draw calls. Keeping this free of GL
// lets the whole simulation run in host unit tests.

// Render state of one sprite, mirroring the original engine's Direct3D 8 states:
//   framebuffer blend  Alpha    SRCBLEND=SRCALPHA, DESTBLEND=INVSRCALPHA
//                      Additive SRCBLEND=SRCALPHA, DESTBLEND=ONE
//   texture stage 0    (default) COLOROP=MODULATE  rgb = tex.rgb * colour.rgb
//                      *ColorAdd COLOROP=ADD       rgb = tex.rgb + colour.rgb
//   alpha is always tex.a * colour.a (ALPHAOP=MODULATE).
enum class Blend : std::uint8_t { Alpha, Additive, AlphaColorAdd, AdditiveColorAdd };

constexpr Blend makeBlend(bool additive, bool colorAdd) {
    return additive ? (colorAdd ? Blend::AdditiveColorAdd : Blend::Additive)
                    : (colorAdd ? Blend::AlphaColorAdd : Blend::Alpha);
}

constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                             std::uint8_t a = 255) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}

// One textured quad. Layout is the GPU per-instance vertex format (40 bytes).
struct SpriteInstance {
    float x = 0, y = 0;    // centre, in target pixels (y down)
    float w = 0, h = 0;    // size in pixels
    float u0 = 0, v0 = 0;  // texture rect (normalised); (u0,v0) maps to the top-left corner
    float u1 = 1, v1 = 1;
    std::uint32_t color = rgba(255, 255, 255);  // tint, bytes R,G,B,A
    float angle = 0;       // radians, clockwise on screen
};
static_assert(sizeof(SpriteInstance) == 40, "SpriteInstance must match the GPU layout");

// One corner of a 3D quad, already projected: clip-space position for the whole target
// (perspective-correct texturing comes from w), texture coordinates, colour and the
// eye-space depth used for fog (0 = not fogged).
struct Vertex3D {
    float x = 0, y = 0, z = 0, w = 1;
    float u = 0, v = 0;
    std::uint32_t color = rgba(255, 255, 255);  // bytes R,G,B,A
    float eyeZ = 0;
};

// Render state of a 3D quad (the stage background): depth tested against other 3D quads
// in the 0.5..1 depth range, wrapping bilinear texturing, linear fog, clipped to a
// rectangle of the target.
struct Draw3DState {
    bool depthWrite = true;
    float fogStart = 1000.0f, fogEnd = 2000.0f;
    std::uint32_t fogColor = rgba(0, 0, 0);
    float clipX = 0, clipY = 0, clipW = 0, clipH = 0;  // target pixels, y down
    bool operator==(const Draw3DState&) const = default;
};

// A rectangle inside one texture.
struct SpriteRegion {
    std::uint16_t texture = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float width = 0, height = 0;  // native size in pixels
};

class SpriteSink {
  public:
    virtual ~SpriteSink() = default;
    virtual void draw(std::uint16_t texture, Blend blend, const SpriteInstance& sprite) = 0;
    // A projected quad, corners in order top-left, top-right, bottom-left, bottom-right.
    // Sinks without 3D support ignore it.
    virtual void drawQuad3D(std::uint16_t texture, Blend blend, const Vertex3D* corners, const Draw3DState& state) {
        (void)texture, (void)blend, (void)corners, (void)state;
    }

    // Convenience: draw a region centred at (x, y) at native size times `scale`.
    void drawRegion(const SpriteRegion& r, float x, float y, float scale = 1.0f,
                    std::uint32_t color = rgba(255, 255, 255), float angle = 0.0f,
                    Blend blend = Blend::Alpha) {
        SpriteInstance s;
        s.x = x;
        s.y = y;
        s.w = r.width * scale;
        s.h = r.height * scale;
        s.u0 = r.u0;
        s.v0 = r.v0;
        s.u1 = r.u1;
        s.v1 = r.v1;
        s.color = color;
        s.angle = angle;
        draw(r.texture, blend, s);
    }

    // Solid rectangle using a white texel region, top-left anchored.
    void fillRect(const SpriteRegion& white, float x, float y, float w, float h,
                  std::uint32_t color, Blend blend = Blend::Alpha) {
        SpriteInstance s;
        s.x = x + w * 0.5f;
        s.y = y + h * 0.5f;
        s.w = w;
        s.h = h;
        s.u0 = s.u1 = (white.u0 + white.u1) * 0.5f;
        s.v0 = s.v1 = (white.v0 + white.v1) * 0.5f;
        s.color = color;
        draw(white.texture, blend, s);
    }
};

}  // namespace th06
