#pragma once

// OpenGL ES 3.0: instanced drawing, gl_VertexID, VAOs and non-power-of-two textures are
// all core, which is everything the sprite batcher needs. Supported by effectively every
// Android device in use (API 18+ with a GLES 3 GPU).
#if defined(TOUHOU_IOS)
// iOS: Apple's OpenGL ES 3.0 framework (deprecated by Apple, still supported).
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#else
#include <GLES3/gl3.h>
#endif

#include <string>

namespace touhou::gl {

GLuint compileProgram(const char* vertexSource, const char* fragmentSource, std::string* error);
const char* errorName(GLenum error);
// Logs and clears pending GL errors; returns true if there were none.
bool check(const char* where);

}  // namespace touhou::gl
