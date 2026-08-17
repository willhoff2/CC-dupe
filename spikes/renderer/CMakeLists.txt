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
# The Objective-C++ sources (the Cocoa window backend, the Metal surface probe) use
# static_assert messages and decltype, so the standard is pinned here rather than left to
# whatever the Xcode clang of the day defaults to.
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Vulkan REQUIRED)
# The lock path is callable from more than one thread (usage class C4), so the tests
# that prove it need a threading library.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

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

# --- the backend, as a library -------------------------------------------------
# The two sources every target here compiled separately are now one static library, because the
# engine needs them too: Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp implements
# the engine's RenderBackendClass over this interface, and scripts/native-build.py builds these
# same two files into the native binary as the `support_renderbackend` archive.  Nothing moved
# and nothing was copied; the file list simply has one home now, so the engine's copy of the
# renderer cannot drift from the one the spike ladder verifies.
#
# Presentation is a COMPILE-TIME choice in this file: with neither SPIKE_WITH_PLATFORM_WINDOW nor
# SPIKE_WITH_SDL defined the surface and swapchain code is not compiled at all.  Defining it on the
# executable, as the window spike used to, defines it for that executable's own sources only -- so
# the window targets linked a swapchain-less backend, drew 240 frames, showed none, and passed.
# Hence one library per presentation path, all from the same two sources: a target picks the variant
# whose window handle it actually passes to Init(), and Init() now refuses a window it cannot
# present to instead of running blind.
function(zh_add_render_backend name)
    add_library(${name} STATIC
        src/state_translate.cpp
        src/vulkan_backend.cpp)
    add_dependencies(${name} spike-shaders)
    target_include_directories(${name} PUBLIC src)
    target_link_libraries(${name} PUBLIC Vulkan::Vulkan)
    target_compile_definitions(${name} PUBLIC
        SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")
endfunction()

# Headless: renders and reads back, never presents. What the pixel, fixed-function, lock,
# staging and throughput rungs use, and what scripts/native-build.py's engine archive is NOT
# (it defines SPIKE_WITH_PLATFORM_WINDOW, and CI gates that it did).
zh_add_render_backend(zh-render-backend)

# --- executable ---------------------------------------------------------------
add_executable(zh-renderer-spike
    src/main.cpp
    src/png_write.cpp)
add_dependencies(zh-renderer-spike spike-shaders)
target_link_libraries(zh-renderer-spike PRIVATE zh-render-backend)

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
add_executable(zh-fixedfunc-tests src/fixedfunc_tests.cpp)
add_dependencies(zh-fixedfunc-tests spike-shaders)
target_link_libraries(zh-fixedfunc-tests PRIVATE zh-render-backend)

# Resource-lock tests: drive the D3D8 Lock/Unlock contract over Vulkan and assert on
# read-back pixels and read-back bytes. This is what backs
# docs/porting/renderer-resource-seam.md.
add_executable(zh-resource-lock-tests src/resource_lock_tests.cpp)
add_dependencies(zh-resource-lock-tests spike-shaders)
target_link_libraries(zh-resource-lock-tests PRIVATE zh-render-backend Threads::Threads)

# HiDPI tests: the points/pixels rule, asserted on read-back pixels at an injected backing
# scale of 2.00 and 1.25. Headless, so Linux CI can run the Retina path no Linux display has;
# scripts/ci/check-hidpi-scale.py runs it and additionally proves the assertions still reject
# rendering at point size.
add_executable(zh-hidpi-tests src/hidpi_tests.cpp)
add_dependencies(zh-hidpi-tests spike-shaders)
target_link_libraries(zh-hidpi-tests PRIVATE zh-render-backend)

# Staging workload: replays a frame's worth of locks in the measured usage-class mix
# and prints the host-visible cost as JSON. scripts/ci/check-staging-cost.py enforces a
# ceiling on it, and threads need pthreads on Linux.
add_executable(zh-staging-workload src/staging_workload.cpp)
add_dependencies(zh-staging-workload spike-shaders)
target_link_libraries(zh-staging-workload PRIVATE zh-render-backend Threads::Threads)

# Draw capacity: how many draws one frame carries, with every draw's own texture and
# uniforms verified in the readback, so a dropped or aliased draw is named rather than
# guessed at. scripts/ci/check-draw-capacity.py gates the number.
add_executable(zh-draw-capacity src/draw_capacity.cpp)
add_dependencies(zh-draw-capacity spike-shaders)
target_link_libraries(zh-draw-capacity PRIVATE zh-render-backend)

# Throughput baseline: draw calls/sec and state changes/sec, so the Mac has a Linux
# number to be compared against.
add_executable(zh-throughput src/throughput.cpp)
add_dependencies(zh-throughput spike-shaders)
target_link_libraries(zh-throughput PRIVATE zh-render-backend)

if(SPIKE_HAVE_SDL2)
    # --window hands an SDL_Window to Init(), so the backend it links has to be the SDL variant:
    # the headless one has no SDL surface code and would present nothing.
    zh_add_render_backend(zh-render-backend-sdl)
    target_compile_definitions(zh-render-backend-sdl PUBLIC SPIKE_WITH_SDL)
    target_compile_definitions(zh-renderer-spike PRIVATE SPIKE_WITH_SDL)
    target_link_libraries(zh-renderer-spike PRIVATE zh-render-backend-sdl)
    if(TARGET SDL2::SDL2)
        target_link_libraries(zh-render-backend-sdl PUBLIC SDL2::SDL2)
        target_link_libraries(zh-renderer-spike PRIVATE SDL2::SDL2)
    else()
        target_include_directories(zh-render-backend-sdl PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(zh-render-backend-sdl PUBLIC ${SDL2_LIBRARIES})
        target_include_directories(zh-renderer-spike PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(zh-renderer-spike PRIVATE ${SDL2_LIBRARIES})
    endif()
endif()

# --- window / event loop / input ---------------------------------------------
# The seam under test lives in the engine tree, not here, because it is engine code; the
# spike just builds it standalone, exactly as it does with the D3D8-shaped backend.
set(WWLIB_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Libraries/Source/WWVegas/WWLib")

# The presentation variant the window spikes link, and the same configuration the engine's own
# archive is built with by scripts/native-build.py: the surface comes from the seam
# (Window_Create_Vulkan_Surface), which is how a CAMetalLayer reaches MoltenVK. The WWPlatform
# symbols it references are provided by whichever platform_window_*.cpp/.mm the executable links.
zh_add_render_backend(zh-render-backend-window)
target_compile_definitions(zh-render-backend-window PUBLIC SPIKE_WITH_PLATFORM_WINDOW)
target_include_directories(zh-render-backend-window PUBLIC "${WWLIB_PLATFORM_DIR}")

# zh-window-spike: the same two draws as zh-renderer-spike, but presented to a real window
# through platform_window.h. One target per backend, because the backend is a link-time
# choice and both need to be buildable on a Mac to be compared.
if(SPIKE_HAVE_SDL2)
    add_executable(zh-window-spike
        src/window_spike.cpp
        src/png_write.cpp
        "${WWLIB_PLATFORM_DIR}/platform/platform_window_sdl2.cpp")
    add_dependencies(zh-window-spike spike-shaders)
    target_include_directories(zh-window-spike PRIVATE "${WWLIB_PLATFORM_DIR}")
    target_link_libraries(zh-window-spike PRIVATE zh-render-backend-window)
    target_compile_definitions(zh-window-spike PRIVATE SPIKE_WITH_PLATFORM_WINDOW)
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

    # zh-hidpi-tests-sdl2: the --window form of the HiDPI check over the SDL2 seam. Its reason to
    # exist on Linux is that it compiles the window-mode code the Mac target runs -- a check written
    # blind is a check that does not build on the day it is needed -- and it can be run at
    # --min-scale 1.0 on an ordinary display as a smoke test. It cannot answer the Retina question:
    # every display Linux CI has is scale 1, which is exactly the scale the pre-fix code passed at.
    add_executable(zh-hidpi-tests-sdl2
        src/hidpi_tests.cpp
        "${WWLIB_PLATFORM_DIR}/platform/platform_window_sdl2.cpp")
    add_dependencies(zh-hidpi-tests-sdl2 spike-shaders)
    target_include_directories(zh-hidpi-tests-sdl2 PRIVATE "${WWLIB_PLATFORM_DIR}")
    target_link_libraries(zh-hidpi-tests-sdl2 PRIVATE zh-render-backend-window)
    target_compile_definitions(zh-hidpi-tests-sdl2 PRIVATE SPIKE_WITH_PLATFORM_WINDOW)
    if(TARGET SDL2::SDL2)
        target_link_libraries(zh-hidpi-tests-sdl2 PRIVATE SDL2::SDL2)
    else()
        target_include_directories(zh-hidpi-tests-sdl2 PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(zh-hidpi-tests-sdl2 PRIVATE ${SDL2_LIBRARIES})
    endif()
endif()

# zh-window-spike-cocoa: the same driver against the native Cocoa/CAMetalLayer backend, and
# zh-metal-surface-probe: the part of that path a machine with no windowing session can still
# exercise (CAMetalLayer -> VK_EXT_metal_surface -> swapchain -> present).
#
# Both are compiled in CI by the `window-seam-macos` job in .github/workflows/native-port-ci.yml,
# which is where platform_window_cocoa.mm - written blind on Linux by PR #32 - first met a
# compiler. tools/macos-window-check.sh builds the same targets on a real Mac.
if(APPLE)
    include(CheckLanguage)
    check_language(OBJCXX)
    if(CMAKE_OBJCXX_COMPILER)
        enable_language(OBJCXX)
        add_executable(zh-window-spike-cocoa
            src/window_spike.cpp
            src/png_write.cpp
            "${WWLIB_PLATFORM_DIR}/platform/platform_window_cocoa.mm")
        add_dependencies(zh-window-spike-cocoa spike-shaders)
        target_include_directories(zh-window-spike-cocoa PRIVATE "${WWLIB_PLATFORM_DIR}")
        target_link_libraries(zh-window-spike-cocoa PRIVATE zh-render-backend-window
            "-framework AppKit" "-framework QuartzCore" "-framework Metal"
            "-framework CoreGraphics" "-framework Foundation" "-framework Carbon")
        target_compile_definitions(zh-window-spike-cocoa PRIVATE
            SPIKE_WITH_PLATFORM_WINDOW
            # Checks the backend's 104 kVK_* literals against HIToolbox's real constants at
            # compile time. WWLib itself does not do this, because it must not need Carbon.
            WWLIB_COCOA_VERIFY_KVK)

        add_executable(zh-metal-surface-probe src/metal_surface_probe.mm)
        target_link_libraries(zh-metal-surface-probe PRIVATE Vulkan::Vulkan
            "-framework QuartzCore" "-framework Metal" "-framework Foundation")

        # The window/Retina/input/fullscreen numbers, which need a real display and a real
        # backingScaleFactor of 2 - so this target only ever runs on a Mac session, never in CI.
        add_executable(zh-macos-window-metrics
            src/macos_window_metrics.mm
            "${WWLIB_PLATFORM_DIR}/platform/platform_window_cocoa.mm")
        target_include_directories(zh-macos-window-metrics PRIVATE src "${WWLIB_PLATFORM_DIR}")
        target_link_libraries(zh-macos-window-metrics PRIVATE Vulkan::Vulkan
            "-framework AppKit" "-framework QuartzCore" "-framework Metal"
            "-framework CoreGraphics" "-framework ApplicationServices"
            "-framework Foundation" "-framework Carbon")
        target_compile_definitions(zh-macos-window-metrics PRIVATE WWLIB_COCOA_VERIFY_KVK)

        # zh-hidpi-tests-cocoa: the points/pixels assertions of zh-hidpi-tests against a real
        # NSWindow, so the scale is the display's rather than an injected one. `--window` is the
        # only form of the check that can say whether the render is full resolution on a Retina
        # panel, and it is the one a Mac session runs (spikes/renderer/tools/macos-window-check.sh
        # step 7). Linux CI runs the headless target instead, at an injected scale.
        add_executable(zh-hidpi-tests-cocoa
            src/hidpi_tests.cpp
            "${WWLIB_PLATFORM_DIR}/platform/platform_window_cocoa.mm")
        add_dependencies(zh-hidpi-tests-cocoa spike-shaders)
        target_include_directories(zh-hidpi-tests-cocoa PRIVATE "${WWLIB_PLATFORM_DIR}")
        target_link_libraries(zh-hidpi-tests-cocoa PRIVATE zh-render-backend-window
            "-framework AppKit" "-framework QuartzCore" "-framework Metal"
            "-framework CoreGraphics" "-framework Foundation" "-framework Carbon")
        target_compile_definitions(zh-hidpi-tests-cocoa PRIVATE
            SPIKE_WITH_PLATFORM_WINDOW WWLIB_COCOA_VERIFY_KVK)
    else()
        message(STATUS "no Objective-C++ compiler: zh-window-spike-cocoa not built")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target zh-render-backend zh-renderer-spike zh-caps-probe zh-feature-probe
                   zh-fixedfunc-tests zh-resource-lock-tests zh-hidpi-tests
                   zh-staging-workload zh-throughput zh-draw-capacity)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    endforeach()
    foreach(target zh-window-spike zh-window-spike-cocoa zh-metal-surface-probe
                   zh-macos-window-metrics zh-hidpi-tests-cocoa zh-hidpi-tests-sdl2)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wno-missing-field-initializers)
        endif()
    endforeach()
endif()
