# ac6net - AC6 recomp online core library (no rex dependencies).
#
# Standalone build (unit tests, no game):
#   cmake -S netplay/core -B netplay/core/build-<name> -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   ninja -C netplay/core/build-<name> && netplay/core/build-<name>/ac6net_tests
#
# Inside the AC6 build the top-level CMakeLists adds this directory and links `ac6net` into the
# SDK runtime (REX_HAVE_AC6NET=1).
cmake_minimum_required(VERSION 3.25)
project(ac6net LANGUAGES C CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AC6NET_BUILD_TESTS "Build ac6net unit tests" ${PROJECT_IS_TOP_LEVEL})

file(GLOB AC6NET_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)
file(GLOB AC6NET_THIRD_PARTY_C CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kcp/*.c
)

add_library(ac6net STATIC ${AC6NET_SOURCES} ${AC6NET_THIRD_PARTY_C})
set_target_properties(ac6net PROPERTIES POSITION_INDEPENDENT_CODE ON)
# src/ is deliberately not an include directory: its headers are included as siblings ("x.h"),
# and src/qos.h would otherwise shadow the Windows SDK <qos.h> that <winsock2.h> includes.
target_include_directories(ac6net
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
  # MSVC ABI: std::thread lives in the CRT. FindThreads would only misdetect a "pthreads"
  # library when the toolchain's try_compile does not link (CMAKE_TRY_COMPILE_TARGET_TYPE).
else()
  find_package(Threads REQUIRED)
  target_link_libraries(ac6net PUBLIC Threads::Threads)
endif()
if(WIN32)
  target_link_libraries(ac6net PUBLIC ws2_32 iphlpapi bcrypt winmm)
  # getenv/fopen are fine here; the UCRT's "_s" deprecation warnings are noise.
  target_compile_definitions(ac6net PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# GNU-style drivers (gcc, clang, clang targeting MSVC). clang-cl has the MSVC front-end variant
# and would read -Wall as /Wall (-Weverything).
set(AC6NET_GNU_DRIVER OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(AC6NET_GNU_DRIVER ON)
endif()
if(AC6NET_GNU_DRIVER)
  # The parent project builds with -w; keep our own code warning-clean when standalone.
  if(PROJECT_IS_TOP_LEVEL)
    target_compile_options(ac6net PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
  endif()
endif()

if(AC6NET_BUILD_TESTS)
  file(GLOB AC6NET_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(ac6net_tests ${AC6NET_TEST_SOURCES})
  target_include_directories(ac6net_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  )
  # The tests include src/ headers with quotes. -iquote keeps src/qos.h from shadowing the
  # Windows SDK <qos.h> (pulled in by <winsock2.h>); compilers without it get a plain -I.
  if(AC6NET_GNU_DRIVER)
    target_compile_options(ac6net_tests PRIVATE "SHELL:-iquote ${CMAKE_CURRENT_SOURCE_DIR}/src")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(ac6net_tests PRIVATE "SHELL:/clang:-iquote${CMAKE_CURRENT_SOURCE_DIR}/src")
  else()
    target_include_directories(ac6net_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  endif()
  if(WIN32)
    target_compile_definitions(ac6net_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  target_link_libraries(ac6net_tests PRIVATE ac6net)
endif()
