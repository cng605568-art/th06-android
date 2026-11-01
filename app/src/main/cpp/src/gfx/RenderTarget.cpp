#include "gfx/RenderTarget.hpp"

namespace th06 {

bool RenderTarget::create(int width, int height, std::string* error) {
    width_ = width;
    height_ = height;
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    // Depth for the 3D stage background (its quads hide each other by depth).
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) *error = "offscreen framebuffer incomplete";
        destroy();
        return false;
    }
    return true;
}

void RenderTarget::destroy() {
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    if (texture_) glDeleteTextures(1, &texture_);
    abandon();
}

void RenderTarget::abandon() {
    framebuffer_ = 0;
    texture_ = 0;
    depth_ = 0;
}

void RenderTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width_, height_);
}

void RenderTarget::bindScreen() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

}  // namespace th06
