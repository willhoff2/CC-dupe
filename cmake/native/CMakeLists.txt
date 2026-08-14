# A native 64-bit build of the platform-independent libraries.
#
# Deliberately a separate project rather than a preset of the top-level CMakeLists.txt. That build
# is 32-bit Windows first: its non-Windows path exists but has never produced an object file, and
# its `unix` preset inherits a 32-bit vcpkg configuration. Bolting a 64-bit native target onto it
# would mean editing the build every Windows CI configuration depends on in order to find out
# whether the code compiles at all. This project answers that question first, in isolation, and
# cannot regress the Windows build because nothing includes it.
#
# It is driven by scripts/native-build.py, which generates one fragment per library into
# ${NATIVE_MANIFEST_DIR} from the same target/include/flag definitions the native probe uses, so
# the object-file result is directly comparable with the probe's -fsyntax-only result. Configuring
# this directory by hand without the generated manifests is not supported.
#
# What this is not: a playable build. There is no linked executable, no Windows compatibility
# layer, and no expectation that every library builds. Targets that fail are the output.
#
# Some slugs in NATIVE_TARGETS are third-party support libraries (lzhl) rather than engine
# libraries. They are built the same way but are excluded from the measurement by the driving
# script: what matters here is only that the engine's calls into them can be resolved.

cmake_minimum_required(VERSION 3.20)
project(sage_native CXX)

if(NOT DEFINED NATIVE_MANIFEST_DIR)
    message(FATAL_ERROR
        "NATIVE_MANIFEST_DIR is not set. Configure through scripts/native-build.py, which "
        "generates the per-library manifests this project reads.")
endif()

if(NOT DEFINED NATIVE_TARGETS)
    message(FATAL_ERROR "NATIVE_TARGETS is not set (semicolon-separated list of library slugs).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Kept in step with CLANG_FLAGS in scripts/native-port-probe.py. `-fms-extensions` keeps
# `__int64`/`__forceinline` working (removing them is its own mechanical pass) and CppMacros.h
# mirrors the MSVC build's /FIUtility/CppMacros.h force-include.
add_compile_options(
    -fms-extensions
    -include Utility/CppMacros.h
)
add_compile_definitions(WIN32_LEAN_AND_MEAN _REENTRANT)

# 64-bit is the point of the exercise: Apple Silicon has no 32-bit mode. On x86-64 hosts say so
# explicitly so a stray -m32 in the environment cannot quietly change what is being measured;
# arm64 has no such flag.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-m64)
endif()

foreach(slug IN LISTS NATIVE_TARGETS)
    set(manifest "${NATIVE_MANIFEST_DIR}/${slug}.cmake")
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "Manifest ${manifest} does not exist; re-run scripts/native-build.py.")
    endif()

    # Each manifest sets NATIVE_SOURCES, NATIVE_INCLUDES and NATIVE_DEFINES for one library.
    unset(NATIVE_SOURCES)
    unset(NATIVE_INCLUDES)
    unset(NATIVE_DEFINES)
    include("${manifest}")

    add_library(${slug} STATIC ${NATIVE_SOURCES})
    # SYSTEM keeps the vendored and engine headers' own warnings out of the way; the interesting
    # diagnostics are errors, and every one of these trees predates -Wall by two decades.
    target_include_directories(${slug} SYSTEM PRIVATE ${NATIVE_INCLUDES})
    if(NATIVE_DEFINES)
        target_compile_definitions(${slug} PRIVATE ${NATIVE_DEFINES})
    endif()
endforeach()
