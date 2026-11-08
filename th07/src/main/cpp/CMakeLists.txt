# Native build of the th07 app, driven by Gradle's externalNativeBuild on Android (AGP
# passes the NDK toolchain, ABI and TOUHOU_THIRD_PARTY). Produces libSDL2.so and
# libmain.so: the shared engine (engine/native/engine.cmake) with Perfect Cherry Blossom's
# game code. (The Android app module comes with the shared launcher library.)
#
# The same file also builds a Linux desktop executable for development
# (needs libgles-dev + an EGL driver):
#   cmake -S th07/src/main/cpp -B build-desktop && cmake --build build-desktop -j
#   ./build-desktop/th07_desktop /path/to/game/folder
cmake_minimum_required(VERSION 3.22)
project(th07port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(TOUHOU_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TOUHOU_THIRD_PARTY)
  set(TOUHOU_THIRD_PARTY "${TOUHOU_ROOT}/third_party")
endif()

# ---- SDL2 from the pinned source release ---------------------------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${TOUHOU_THIRD_PARTY}/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/SDL2")

# PCB's simulation needs single-precision arithmetic in the original's order, so nothing in
# this build may fuse multiply-adds (ARM compilers would by default).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-ffp-contract=off)
endif()

# ---- the shared engine (core + SDL host) and the game ----------------------------------
include("${TOUHOU_ROOT}/engine/native/engine.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/th07.cmake")

set(TH07_APP_SOURCES
  src/main.cpp
  src/th07/Th07Game.cpp)

if(ANDROID)
  # SDLActivity loads "libmain.so" and calls SDL_main() in it.
  add_library(main SHARED ${TH07_APP_SOURCES})
  target_link_options(main PRIVATE -Wl,--gc-sections)
  set(TH07_APP_TARGET main)
else()
  add_executable(th07_desktop ${TH07_APP_SOURCES})
  set(TH07_APP_TARGET th07_desktop)
endif()

target_link_libraries(${TH07_APP_TARGET} PRIVATE th07_game thengine_host)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${TH07_APP_TARGET} PRIVATE -Wall -Wextra)
endif()
