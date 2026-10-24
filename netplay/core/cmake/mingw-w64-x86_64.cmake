# Cross-compile ac6net for Windows x86-64 with MinGW-w64 (portability check only).
#   cmake -S netplay/core -B out/ac6net-mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=netplay/core/cmake/mingw-w64-x86_64.cmake -DAC6NET_BUILD_TESTS=ON
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_mingw_prefix x86_64-w64-mingw32)
find_program(CMAKE_C_COMPILER NAMES ${_mingw_prefix}-gcc PATHS /opt/homebrew/bin /usr/bin REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_mingw_prefix}-g++ PATHS /opt/homebrew/bin /usr/bin REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_mingw_prefix}-windres PATHS /opt/homebrew/bin /usr/bin)
find_program(CMAKE_AR NAMES ${_mingw_prefix}-ar PATHS /opt/homebrew/bin /usr/bin REQUIRED)
find_program(CMAKE_RANLIB NAMES ${_mingw_prefix}-ranlib PATHS /opt/homebrew/bin /usr/bin REQUIRED)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Static runtime so the test exe runs under Wine without the MinGW DLLs on PATH.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
