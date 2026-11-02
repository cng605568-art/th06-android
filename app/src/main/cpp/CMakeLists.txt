# Native build, driven by Gradle's externalNativeBuild on Android (AGP passes the NDK
# toolchain, ABI and TH06_THIRD_PARTY). Produces libSDL2.so and libmain.so, the two
# libraries GameActivity loads.
#
# The same file also builds a Linux desktop executable for development
# (needs libgles-dev + an EGL driver):
#   cmake -S app/src/main/cpp -B build-desktop && cmake --build build-desktop -j
#   ./build-desktop/th06_desktop /path/to/game/folder
cmake_minimum_required(VERSION 3.22)
project(th06port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TH06_THIRD_PARTY)
  get_filename_component(TH06_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party" ABSOLUTE)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/th06core.cmake)

# ---- SDL2 from the pinned source release ---------------------------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${TH06_THIRD_PARTY}/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/SDL2")

# ---- engine: SDL2 + OpenGL ES 3 -------------------------------------------------------
set(TH06_ENGINE_SOURCES
  src/main.cpp
  src/engine/App.cpp
  src/audio/AudioMixer.cpp
  src/input/InputMapper.cpp
  src/input/VirtualController.cpp
  src/gfx/Gl.cpp
  src/gfx/GpuTextures.cpp
  src/gfx/RenderTarget.cpp
  src/gfx/SpriteBatch.cpp
  src/platform/CrashHandler.cpp
  src/platform/DataMount.cpp)

if(ANDROID)
  # SDLActivity loads "libmain.so" and calls SDL_main() in it.
  add_library(main SHARED ${TH06_ENGINE_SOURCES} src/platform/AndroidStorage.cpp)
  target_link_libraries(main PRIVATE th06_core SDL2::SDL2 GLESv3 EGL android log)
  target_link_options(main PRIVATE -Wl,--gc-sections)
  set(TH06_ENGINE_TARGET main)
else()
  find_path(GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
  find_library(GLES_LIBRARY NAMES GLESv2 libGLESv2.so.2 REQUIRED)
  add_executable(th06_desktop ${TH06_ENGINE_SOURCES})
  target_include_directories(th06_desktop PRIVATE "${GLES3_INCLUDE_DIR}")
  target_link_libraries(th06_desktop PRIVATE th06_core SDL2::SDL2 "${GLES_LIBRARY}")
  set(TH06_ENGINE_TARGET th06_desktop)
endif()

target_include_directories(${TH06_ENGINE_TARGET} PRIVATE src)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${TH06_ENGINE_TARGET} PRIVATE -Wall -Wextra)
endif()
