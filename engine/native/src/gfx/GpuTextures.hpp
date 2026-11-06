#pragma once

#include <cstdint>
#include <vector>

#include "gfx/Gl.hpp"

namespace touhou {

class TextureLibrary;

// GL texture objects for every TextureLibrary entry, index-aligned with it.
class GpuTextures {
  public:
    // Uploads entries that have no GL texture yet. Entries that fail to build get a
    // visible magenta placeholder instead of breaking the frame.
    void sync(const TextureLibrary& library);
    GLuint get(std::uint16_t id) const;

    void destroy();  // delete GL objects (context alive)
    void abandon();  // context was lost: forget the handles without touching GL

  private:
    GLuint placeholder();

    std::vector<GLuint> textures_;
    std::vector<std::uint32_t> versions_;  // uploaded version of each (dynamic textures change)
    GLuint placeholder_ = 0;
};

}  // namespace touhou
