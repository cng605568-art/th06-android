# Perfect Cherry Blossom's game code, on top of the shared engine's core (include
# engine/native/engine.cmake first). Used by the app's native build and by the host unit
# tests. It grows system by system as the clean-room specs are implemented.
#   th07_game   the ANM animation system, music (thbgm.dat + thbgm.fmt), sound effects, the
#               tables read from th07.exe, the version file, the stage systems (backgrounds,
#               player, bullets, effects, ECL and enemies), and the game flow (supervisor,
#               title and select screens, score.dat, result screen, ending, Music Room). No SDL.
# Th07Game.cpp (the game as the host runs it) and main.cpp are compiled into the app.

set(TH07_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

add_library(th07_game STATIC
  ${TH07_SRC}/th07/Anm.cpp
  ${TH07_SRC}/th07/Background.cpp
  ${TH07_SRC}/th07/Bgm.cpp
  ${TH07_SRC}/th07/Bombs.cpp
  ${TH07_SRC}/th07/Bullets.cpp
  ${TH07_SRC}/th07/Ecl.cpp
  ${TH07_SRC}/th07/EclEx.cpp
  ${TH07_SRC}/th07/Effects.cpp
  ${TH07_SRC}/th07/Enemies.cpp
  ${TH07_SRC}/th07/ExeTables.cpp
  ${TH07_SRC}/th07/GameConfig.cpp
  ${TH07_SRC}/th07/Gameplay.cpp
  ${TH07_SRC}/th07/Gui.cpp
  ${TH07_SRC}/th07/InGameMenu.cpp
  ${TH07_SRC}/th07/Player.cpp
  ${TH07_SRC}/th07/ScoreFile.cpp
  ${TH07_SRC}/th07/ShotData.cpp
  ${TH07_SRC}/th07/SoundPlayer.cpp
  ${TH07_SRC}/th07/Version.cpp
  ${TH07_SRC}/th07/World.cpp)
target_include_directories(th07_game PUBLIC "${TH07_SRC}")
target_link_libraries(th07_game PUBLIC thengine_core)
set_target_properties(th07_game PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # PCB's simulation is single-precision arithmetic in a fixed order (x87 at 24-bit
  # precision): no fused multiply-add, which ARM compilers would otherwise form.
  target_compile_options(th07_game PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
