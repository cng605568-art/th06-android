# Platform-independent engine core, shared by the Android build and the host unit tests.
# Defines:
#   th06_zstd   Zstandard decompressor (static, from the pinned zstd release)
#   th06_core   archives (PBG3/LZSS, PKGL/zstd), VFS, image/DDS decoding, the ANM VM, the
#               ECL VM with enemies/bullets/player, sound-effect scheduling. No SDL, no GL.
# Requires TH06_THIRD_PARTY to point at the directory populated by scripts/fetch_deps.sh.

if(NOT EXISTS "${TH06_THIRD_PARTY}/zstd/lib/zstd.h" OR NOT EXISTS "${TH06_THIRD_PARTY}/include/stb_image.h")
  message(FATAL_ERROR "Third-party sources missing in ${TH06_THIRD_PARTY}. Run: bash scripts/fetch_deps.sh")
endif()

set(TH06_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")

file(GLOB TH06_ZSTD_SOURCES
  "${TH06_THIRD_PARTY}/zstd/lib/common/*.c"
  "${TH06_THIRD_PARTY}/zstd/lib/decompress/*.c")
if(TH06_ZSTD_WITH_COMPRESSOR)
  # Tests only: real compressed frames for the New Classic archive round-trip.
  file(GLOB TH06_ZSTD_COMPRESS_SOURCES "${TH06_THIRD_PARTY}/zstd/lib/compress/*.c")
  list(APPEND TH06_ZSTD_SOURCES ${TH06_ZSTD_COMPRESS_SOURCES})
endif()
add_library(th06_zstd STATIC ${TH06_ZSTD_SOURCES})
target_include_directories(th06_zstd PUBLIC "${TH06_THIRD_PARTY}/zstd/lib")
# C-only decoder: skip the x86-64 assembly fast path so every ABI builds the same way.
target_compile_definitions(th06_zstd PRIVATE ZSTD_DISABLE_ASM=1)
set_target_properties(th06_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)

add_library(th06_core STATIC
  ${TH06_SRC}/core/Log.cpp
  ${TH06_SRC}/core/Sha256.cpp
  ${TH06_SRC}/io/Archive.cpp
  ${TH06_SRC}/io/Lzss.cpp
  ${TH06_SRC}/io/MappedFile.cpp
  ${TH06_SRC}/io/Pbg3Archive.cpp
  ${TH06_SRC}/io/PkglArchive.cpp
  ${TH06_SRC}/io/Vfs.cpp
  ${TH06_SRC}/gfx/Anm.cpp
  ${TH06_SRC}/gfx/AnmManager.cpp
  ${TH06_SRC}/gfx/Image.cpp
  ${TH06_SRC}/gfx/ProceduralArt.cpp
  ${TH06_SRC}/gfx/TextureLibrary.cpp
  ${TH06_SRC}/audio/SoundPlayer.cpp
  ${TH06_SRC}/audio/MusicPlayer.cpp
  ${TH06_SRC}/game/BulletManager.cpp
  ${TH06_SRC}/game/Ecl.cpp
  ${TH06_SRC}/game/EnemyManager.cpp
  ${TH06_SRC}/game/ItemManager.cpp
  ${TH06_SRC}/game/Resources.cpp
  ${TH06_SRC}/game/ShotData.cpp
  ${TH06_SRC}/game/Player.cpp
  ${TH06_SRC}/game/Bomb.cpp
  ${TH06_SRC}/game/ScreenShake.cpp
  ${TH06_SRC}/game/EffectManager.cpp
  ${TH06_SRC}/game/Gui.cpp
  ${TH06_SRC}/game/StageBackground.cpp
  ${TH06_SRC}/game/StageMenu.cpp
  ${TH06_SRC}/game/ScoreFile.cpp
  ${TH06_SRC}/game/ResultScreen.cpp
  ${TH06_SRC}/game/Ending.cpp
  ${TH06_SRC}/game/Replay.cpp
  ${TH06_SRC}/game/Translation.cpp
  ${TH06_SRC}/core/Json.cpp
  ${TH06_SRC}/game/Title.cpp
  ${TH06_SRC}/game/MusicRoom.cpp
  ${TH06_SRC}/input/DragPad.cpp
  ${TH06_SRC}/game/GameConfig.cpp
  ${TH06_SRC}/gfx/TextRenderer.cpp
  ${TH06_SRC}/game/Stage.cpp
  ${TH06_SRC}/third_party_impl.cpp)
target_include_directories(th06_core PUBLIC "${TH06_SRC}")
target_include_directories(th06_core SYSTEM PUBLIC "${TH06_THIRD_PARTY}/include")
target_compile_features(th06_core PUBLIC cxx_std_20)
target_link_libraries(th06_core PUBLIC th06_zstd)
set_target_properties(th06_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(th06_core PRIVATE -Wall -Wextra)
  set_source_files_properties(${TH06_SRC}/third_party_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(ANDROID)
  target_link_libraries(th06_core PUBLIC log)
endif()
