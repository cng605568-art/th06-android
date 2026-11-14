# Native build of the th06 app, driven by Gradle's externalNativeBuild on Android (AGP
# passes the NDK toolchain, ABI and TOUHOU_THIRD_PARTY). Produces libSDL2.so and
# libth06main.so, the two libraries GameActivity loads: the shared engine
# (engine/native/engine.cmake) with Embodiment of Scarlet Devil's game code (th06.cmake).
#
# The same file also builds a Linux desktop executable for development
# (needs libgles-dev + an EGL driver):
#   cmake -S th06/src/main/cpp -B build-desktop && cmake --build build-desktop -j
#   ./build-desktop/th06_desktop /path/to/game/folder
cmake_minimum_required(VERSION 3.22)
project(th06port LANGUAGES C CXX)

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

# ---- the shared engine (core + SDL host) and the game ----------------------------------
include("${TOUHOU_ROOT}/engine/native/engine.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/th06.cmake")

set(TH06_APP_SOURCES
  src/main.cpp
  src/th06/Th06Game.cpp)

if(ANDROID)
  # GameActivity names "libth06main.so" (one name per game, so the three can share Touhou
  # Launcher's APK); SDLActivity loads it and calls SDL_main() in it.
  add_library(th06main SHARED ${TH06_APP_SOURCES})
  target_link_options(th06main PRIVATE -Wl,--gc-sections)
  set(TH06_APP_TARGET th06main)
else()
  add_executable(th06_desktop ${TH06_APP_SOURCES})
  set(TH06_APP_TARGET th06_desktop)
endif()

target_link_libraries(${TH06_APP_TARGET} PRIVATE th06_game thengine_host)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${TH06_APP_TARGET} PRIVATE -Wall -Wextra)
endif()
