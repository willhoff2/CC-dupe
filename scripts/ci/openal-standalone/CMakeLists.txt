# Standalone build of Core/Libraries/Source/OpenALAudioDevice, for CI.
#
# The repository's own top-level CMakeLists cannot be configured off Windows yet (the non-Windows
# branch skips dx8/bink, which the renderer and video code still need), so cmake/openal.cmake is
# unreachable and the OpenAL backend has never been built by anything automated. This harness
# builds exactly the target cmake/openal.cmake would build -- the real
# Core/Libraries/Source/OpenALAudioDevice/CMakeLists.txt, unmodified -- against a system OpenAL,
# and nothing else. When the top-level non-Windows configure works, this should be deleted and
# the job pointed at the real build.
cmake_minimum_required(VERSION 3.20)
project(openal_audio_device_standalone CXX)

find_package(OpenAL REQUIRED)

# FindOpenAL only defines the OpenAL::OpenAL imported target from CMake 3.27; the
# OpenALAudioDevice target links it by name, so synthesise it on older CMake.
if(NOT TARGET OpenAL::OpenAL)
    add_library(OpenAL::OpenAL UNKNOWN IMPORTED)
    set_target_properties(OpenAL::OpenAL PROPERTIES
        IMPORTED_LOCATION "${OPENAL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENAL_INCLUDE_DIR}")
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_subdirectory("${REPO_ROOT}/Core/Libraries/Source/OpenALAudioDevice" openalaudiodevice)

# The backend is a drop-in for mss32: warnings here are portability defects, not noise.
target_compile_options(core_openalaudiodevice PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra>)
