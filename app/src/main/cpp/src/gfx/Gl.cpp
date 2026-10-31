#include "gfx/Gl.hpp"

#include <vector>

#include "core/Log.hpp"

namespace th06::gl {
namespace {

GLuint compile(GLenum type, const char* source, std::string* error) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length > 1 ? length : 1));
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        if (error) {
            *error = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") +
                     " shader: " + log.data();
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

GLuint compileProgram(const char* vertexSource, const char* fragmentSource, std::string* error) {
    const GLuint vs = compile(GL_VERTEX_SHADER, vertexSource, error);
    if (!vs) return 0;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSource, error);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length > 1 ? length : 1));
        glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
        if (error) *error = std::string("link: ") + log.data();
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

const char* errorName(GLenum error) {
    switch (error) {
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        default: return "unknown GL error";
    }
}

bool check(const char* where) {
    bool clean = true;
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
        log::error("GL error at {}: {} (0x{:x})", where, errorName(e), e);
        clean = false;
    }
    return clean;
}

}  // namespace th06::gl
