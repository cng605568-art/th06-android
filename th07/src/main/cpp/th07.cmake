# Perfect Cherry Blossom's game code, on top of the shared engine's core (include
# engine/native/engine.cmake first). Used by the app's native build and by the host unit
# tests. It grows system by system as the clean-room specs are implemented.
#   th07_game   music (thbgm.dat + thbgm.fmt), the version file. No SDL.
# Th07Game.cpp (the game as the host runs it) and main.cpp are compiled into the app.

set(TH07_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

add_library(th07_game STATIC
  ${TH07_SRC}/th07/Bgm.cpp
  ${TH07_SRC}/th07/Version.cpp)
target_include_directories(th07_game PUBLIC "${TH07_SRC}")
target_link_libraries(th07_game PUBLIC thengine_core)
set_target_properties(th07_game PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(th07_game PRIVATE -Wall -Wextra)
endif()
