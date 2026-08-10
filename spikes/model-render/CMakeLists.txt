## zh-model-render — draws a retail Zero Hour model, out of the user's own .big archives,
## through the renderer spike's Vulkan backend.
##
## Standalone like the other spikes, so it cannot affect the Windows game build:
##
##   cmake -S spikes/model-render -B build/model-render -DCMAKE_BUILD_TYPE=RelWithDebInfo
##   cmake --build build/model-render
##   build/model-render/zh-model-render --data /path/to/zero-hour --out crusader.png
##
## Requires a Vulkan loader with headers (MoltenVK on macOS) and glslangValidator or glslc.
## The engine sources listed below are compiled unmodified; see
## docs/porting/native-model-render.md for what would not compile and why.

cmake_minimum_required(VERSION 3.20)
project(zh_model_render CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ASSETS_SPIKE "${REPO_ROOT}/spikes/assets")
set(RENDERER_SPIKE "${REPO_ROOT}/spikes/renderer")
set(WWVEGAS "${REPO_ROOT}/Core/Libraries/Source/WWVegas")

find_package(Vulkan REQUIRED)

# --- shaders (the renderer spike's own, unmodified) ---------------------------
find_program(GLSL_COMPILER NAMES glslangValidator glslc)
if(NOT GLSL_COMPILER)
    message(FATAL_ERROR "need glslangValidator or glslc on PATH to compile the shaders")
endif()

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
set(SPIRV_OUTPUTS)
foreach(shader fixedfunc.vert fixedfunc.frag)
    set(shader_in "${RENDERER_SPIKE}/shaders/${shader}")
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
add_custom_target(model-render-shaders DEPENDS ${SPIRV_OUTPUTS})

# --- the engine's own asset code, compiled natively at 64 bits ----------------
# Anything that includes dx8wrapper.h (mesh.cpp, meshmdl.cpp, texture.cpp, ...) cannot be in
# this list: it needs the real d3d8.h. The chunk reader and the hierarchy do not.
add_library(zh-engine-w3d STATIC
    src/w3d_model.cpp
    src/engine_link_support.cpp
    "${ASSETS_SPIKE}/src/posix_file.cpp"
    "${WWVEGAS}/WWLib/chunkio.cpp"
    "${WWVEGAS}/WWMath/matrix3d.cpp"
    "${WWVEGAS}/WWMath/matrix4.cpp"
    "${WWVEGAS}/WWMath/quat.cpp"
    "${WWVEGAS}/WWMath/wwmath.cpp"
    "${WWVEGAS}/WW3D2/htree.cpp"
    "${WWVEGAS}/WW3D2/pivot.cpp"
    # The engine's own no-game-engine allocator stubs, used as-is: htree.cpp's
    # W3DMPO_GLUE macro calls into the memory pool.
    "${WWVEGAS}/WWStub/wwallocstub.cpp"
    "${WWVEGAS}/WWStub/wwdebugstub.cpp")

target_include_directories(zh-engine-w3d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(zh-engine-w3d PRIVATE
    "${ASSETS_SPIKE}/src"
    "${REPO_ROOT}/Dependencies/Utility"
    "${REPO_ROOT}/Core/Libraries/Include"
    "${WWVEGAS}"
    "${WWVEGAS}/WWLib"
    "${WWVEGAS}/WWDebug"
    "${WWVEGAS}/WWMath"
    "${WWVEGAS}/WW3D2"
    # w3d_file.h and the WW3D2 headers the hierarchy needs live in the game trees.
    "${REPO_ROOT}/GeneralsMD/Code/Libraries/Source/WWVegas"
    "${REPO_ROOT}/GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2"
    # Declaration-only stand-ins for the two D3D8 headers WWMath's matrix conversion helpers
    # include. Nothing in this target calls into D3D.
    "${REPO_ROOT}/scripts/native-port-shims")

# Mirrors the MSVC build's /FIUtility/CppMacros.h, exactly as the port probe does.
target_compile_options(zh-engine-w3d PRIVATE
    -fms-extensions
    -include Utility/CppMacros.h
    -Wno-implicit-exception-spec-mismatch)
# chunkio.cpp asserts on a chunk-stack overflow; the tool's own bounds checks report instead.
target_compile_definitions(zh-engine-w3d PRIVATE NDEBUG)

# --- the renderer spike's Vulkan backend, unmodified -------------------------
add_library(zh-render-backend STATIC
    "${RENDERER_SPIKE}/src/png_write.cpp"
    "${RENDERER_SPIKE}/src/state_translate.cpp"
    "${RENDERER_SPIKE}/src/vulkan_backend.cpp")
add_dependencies(zh-render-backend model-render-shaders)
target_include_directories(zh-render-backend PUBLIC "${RENDERER_SPIKE}/src")
target_link_libraries(zh-render-backend PUBLIC Vulkan::Vulkan)
target_compile_definitions(zh-render-backend PRIVATE SPIKE_SHADER_DIR="${SHADER_OUTPUT_DIR}")

# --- the tool -----------------------------------------------------------------
add_executable(zh-model-render
    src/main.cpp
    src/asset_source.cpp
    src/scene.cpp
    src/texture_load.cpp
    "${ASSETS_SPIKE}/src/big_archive.cpp")
add_dependencies(zh-model-render model-render-shaders)
target_include_directories(zh-model-render PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ASSETS_SPIKE}/src"
    "${REPO_ROOT}/Dependencies/Utility"
    "${REPO_ROOT}/Core/Libraries/Include")
target_link_libraries(zh-model-render PRIVATE zh-engine-w3d zh-render-backend)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(zh-model-render PRIVATE
        -Wall -Wextra -Wno-missing-field-initializers)
endif()
