# Host unit tests for the platform-independent core (runs in CI before the APK build).
#   bash scripts/fetch_deps.sh
#   cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Release && cmake --build build-tests -j
#   ctest --test-dir build-tests --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(th06_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(TH06_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/../third_party" ABSOLUTE)
set(TH06_ZSTD_WITH_COMPRESSOR ON)
include(${CMAKE_CURRENT_LIST_DIR}/../app/src/main/cpp/cmake/th06core.cmake)

add_executable(th06_tests test_main.cpp)
target_link_libraries(th06_tests PRIVATE th06_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(th06_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME th06_core_tests COMMAND th06_tests)
