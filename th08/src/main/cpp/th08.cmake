# Imperishable Night's game code, on top of the shared engine's core (include
# engine/native/engine.cmake first). Used by the app's native build and by the host unit
# tests. It grows system by system as the clean-room specs (claude/th08-specs) are
# implemented.
#   th08_game   the player's files (th08.exe 1.00d, the version file), th08.cfg, the ANM
#               animation system (version 3), and later the TH08 systems. No SDL.
# Th08Game.cpp (the game as the host runs it) and main.cpp are compiled into the app.

set(TH08_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

add_library(th08_game STATIC
  ${TH08_SRC}/th08/Anm.cpp
  ${TH08_SRC}/th08/GameConfig.cpp
  ${TH08_SRC}/th08/GameFiles.cpp
  ${TH08_SRC}/th08/Version.cpp)
target_include_directories(th08_game PUBLIC "${TH08_SRC}")
target_link_libraries(th08_game PUBLIC thengine_core)
set_target_properties(th08_game PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # IN's simulation is single-precision arithmetic in a fixed order (x87): no fused
  # multiply-add, which ARM compilers would otherwise form.
  target_compile_options(th08_game PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
