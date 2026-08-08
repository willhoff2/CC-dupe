# OpenAL replacement for the Miles Sound System.
#
# The 32-bit Windows build fetches the real miles-sdk-stub (see cmake/miles.cmake) and links the
# retail mss32.dll at runtime. Every other configuration gets an OpenAL-backed implementation of
# the same AIL_* API instead, so WWAudio and MilesAudioManager compile unmodified on both.
#
# See docs/porting/audio-surface.md for the measured API surface this replaces.

find_package(OpenAL QUIET)

if(NOT OpenAL_FOUND AND NOT TARGET OpenAL::OpenAL)
    # OpenAL Soft is the reference implementation on Linux and works on macOS too. Pinned to a tag
    # rather than a branch so builds are reproducible.
    FetchContent_Declare(
        openal_soft
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG        1.23.1
    )

    set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(openal_soft)

    if(NOT TARGET OpenAL::OpenAL)
        add_library(OpenAL::OpenAL ALIAS OpenAL)
    endif()
endif()

add_subdirectory(Core/Libraries/Source/OpenALAudioDevice)

# Every consumer links an abstract `milesstub` target; substituting the implementation here means
# no consumer needs to know which audio backend it got.
add_library(milesstub INTERFACE)
target_link_libraries(milesstub INTERFACE core_openalaudiodevice)
