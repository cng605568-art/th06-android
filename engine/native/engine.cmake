# The shared engine: one copy of everything that is not a particular game, used by every
# game's native build and by the host unit tests. Requires TOUHOU_THIRD_PARTY to point at
# the directory populated by scripts/fetch_deps.sh.
#
# Defines:
#   thengine_zstd   Zstandard decompressor (static, from the pinned zstd release)
#   thengine_core   platform-independent: archives (PBG3/LZSS, PKGL/zstd), the VFS, image
#                   and DDS decoding, the ANM VM, texture library, music streaming, the
#                   JSON reader and translation patch stack, touch-drag maths, logging.
#                   No SDL, no GL: it builds and runs in the unit tests.
# and, when SDL2::SDL2 exists (a game's build, after add_subdirectory(SDL2)):
#   thengine_host   the SDL host that runs a game (host/App + host/Game): window, OpenGL
#                   ES 3 rendering, audio mixer, gamepad/keyboard/touch input, crash
#                   handler, data mounting and the Android JNI glue.
# A game adds its own static library on top of thengine_core and links thengine_host
# into its executable (desktop) or libmain.so (Android).

if(NOT EXISTS "${TOUHOU_THIRD_PARTY}/zstd/lib/zstd.h" OR NOT EXISTS "${TOUHOU_THIRD_PARTY}/include/stb_image.h")
  message(FATAL_ERROR "Third-party sources missing in ${TOUHOU_THIRD_PARTY}. Run: bash scripts/fetch_deps.sh")
endif()

set(THENGINE_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

file(GLOB THENGINE_ZSTD_SOURCES
  "${TOUHOU_THIRD_PARTY}/zstd/lib/common/*.c"
  "${TOUHOU_THIRD_PARTY}/zstd/lib/decompress/*.c")
if(THENGINE_ZSTD_WITH_COMPRESSOR)
  # Tests only: real compressed frames for the New Classic archive round-trip.
  file(GLOB THENGINE_ZSTD_COMPRESS_SOURCES "${TOUHOU_THIRD_PARTY}/zstd/lib/compress/*.c")
  list(APPEND THENGINE_ZSTD_SOURCES ${THENGINE_ZSTD_COMPRESS_SOURCES})
endif()
add_library(thengine_zstd STATIC ${THENGINE_ZSTD_SOURCES})
target_include_directories(thengine_zstd PUBLIC "${TOUHOU_THIRD_PARTY}/zstd/lib")
# C-only decoder: skip the x86-64 assembly fast path so every ABI builds the same way.
target_compile_definitions(thengine_zstd PRIVATE ZSTD_DISABLE_ASM=1)
set_target_properties(thengine_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)

add_library(thengine_core STATIC
  ${THENGINE_SRC}/core/Json.cpp
  ${THENGINE_SRC}/core/Log.cpp
  ${THENGINE_SRC}/core/Sha256.cpp
  ${THENGINE_SRC}/io/Archive.cpp
  ${THENGINE_SRC}/io/Lzss.cpp
  ${THENGINE_SRC}/io/MappedFile.cpp
  ${THENGINE_SRC}/io/PeImage.cpp
  ${THENGINE_SRC}/io/Pbg3Archive.cpp
  ${THENGINE_SRC}/io/Pbg4Archive.cpp
  ${THENGINE_SRC}/io/PbgzArchive.cpp
  ${THENGINE_SRC}/io/ThCrypt.cpp
  ${THENGINE_SRC}/io/PkglArchive.cpp
  ${THENGINE_SRC}/io/Vfs.cpp
  ${THENGINE_SRC}/gfx/Anm.cpp
  ${THENGINE_SRC}/gfx/AnmManager.cpp
  ${THENGINE_SRC}/gfx/Image.cpp
  ${THENGINE_SRC}/gfx/ProceduralArt.cpp
  ${THENGINE_SRC}/gfx/TextRenderer.cpp
  ${THENGINE_SRC}/gfx/TextureLibrary.cpp
  ${THENGINE_SRC}/audio/MusicPlayer.cpp
  ${THENGINE_SRC}/input/DragPad.cpp
  ${THENGINE_SRC}/text/PatchStack.cpp
  ${THENGINE_SRC}/third_party_impl.cpp)
target_include_directories(thengine_core PUBLIC "${THENGINE_SRC}")
target_include_directories(thengine_core SYSTEM PUBLIC "${TOUHOU_THIRD_PARTY}/include")
target_compile_features(thengine_core PUBLIC cxx_std_20)
target_link_libraries(thengine_core PUBLIC thengine_zstd)
set_target_properties(thengine_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(thengine_core PRIVATE -Wall -Wextra)
  set_source_files_properties(${THENGINE_SRC}/third_party_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(ANDROID)
  target_link_libraries(thengine_core PUBLIC log)
endif()
if(TOUHOU_IOS)
  target_compile_definitions(thengine_core PUBLIC TOUHOU_IOS=1)
endif()

if(TARGET SDL2::SDL2)
  add_library(thengine_host STATIC
    ${THENGINE_SRC}/host/App.cpp
    ${THENGINE_SRC}/audio/AudioMixer.cpp
    ${THENGINE_SRC}/input/InputMapper.cpp
    ${THENGINE_SRC}/input/VirtualController.cpp
    ${THENGINE_SRC}/gfx/Gl.cpp
    ${THENGINE_SRC}/gfx/GpuTextures.cpp
    ${THENGINE_SRC}/gfx/RenderTarget.cpp
    ${THENGINE_SRC}/gfx/SpriteBatch.cpp
    ${THENGINE_SRC}/platform/CrashHandler.cpp
    ${THENGINE_SRC}/platform/DataMount.cpp)
  target_link_libraries(thengine_host PUBLIC thengine_core SDL2::SDL2)
  set_target_properties(thengine_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(ANDROID)
    target_sources(thengine_host PRIVATE ${THENGINE_SRC}/platform/AndroidStorage.cpp)
    target_link_libraries(thengine_host PUBLIC GLESv3 EGL android log)
  elseif(TOUHOU_IOS)
    # iOS: Apple's OpenGL ES, and the Settings app, Files folder, Core Text and haptics.
    target_sources(thengine_host PRIVATE ${THENGINE_SRC}/platform/IosPlatform.mm)
    set_source_files_properties(${THENGINE_SRC}/platform/IosPlatform.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(thengine_host PUBLIC "-framework OpenGLES" "-framework UIKit" "-framework Foundation"
                          "-framework CoreText" "-framework CoreGraphics" "-framework QuartzCore")
  else()
    find_path(GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
    find_library(GLES_LIBRARY NAMES GLESv2 libGLESv2.so.2 REQUIRED)
    target_include_directories(thengine_host PUBLIC "${GLES3_INCLUDE_DIR}")
    target_link_libraries(thengine_host PUBLIC "${GLES_LIBRARY}")
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(thengine_host PRIVATE -Wall -Wextra)
  endif()
endif()
