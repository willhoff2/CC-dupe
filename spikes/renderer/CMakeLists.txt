# Renderer spike: standalone. Deliberately NOT wired into the top-level build, so
# it cannot affect the Windows game build.
#
#   cmake -S . -B build && cmake --build build && ./build/zh-renderer-spike
#
# Requires: a Vulkan SDK/loader with headers, glslangValidator (or glslc), CMake 3.20+.
# SDL2 is optional; it backs zh-renderer-spike's --window and the portable backend of the
# window/event-loop/input seam (zh-window-spike, see tools/macos-window-check.sh).

cmake_minimum_required(VERSION 3.20)
project(zh-renderer-spike CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Vulkan REQUIRED)

option(SPIKE_USE_SDL2 "Build the on-screen presentation path with SDL2" ON)
set(SPIKE_HAVE_SDL2 OFF)
if(SPIKE_USE_SDL2)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        set(SPIKE_HAVE_SDL2 ON)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2 QUIET sdl2)
            if(SDL2_FOUND)
                set(SPIKE_HAVE_SDL2 ON)
            endif()
        endif()
    endif()
    if(NOT SPIKE_HAVE_SDL2)
        message(STATUS "SDL2 not found: building headless-only (still renders and writes a PNG)")
    endif()
endif()

# --- shaders ------------------------------------------------------------------
find_program(GLSL_COMPILER NAMES glslangValidator glslc)
if(NOT GLSL_COMPILER)
    message(FATAL_ERROR "need glslangValidator or glslc on PATH to compile the shaders")
endif()

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

set(SPIRV_OUTPUTS)
foreach(shader fixedfunc.vert fixedfunc.frag probe.vert probe.frag)
    set(shader_in "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(shader_out "${SHADER_OUTPUT_DIR}/${shader}.spv")
    if(GLSL_COMPILER MATCHES "glslc")
        set(compile_command "${GLSL_COMPILER}" "${shader_in}" -o "${shader_out}")
    else()
        set(compile_command "${GLSL_COMPILER}" -V "${shader_in}" -o "${shader_out}")
    endif()
    add_custom_command(
        OUTPUT "${shader_out}"
        COMMAND ${compile_command}
        DEPENDS "${shader_in}"
        COMMENT "compiling ${shader} to SPIR-V"
        VERBATIM)
    list(APPEND SPIRV_OUTPUTS "${shader_out}")
endforeach()
add_custom_target(spike-shaders DEPENDS ${SPIRV_OUTPUTS})

# --- executable ---------------------------------------------------------------
add_executable(zh-renderer-spike
    src/main.cpp
    src/png_write.cpp
    src/state_translate.cpp
    src/vulkan_backend.cpp)
add_dependencies(zh-renderer-spike spike-shaders)

target_include_directories(zh-renderer-spike PRIVATE src)
target_link_libraries(zh-renderer-spike PRIVATE Vulkan::Vulkan)
target_compile_definitions(zh-renderer-spike PRIVATE
    SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")

# Capability probe: queries only, no rendering. Answers the format/feature questions
# the two-draw spike cannot -- see docs/porting/moltenvk-findings.md.
add_executable(zh-caps-probe src/caps_probe.cpp)
target_link_libraries(zh-caps-probe PRIVATE Vulkan::Vulkan)

# Feature probe: renders each case and checks the pixels it produced.
add_executable(zh-feature-probe src/feature_probe.cpp)
add_dependencies(zh-feature-probe spike-shaders)
target_link_libraries(zh-feature-probe PRIVATE Vulkan::Vulkan)
target_compile_definitions(zh-feature-probe PRIVATE SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")

# Fixed-function tests: drive the backend through the DX8Wrapper-shaped interface
# and assert on read-back pixels. This is the harness the measured feature set is
# validated in.
add_executable(zh-fixedfunc-tests
    src/fixedfunc_tests.cpp
    src/state_translate.cpp
    src/vulkan_backend.cpp)
add_dependencies(zh-fixedfunc-tests spike-shaders)
target_include_directories(zh-fixedfunc-tests PRIVATE src)
target_link_libraries(zh-fixedfunc-tests PRIVATE Vulkan::Vulkan)
target_compile_definitions(zh-fixedfunc-tests PRIVATE SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")

# Resource-lock tests: drive the D3D8 Lock/Unlock contract over Vulkan and assert on
# read-back pixels and read-back bytes. This is what backs
# docs/porting/renderer-resource-seam.md.
add_executable(zh-resource-lock-tests
    src/resource_lock_tests.cpp
    src/state_translate.cpp
    src/vulkan_backend.cpp)
add_dependencies(zh-resource-lock-tests spike-shaders)
target_include_directories(zh-resource-lock-tests PRIVATE src)
target_link_libraries(zh-resource-lock-tests PRIVATE Vulkan::Vulkan)
target_compile_definitions(zh-resource-lock-tests PRIVATE SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")

# Throughput baseline: draw calls/sec and state changes/sec, so the Mac has a Linux
# number to be compared against.
add_executable(zh-throughput
    src/throughput.cpp
    src/state_translate.cpp
    src/vulkan_backend.cpp)
add_dependencies(zh-throughput spike-shaders)
target_include_directories(zh-throughput PRIVATE src)
target_link_libraries(zh-throughput PRIVATE Vulkan::Vulkan)
target_compile_definitions(zh-throughput PRIVATE SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")

if(SPIKE_HAVE_SDL2)
    target_compile_definitions(zh-renderer-spike PRIVATE SPIKE_WITH_SDL)
    if(TARGET SDL2::SDL2)
        target_link_libraries(zh-renderer-spike PRIVATE SDL2::SDL2)
    else()
        target_include_directories(zh-renderer-spike PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(zh-renderer-spike PRIVATE ${SDL2_LIBRARIES})
    endif()
endif()

# --- window / event loop / input ---------------------------------------------
# The seam under test lives in the engine tree, not here, because it is engine code; the
# spike just builds it standalone, exactly as it does with the D3D8-shaped backend.
set(WWLIB_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Libraries/Source/WWVegas/WWLib")

# zh-window-spike: the same two draws as zh-renderer-spike, but presented to a real window
# through platform_window.h. One target per backend, because the backend is a link-time
# choice and both need to be buildable on a Mac to be compared.
if(SPIKE_HAVE_SDL2)
    add_executable(zh-window-spike
        src/window_spike.cpp
        src/png_write.cpp
        src/state_translate.cpp
        src/vulkan_backend.cpp
        "${WWLIB_PLATFORM_DIR}/platform/platform_window_sdl2.cpp")
    add_dependencies(zh-window-spike spike-shaders)
    target_include_directories(zh-window-spike PRIVATE src "${WWLIB_PLATFORM_DIR}")
    target_link_libraries(zh-window-spike PRIVATE Vulkan::Vulkan)
    target_compile_definitions(zh-window-spike PRIVATE
        SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}"
        SPIKE_WITH_PLATFORM_WINDOW)
    if(TARGET SDL2::SDL2)
        target_link_libraries(zh-window-spike PRIVATE SDL2::SDL2)
    else()
        target_include_directories(zh-window-spike PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(zh-window-spike PRIVATE ${SDL2_LIBRARIES})
    endif()
    # The SDL2 backend includes <SDL.h>, not <SDL2/SDL.h>, so that the same file works
    # against a Homebrew/framework SDL2 on macOS.
    if(SDL2_INCLUDE_DIRS)
        target_include_directories(zh-window-spike PRIVATE ${SDL2_INCLUDE_DIRS})
    endif()
endif()

# zh-window-spike-cocoa: the same driver against the native Cocoa/CAMetalLayer backend.
#
# WRITTEN BLIND - neither this target nor the file it compiles has ever been configured,
# compiled or run, because the session that wrote them had only Linux. It is what
# tools/macos-window-check.sh builds, and finding out whether it even compiles is that
# script's first job.
if(APPLE)
    include(CheckLanguage)
    check_language(OBJCXX)
    if(CMAKE_OBJCXX_COMPILER)
        enable_language(OBJCXX)
        add_executable(zh-window-spike-cocoa
            src/window_spike.cpp
            src/png_write.cpp
            src/state_translate.cpp
            src/vulkan_backend.cpp
            "${WWLIB_PLATFORM_DIR}/platform/platform_window_cocoa.mm")
        add_dependencies(zh-window-spike-cocoa spike-shaders)
        target_include_directories(zh-window-spike-cocoa PRIVATE src "${WWLIB_PLATFORM_DIR}")
        target_link_libraries(zh-window-spike-cocoa PRIVATE Vulkan::Vulkan
            "-framework AppKit" "-framework QuartzCore" "-framework Metal"
            "-framework CoreGraphics" "-framework Foundation")
        target_compile_definitions(zh-window-spike-cocoa PRIVATE
            SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}"
            SPIKE_WITH_PLATFORM_WINDOW)
    else()
        message(STATUS "no Objective-C++ compiler: zh-window-spike-cocoa not built")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target zh-renderer-spike zh-caps-probe zh-feature-probe zh-fixedfunc-tests
                   zh-resource-lock-tests zh-throughput)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    endforeach()
    foreach(target zh-window-spike zh-window-spike-cocoa)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wno-missing-field-initializers)
        endif()
    endforeach()
endif()
