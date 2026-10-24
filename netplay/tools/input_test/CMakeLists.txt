# Unit tests for the netplay test harness's script engine (src/netplay_input_script.cpp).
# No game, SDK or network needed:
#   cmake -S netplay/tools/input_test -B <build-dir> -G Ninja
#   ninja -C <build-dir> && <build-dir>/netplay_input_test
cmake_minimum_required(VERSION 3.25)
project(netplay_input_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")
add_executable(netplay_input_test
  netplay_input_test.cpp
  "${APP_SRC}/netplay_input_script.cpp"
)
target_include_directories(netplay_input_test PRIVATE "${APP_SRC}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(netplay_input_test PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  option(NETPLAY_INPUT_TEST_UBSAN "Build the tests with UBSan" ON)
  if(NETPLAY_INPUT_TEST_UBSAN)
    target_compile_options(netplay_input_test PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(netplay_input_test PRIVATE -fsanitize=undefined)
  endif()
endif()
