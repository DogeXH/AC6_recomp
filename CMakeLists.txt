# ac6recomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ac6recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT AC6RECOMP_IPO_SUPPORTED OUTPUT AC6RECOMP_IPO_ERROR)

# Keep reconfigure passes pinned to the vendored SDK when the cache is empty.
if(NOT DEFINED REXSDK_DIR OR REXSDK_DIR STREQUAL "")
    set(_ac6_vendored_rexsdk "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk")
    if(EXISTS "${_ac6_vendored_rexsdk}/CMakeLists.txt")
        set(REXSDK_DIR "${_ac6_vendored_rexsdk}" CACHE PATH "Path to rexglue-sdk source tree" FORCE)
    endif()
endif()

# Windows: MSVC STL 14.40+ (VS 2022 17.10+) gives std::mutex a constexpr constructor whose
# object layout only msvcp140.dll 14.40+ understands; with an older VC++ runtime (an old
# redistributable, or Wine/Proton's builtin msvcp140) the first lock deadlocks or crashes.
# Microsoft's documented switch keeps the old, runtime-initialised layout that every
# msvcp140.dll handles. Set before the SDK so the SDK and its third-party code get it.
if(WIN32)
    set(AC6_WIN_MUTEX_COMPAT_DEFINE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    add_compile_definitions(${AC6_WIN_MUTEX_COMPAT_DEFINE})
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    include(generated/rexglue.cmake)
else()
    include(cmake/rexglue_bootstrap.cmake)
endif()

if(WIN32)
    # The define only changes std::mutex's constructor, and the generated guest code never
    # constructs a mutex (it only locks the SDK's): keep it off generated/ so its large TUs keep
    # their compile command. The app's own sources and ac6net get it per source/target below.
    get_property(_ac6_dir_defs DIRECTORY PROPERTY COMPILE_DEFINITIONS)
    list(REMOVE_ITEM _ac6_dir_defs ${AC6_WIN_MUTEX_COMPAT_DEFINE})
    set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "${_ac6_dir_defs}")
endif()

# Sources
set(AC6RECOMP_SOURCES
    src/main.cpp
    src/ac6_pac_decode_dump.cpp
    src/ac6_pac_decoder_probe.cpp
    src/ac6_pac_index.cpp
    src/d3d_hooks.cpp
    src/render_hooks.cpp
    src/ac6_native_renderer2/native_renderer.cpp
    src/ac6_native_graphics.cpp
    src/ac6_native_graphics_overlay.cpp
    src/ac6_backend_fixes/ac6_backend_capture_bridge.cpp
    src/ac6_backend_fixes/ac6_backend_hooks.cpp
    src/ac6_backend_fixes/ac6_backend_pass_classifier.cpp
    src/ac6_backend_fixes/ac6_cutscene_resync.cpp
    src/ac6_backend_fixes/ac6_effect_mode_fix.cpp
    src/ac6_backend_fixes/ac6_fps_physics_fix.cpp
    src/ac6_backend_fixes/ac6_fullres_effects.cpp
    src/ac6_backend_fixes/ac6_world_msaa.cpp
    src/ac6_backend_fixes/ac6_kbm_input.cpp
)

if(WIN32)
    list(APPEND AC6RECOMP_SOURCES
        src/ac6_texture_overrides.cpp
        src/ac6_backend_fixes/ac6_widescreen.cpp
    )
    add_executable(ac6recomp WIN32 ${AC6RECOMP_SOURCES})
else()
    list(APPEND AC6RECOMP_SOURCES src/ac6_backend_fixes/ac6_widescreen_stub.cpp)
    add_executable(ac6recomp ${AC6RECOMP_SOURCES})
endif()

rexglue_setup_target(ac6recomp)

# AC6 online (ac6net core library, netplay/core). The vendored SDK's XAM glue builds into the
# static rexkernel and kernel_state.cpp into rexsystem; a PRIVATE link on a static library still
# carries ac6net to the executable. Inert unless the runtime config sets [netplay] enabled = true.
option(AC6_NETPLAY "Compile the AC6 online glue into the vendored SDK" ON)
if(AC6_NETPLAY)
    add_subdirectory(netplay/core)
    # Test harness (scripted pad, in-process screenshots, repl). Inert unless
    # netplay_input_script or netplay_screenshot_dir is set; see netplay/docs/TESTING.md.
    target_sources(ac6recomp PRIVATE src/netplay_input_driver.cpp src/netplay_input_script.cpp)
    target_compile_definitions(ac6recomp PRIVATE AC6_NETPLAY_HARNESS=1)
    foreach(_ac6net_target rexkernel rexsystem)
        if(TARGET ${_ac6net_target})
            target_link_libraries(${_ac6net_target} PRIVATE ac6net)
            target_compile_definitions(${_ac6net_target} PRIVATE REX_HAVE_AC6NET=1)
        endif()
    endforeach()
endif()

if(WIN32)
    get_target_property(_ac6_own_sources ac6recomp SOURCES)
    list(FILTER _ac6_own_sources EXCLUDE REGEX "/generated/")
    set_property(SOURCE ${_ac6_own_sources} APPEND PROPERTY COMPILE_DEFINITIONS
        ${AC6_WIN_MUTEX_COMPAT_DEFINE})
    if(TARGET ac6net)
        target_compile_definitions(ac6net PRIVATE ${AC6_WIN_MUTEX_COMPAT_DEFINE})
    endif()
endif()

# Link libmspack for AC6 PAC LZX decompression in the runtime dump path.
target_link_libraries(ac6recomp PRIVATE mspack)

if(AC6RECOMP_IPO_SUPPORTED)
    set_property(TARGET ac6recomp PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET ac6recomp PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
    message(STATUS "IPO/LTO disabled for ac6recomp: ${AC6RECOMP_IPO_ERROR}")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ac6recomp PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-fomit-frame-pointer>
    )
endif()

# M3 slice 2: headless native-draw trace harness (ac6-nr2-trace-draw).
# Compiles the nr2 module a second time with AC6_NR2_TRACE_TOOL (which drops
# the guest Present hook, so the recompiled guest-code objects are not needed)
# and drives it from a TracePlayer-based main against a captured GPU trace.
# Requires the SDK trace tools (TracePlayer lives in rexgraphics only when
# REXGLUE_BUILD_TRACE_TOOLS is ON, as it is in the deck build tree).
if(NOT WIN32 AND REXGLUE_BUILD_TRACE_TOOLS)
    add_executable(ac6-nr2-trace-draw
        src/ac6_native_renderer2/trace_draw_main.cpp
        src/ac6_native_renderer2/native_renderer.cpp
        # Same SDK->app diagnostics stub the rexgpu-vulkan-trace-dump tool uses
        # (ac6::backend::ReportSwapDecision no-op).
        thirdparty/rexglue-sdk/src/graphics/trace_replay_stubs.cpp
    )
    target_compile_definitions(ac6-nr2-trace-draw PRIVATE AC6_NR2_TRACE_TOOL=1)
    target_include_directories(ac6-nr2-trace-draw PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
    )
    target_link_libraries(ac6-nr2-trace-draw PRIVATE rex::graphics rex::kernel rex::audio)
    # kernel_state.cpp dynamic_casts to rex::audio::AudioSystem; force the
    # typeinfo object out of librexaudio despite archive scan order (same as
    # the SDK's trace tool target).
    target_link_options(ac6-nr2-trace-draw PRIVATE "-Wl,-u,_ZTIN3rex5audio11AudioSystemE")
endif()
