#pragma once

#include <string>

#include "gfx/Gl.hpp"

namespace touhou {

// Offscreen colour buffer. The game always renders at the original 640x480 into one of
// these; the result is then scaled into the letterboxed area of the real screen.
class RenderTarget {
  public:
    bool create(int width, int height, std::string* error);
    void destroy();
    void abandon();

    void bind() const;
    static void bindScreen();
    // The window's own framebuffer: 0 on Android and desktop; SDL's on iOS.
    static void setScreenFramebuffer(GLuint fbo) { screenFramebuffer_ = fbo; }

    GLuint texture() const { return texture_; }
    int width() const { return width_; }
    int height() const { return height_; }

  private:
    static inline GLuint screenFramebuffer_ = 0;
    GLuint framebuffer_ = 0;
    GLuint depth_ = 0;
    GLuint texture_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace touhou
