set(DEBUG_SRC
    "debug_cmd.cpp"
    "debug_cmd.h"
    "debug_debug.cpp"
    "debug_debug.h"
    "debug_doc.h"
    "debug_except.cpp"
    "debug_getdefaultcommands.cpp"
    "debug_internal.cpp"
    "debug_io_con.cpp"
    "debug_io_flat.cpp"
    "debug_io_net.cpp"
    "debug_io_ods.cpp"
    "debug_io.h"
    "debug_macro.h"
    "debug_stack.cpp"
    "debug_stack.h"
    "debug.h"
    "internal_except.h"
    "internal_io.h"
    "internal.h"
)

if(NOT WIN32)
    # Stand-ins for the Win32 memory, file, console, debugger-output and process calls this
    # library makes, plus the backtrace()/dladdr() stack walk that replaces DbgHelp.
    # docs/porting/debug-and-profile-libs.md.
    list(APPEND DEBUG_SRC
        platform/debug_platform.cpp
        platform/debug_platform.h
    )
endif()

add_library(core_debug STATIC)

target_sources(core_debug PRIVATE ${DEBUG_SRC})

target_include_directories(core_debug INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_precompile_headers(core_debug PRIVATE
    [["Utility/CppMacros.h"]] # Must be first, to be removed when abandoning VC6
    "debug.h"
    "internal.h"
    "internal_except.h"
    "internal_io.h"
    <windows.h>
)

target_link_libraries(core_debug PRIVATE
    core_wwcommon
    corei_always
)
