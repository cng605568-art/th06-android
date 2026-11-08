# Host unit tests for the shared engine's core and each game's code (th06, th07); run in CI
# before the APK build.
#   bash scripts/fetch_deps.sh
#   cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Release && cmake --build build-tests -j
#   ctest --test-dir build-tests --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(touhou_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(TOUHOU_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/../third_party" ABSOLUTE)
set(THENGINE_ZSTD_WITH_COMPRESSOR ON)
include(${CMAKE_CURRENT_LIST_DIR}/../engine/native/engine.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../th06/src/main/cpp/th06.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../th07/src/main/cpp/th07.cmake)

add_executable(th06_tests test_main.cpp)
target_link_libraries(th06_tests PRIVATE th06_game)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(th06_tests PRIVATE -Wall -Wextra)
endif()

add_executable(th07_tests th07_tests.cpp)
target_link_libraries(th07_tests PRIVATE th07_game)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(th07_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME th06_tests COMMAND th06_tests)
add_test(NAME th07_tests COMMAND th07_tests)
