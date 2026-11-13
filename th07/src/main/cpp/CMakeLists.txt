# Native build of the th07 app, driven by Gradle's externalNativeBuild on Android (AGP
# passes the NDK toolchain, ABI and TOUHOU_THIRD_PARTY). Produces libSDL2.so and
# libth07main.so: the shared engine (engine/native/engine.cmake) with Perfect Cherry Blossom's
# game code, which GameActivity in the :th07 game library (th07/build.gradle) loads.
#
# The same file also builds a Linux desktop executable for development
# (needs libgles-dev + an EGL driver):
#   cmake -S th07/src/main/cpp -B build-desktop && cmake --build build-desktop -j
#   ./build-desktop/th07_desktop /path/to/game/folder
# and the iOS app (on macOS with Xcode; CI packages it with scripts/package_ios.sh):
#   cmake -S th07/src/main/cpp -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS \
#     -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.3
#   cmake --build build-ios   (-> build-ios/TH07Port.app)
cmake_minimum_required(VERSION 3.22)
project(th07port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(TOUHOU_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TOUHOU_THIRD_PARTY)
  set(TOUHOU_THIRD_PARTY "${TOUHOU_ROOT}/third_party")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(TOUHOU_IOS TRUE)
  enable_language(OBJC OBJCXX)
endif()

# ---- SDL2 from the pinned source release ---------------------------------------------
if(TOUHOU_IOS)
  # iOS: a static SDL2 inside the app, with SDL2main (UIKit's main() calling SDL_main).
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_SDL2MAIN OFF CACHE BOOL "" FORCE)
else()
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
endif()
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${TOUHOU_THIRD_PARTY}/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/SDL2")
if(TOUHOU_IOS AND NOT TARGET SDL2::SDL2 AND TARGET SDL2-static)
  add_library(SDL2::SDL2 ALIAS SDL2-static)
endif()

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
  # GameActivity names "libth07main.so" (one name per game, so the three can share Touhou
  # Launcher's APK); SDLActivity loads it and calls SDL_main() in it.
  add_library(th07main SHARED ${TH07_APP_SOURCES})
  target_link_options(th07main PRIVATE -Wl,--gc-sections)
  set(TH07_APP_TARGET th07main)
elseif(TOUHOU_IOS)
  # The iOS app bundle (unsigned; sideloading tools sign it). Info.plist comes from
  # th07/ios; scripts/package_ios.sh adds Settings.bundle and the icon and makes the .ipa.
  if(NOT TH07_VERSION)
    file(READ "${TOUHOU_ROOT}/VERSION" TH07_VERSION)
    string(STRIP "${TH07_VERSION}" TH07_VERSION)
  endif()
  if(NOT TH07_BUILD)
    set(TH07_BUILD 1)
  endif()
  add_executable(TH07Port MACOSX_BUNDLE ${TH07_APP_SOURCES})
  set_target_properties(TH07Port PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${TOUHOU_ROOT}/th07/ios/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.cng605568art.th07"
    MACOSX_BUNDLE_BUNDLE_NAME "TH07 Port"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${TH07_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${TH07_BUILD}")
  target_link_libraries(TH07Port PRIVATE SDL2::SDL2main)
  set(TH07_APP_TARGET TH07Port)
else()
  add_executable(th07_desktop ${TH07_APP_SOURCES})
  set(TH07_APP_TARGET th07_desktop)
endif()

target_link_libraries(${TH07_APP_TARGET} PRIVATE th07_game thengine_host)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${TH07_APP_TARGET} PRIVATE -Wall -Wextra)
endif()
