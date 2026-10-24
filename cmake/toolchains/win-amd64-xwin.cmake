# Cross toolchain: Windows x64 (MSVC ABI) from macOS or Linux.
#
# clang/clang++ in GNU driver mode with --target=x86_64-pc-windows-msvc (the same compiler
# front end upstream's windows-base preset uses on Windows), lld-link, llvm-rc/llvm-mt, and
# a Windows SDK + MSVC CRT splatted by xwin (https://github.com/Jake-Shadle/xwin):
#
#   cargo install xwin
#   xwin --accept-license splat --output <XWIN_DIR>
#
# Inputs (cache variables or environment variables of the same name):
#   XWIN_DIR       xwin splat root (contains crt/ and sdk/). Required.
#   REX_LLVM_BIN   directory with clang, clang++, llvm-rc, llvm-mt, llvm-ar, llvm-ranlib.
#                  Defaults: /opt/homebrew/opt/llvm@21/bin, /usr/lib/llvm-21/bin, PATH.
#   REX_LLD_BIN    directory with lld-link. Defaults: REX_LLVM_BIN,
#                  /opt/homebrew/opt/lld@21/bin, /usr/lib/llvm-21/bin, PATH.
#   REX_MSVC_COMPAT_VERSION  value for -fms-compatibility-version (default 19.44, which is
#                  the VS 2022 17.14 CRT xwin downloads by default).
#
# See netplay/docs/BUILDING_WINDOWS.md.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# try_compile() re-reads this file in a fresh project; carry the inputs over.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    XWIN_DIR REX_LLVM_BIN REX_LLD_BIN REX_MSVC_COMPAT_VERSION)

foreach(_rex_var XWIN_DIR REX_LLVM_BIN REX_LLD_BIN REX_MSVC_COMPAT_VERSION)
    if(NOT ${_rex_var} AND DEFINED ENV{${_rex_var}})
        set(${_rex_var} "$ENV{${_rex_var}}")
    endif()
endforeach()

if(NOT XWIN_DIR)
    message(FATAL_ERROR "win-amd64-xwin toolchain: set XWIN_DIR to the xwin splat root "
        "(the directory with crt/ and sdk/), e.g. -DXWIN_DIR=$HOME/.xwin")
endif()
get_filename_component(XWIN_DIR "${XWIN_DIR}" ABSOLUTE)
if(NOT EXISTS "${XWIN_DIR}/crt/include" OR NOT EXISTS "${XWIN_DIR}/sdk/include/um")
    message(FATAL_ERROR "win-amd64-xwin toolchain: ${XWIN_DIR} is not an xwin splat root "
        "(expected crt/include and sdk/include/um)")
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat root (crt/ and sdk/)")

if(NOT REX_MSVC_COMPAT_VERSION)
    set(REX_MSVC_COMPAT_VERSION 19.44)
endif()

find_program(REX_XWIN_CLANG NAMES clang clang-21 NAMES_PER_DIR
    HINTS ${REX_LLVM_BIN} /opt/homebrew/opt/llvm@21/bin /usr/lib/llvm-21/bin
    NO_CACHE REQUIRED)
# NAMES_PER_DIR + HINTS: the LLVM bin dir itself (Homebrew opt/, /usr/lib/llvm-21/bin) wins
# over a versioned /usr/bin/clang-21 symlink, so the sibling clang++, llvm-rc, llvm-mt and
# llvm-ar below come from the same LLVM.
get_filename_component(_rex_llvm_bin "${REX_XWIN_CLANG}" DIRECTORY)
if(NOT EXISTS "${_rex_llvm_bin}/clang++")
    message(FATAL_ERROR "win-amd64-xwin toolchain: no clang++ next to ${REX_XWIN_CLANG}; "
        "set REX_LLVM_BIN to the LLVM bin directory")
endif()
if(NOT REX_LLVM_BIN)
    set(REX_LLVM_BIN "${_rex_llvm_bin}")
endif()

find_program(REX_XWIN_LLD_LINK NAMES lld-link
    HINTS ${REX_LLD_BIN} ${REX_LLVM_BIN} /opt/homebrew/opt/lld@21/bin /usr/lib/llvm-21/bin
    NO_CACHE REQUIRED)
get_filename_component(_rex_lld_bin "${REX_XWIN_LLD_LINK}" DIRECTORY)

set(_rex_target x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER_TARGET ${_rex_target})
set(CMAKE_CXX_COMPILER_TARGET ${_rex_target})
set(CMAKE_ASM_COMPILER_TARGET ${_rex_target})

# Clang's own resource headers (immintrin.h, stdint.h, ...) must be found before the MSVC
# copies in crt/include, exactly as clang orders them on Windows (resource dir first, then
# %INCLUDE%). -isystem keeps that order; the duplicate built-in entry is dropped.
execute_process(COMMAND "${REX_XWIN_CLANG}" -print-resource-dir
    OUTPUT_VARIABLE _rex_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)

set(_rex_sysflags
    --target=${_rex_target}
    -fms-compatibility-version=${REX_MSVC_COMPAT_VERSION}
    -isystem "${_rex_resource_dir}/include"
    -isystem "${XWIN_DIR}/crt/include"
    -isystem "${XWIN_DIR}/sdk/include/ucrt"
    -isystem "${XWIN_DIR}/sdk/include/um"
    -isystem "${XWIN_DIR}/sdk/include/shared"
    -isystem "${XWIN_DIR}/sdk/include/winrt"
)

# Path-independent output: no absolute host paths in __FILE__ / std::source_location
# strings or debug info (-ffile-prefix-map = -fmacro-prefix-map + -fdebug-prefix-map).
# The source tree (this file lives in <root>/cmake/toolchains) maps to ".", the build tree
# to "build" when it lives outside the source tree, the xwin splat to "xwin" and clang's
# resource dir to "clang". Derived at configure time, so nothing here names a user's home.
get_filename_component(_rex_src_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_rex_prefix_maps "${_rex_src_root}=." "${XWIN_DIR}=xwin" "${_rex_resource_dir}=clang")
if(CMAKE_BINARY_DIR)
    get_filename_component(_rex_bin_root "${CMAKE_BINARY_DIR}" ABSOLUTE)
    string(FIND "${_rex_bin_root}/" "${_rex_src_root}/" _rex_bin_in_src)
    string(FIND "${_rex_src_root}/" "${_rex_bin_root}/" _rex_src_in_bin)
    if(NOT _rex_bin_in_src EQUAL 0 AND NOT _rex_src_in_bin EQUAL 0)
        list(APPEND _rex_prefix_maps "${_rex_bin_root}=build")
    endif()
endif()
foreach(_rex_map IN LISTS _rex_prefix_maps)
    # Map the real path too when the spelled one goes through a symlink.
    string(REGEX REPLACE "=[^=]*$" "" _rex_map_from "${_rex_map}")
    string(REGEX REPLACE "^.*=" "" _rex_map_to "${_rex_map}")
    file(REAL_PATH "${_rex_map_from}" _rex_map_real)
    list(APPEND _rex_sysflags "-ffile-prefix-map=${_rex_map_from}=${_rex_map_to}")
    if(NOT _rex_map_real STREQUAL _rex_map_from)
        list(APPEND _rex_sysflags "-ffile-prefix-map=${_rex_map_real}=${_rex_map_to}")
    endif()
endforeach()

# The system flags ride on the compiler command itself (CMake splits a list-valued
# CMAKE_<LANG>_COMPILER into the compiler and CMAKE_<LANG>_COMPILER_ARG1), so presets and
# users stay free to set CMAKE_<LANG>_FLAGS (upstream's -march=x86-64-v3 -w) without losing
# them. The same arguments reach the llvm-rc preprocessing step.
set(CMAKE_C_COMPILER "${_rex_llvm_bin}/clang" ${_rex_sysflags})
set(CMAKE_CXX_COMPILER "${_rex_llvm_bin}/clang++" ${_rex_sysflags})
set(CMAKE_ASM_COMPILER "${_rex_llvm_bin}/clang" ${_rex_sysflags})

set(CMAKE_RC_COMPILER "${_rex_llvm_bin}/llvm-rc" CACHE FILEPATH "Resource compiler")
set(CMAKE_MT "${_rex_llvm_bin}/llvm-mt" CACHE FILEPATH "Manifest tool")
set(CMAKE_AR "${_rex_llvm_bin}/llvm-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${_rex_llvm_bin}/llvm-ranlib" CACHE FILEPATH "Ranlib")
set(CMAKE_LINKER "${REX_XWIN_LLD_LINK}" CACHE FILEPATH "Linker")

# Link through the clang driver (CMake's Windows-Clang GNU-frontend rules) with lld-link.
# -B lets the driver find lld-link when it lives outside the LLVM bin dir (Homebrew lld@21).
set(_rex_linkflags
    "-fuse-ld=lld -B${_rex_lld_bin}/"
    "-L${XWIN_DIR}/crt/lib/x86_64"
    "-L${XWIN_DIR}/sdk/lib/um/x86_64"
    "-L${XWIN_DIR}/sdk/lib/ucrt/x86_64"
    # The exe's debug directory names only <name>.pdb, not the build machine's path.
    "-Wl,/pdbaltpath:%_PDB%"
)
list(JOIN _rex_linkflags " " _rex_linkflags)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_rex_linkflags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_rex_linkflags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_rex_linkflags}")

# Never pick up host (macOS/Linux) libraries or headers.
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
