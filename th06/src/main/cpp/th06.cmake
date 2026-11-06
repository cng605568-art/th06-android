# Embodiment of Scarlet Devil's game code, on top of the shared engine's core (include
# engine/native/engine.cmake first). Used by the app's native build and by the host unit
# tests.
#   th06_game   the stage and its systems (ECL, enemies, bullets, items, player, bombs,
#               effects, background, GUI), the title screens, menus, results, ending,
#               score.dat, th06.cfg, replays, translations and sound effects. No SDL.
# Th06Game.cpp (the game as the host runs it) and main.cpp are compiled into the app.

set(TH06_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

add_library(th06_game STATIC
  ${TH06_SRC}/th06/Bomb.cpp
  ${TH06_SRC}/th06/BulletManager.cpp
  ${TH06_SRC}/th06/Ecl.cpp
  ${TH06_SRC}/th06/EffectManager.cpp
  ${TH06_SRC}/th06/Ending.cpp
  ${TH06_SRC}/th06/EnemyManager.cpp
  ${TH06_SRC}/th06/GameConfig.cpp
  ${TH06_SRC}/th06/Gui.cpp
  ${TH06_SRC}/th06/ItemManager.cpp
  ${TH06_SRC}/th06/MusicRoom.cpp
  ${TH06_SRC}/th06/Player.cpp
  ${TH06_SRC}/th06/Replay.cpp
  ${TH06_SRC}/th06/Resources.cpp
  ${TH06_SRC}/th06/ResultScreen.cpp
  ${TH06_SRC}/th06/ScoreFile.cpp
  ${TH06_SRC}/th06/ScreenShake.cpp
  ${TH06_SRC}/th06/ShotData.cpp
  ${TH06_SRC}/th06/SoundPlayer.cpp
  ${TH06_SRC}/th06/Stage.cpp
  ${TH06_SRC}/th06/StageBackground.cpp
  ${TH06_SRC}/th06/StageMenu.cpp
  ${TH06_SRC}/th06/Title.cpp
  ${TH06_SRC}/th06/Translation.cpp)
target_include_directories(th06_game PUBLIC "${TH06_SRC}")
target_link_libraries(th06_game PUBLIC thengine_core)
set_target_properties(th06_game PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(th06_game PRIVATE -Wall -Wextra)
endif()
