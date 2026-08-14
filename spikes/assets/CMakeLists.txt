## zh-asset-inspect — native 64-bit reader for retail Zero Hour asset files.
##
## Standalone on purpose: it links the engine's own chunkio.cpp, not a reimplementation, so it
## measures the real port. It is not part of the game build and is configured on its own:
##
##   cmake -S spikes/assets -B build/assets -DCMAKE_BUILD_TYPE=RelWithDebInfo
##   cmake --build build/assets

cmake_minimum_required(VERSION 3.20)
project(zh_asset_inspect CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(W3D_FILE_HEADER "${REPO_ROOT}/GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/w3d_file.h")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CHUNK_NAMES_HEADER "${GENERATED_DIR}/chunk_names.h")

# The chunk-id table is derived from the engine header at build time, never hand-copied.
add_custom_command(
    OUTPUT "${CHUNK_NAMES_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_chunk_names.py"
            "${W3D_FILE_HEADER}" "${CHUNK_NAMES_HEADER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_chunk_names.py" "${W3D_FILE_HEADER}"
    COMMENT "Generating W3D chunk id table from w3d_file.h"
    VERBATIM)

add_executable(zh-asset-inspect
    src/big_archive.cpp
    src/dds_inspect.cpp
    src/main.cpp
    src/posix_file.cpp
    src/w3d_dump.cpp
    # The engine's real chunk reader, compiled natively at 64-bit.
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp"
    "${CHUNK_NAMES_HEADER}")

target_include_directories(zh-asset-inspect PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GENERATED_DIR}"
    "${REPO_ROOT}/Dependencies/Utility"
    "${REPO_ROOT}/Core/Libraries/Include"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWDebug")

# Mirrors what scripts/native-port-probe.py compiles the engine with: the VC6 compatibility
# macros are force-included exactly as the MSVC build does with /FIUtility/CppMacros.h.
target_compile_options(zh-asset-inspect PRIVATE
    -fms-extensions
    -include Utility/CppMacros.h
    -Wno-implicit-exception-spec-mismatch)

# chunkio.cpp asserts on a chunk stack overflow. With a mis-sized ChunkHeader the parse runs off
# the rails and would abort before the tool could report why, so asserts are compiled out and the
# tool's own bounds checks catch it instead.
target_compile_definitions(zh-asset-inspect PRIVATE NDEBUG)

# zh-path-probe points the engine's own directory seam (WWPlatform::Path, the code that replaced
# the FindFirstFile()/FindNextFile() loops) at a real data directory. Same deal as above: the
# seam is compiled unmodified, so what it reports is the engine's behaviour, not a stand-in's.
add_executable(zh-path-probe
    src/path_probe.cpp
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib/platform/platform_path.cpp"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib/wwstring.cpp"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib/Vector.cpp"
    # BooleanVectorClass in Vector.cpp calls the bit helpers that live here.
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib/jshell.cpp")

# Off Windows the probe also drives the Win32 file seam directly, under the names the engine's own
# asset scan still calls: FindFirstFile()/FindNextFile()/GetFileAttributes(). scripts/native-port-shims
# is what supplies <windows.h>; without it platform_win32_compat.h compiles the seam to nothing.
if(NOT WIN32)
    target_sources(zh-path-probe PRIVATE
        "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib/platform/platform_win32_file.cpp")
    target_include_directories(zh-path-probe PRIVATE "${REPO_ROOT}/scripts/native-port-shims")
    target_compile_definitions(zh-path-probe PRIVATE
        ZH_PATH_PROBE_WIN32_SEAM WIN32_LEAN_AND_MEAN _REENTRANT)
endif()

target_include_directories(zh-path-probe PRIVATE
    "${REPO_ROOT}/Dependencies/Utility"
    "${REPO_ROOT}/Core/Libraries/Include"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWLib"
    "${REPO_ROOT}/Core/Libraries/Source/WWVegas/WWDebug")

target_compile_options(zh-path-probe PRIVATE
    -fms-extensions
    -include Utility/CppMacros.h
    -Wno-implicit-exception-spec-mismatch)

target_compile_definitions(zh-path-probe PRIVATE NDEBUG)

# The `sizes` command reports sizeof() for every on-disk structure, and `w3d` refuses to call a
# parse successful when ChunkHeader is not 8 bytes, so building this against a tree whose
# WWLib/bittype.h still spells uint32 as `unsigned long` reproduces the 16-byte failure rather
# than hiding it.
