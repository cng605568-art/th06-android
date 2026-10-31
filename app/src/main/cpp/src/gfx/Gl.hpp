#pragma once

// OpenGL ES 3.0: instanced drawing, gl_VertexID, VAOs and non-power-of-two textures are
// all core, which is everything the sprite batcher needs. Supported by effectively every
// Android device in use (API 18+ with a GLES 3 GPU).
#include <GLES3/gl3.h>

#include <string>

namespace th06::gl {

GLuint compileProgram(const char* vertexSource, const char* fragmentSource, std::string* error);
const char* errorName(GLenum error);
// Logs and clears pending GL errors; returns true if there were none.
bool check(const char* where);

}  // namespace th06::gl
