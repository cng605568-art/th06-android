// Single translation unit holding the implementations of the header-only libraries.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include <stb_image.h>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>
