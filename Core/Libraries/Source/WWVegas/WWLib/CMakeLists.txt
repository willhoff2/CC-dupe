set(WWLIB_SRC
    always.h
    argv.cpp
    argv.h
    b64pipe.cpp
    b64pipe.h
    b64straw.cpp
    b64straw.h
    base64.cpp
    base64.h
    #bfiofile.cpp
    #bfiofile.h
    binheap.h
    bittype.h
    bound.h
    BSEARCH.h
    buff.cpp
    BUFF.h
    bufffile.cpp
    bufffile.h
    CallbackHook.h
    chunkio.cpp
    chunkio.h
    cpudetect.cpp
    cpudetect.h
    #crandom.h
    crc.cpp
    CRC.h
    #crcpipe.cpp
    #crcpipe.h
    #crcstraw.cpp
    #crcstraw.h
    cstraw.cpp
    cstraw.h
    DbgHelpGuard.cpp
    DbgHelpGuard.h
    DbgHelpLoader.cpp
    DbgHelpLoader.h
    DbgHelpLoader_minidump.h
    Except.cpp
    Except.h
    FastAllocator.cpp
    FastAllocator.h
    ffactory.cpp
    ffactory.h
    gcd_lcm.cpp
    gcd_lcm.h
    #global.h
    hash.cpp
    hash.h
    hashcalc.h
    HASHLIST.h
    #hashtab.h
    hashtemplate.h
    #incdec.h
    INDEX.h
    ini.cpp
    INI.h
    inisup.h
    iostruct.h
    jshell.cpp
    LISTNODE.h
    #lzo.cpp
    #lzo.h
    #lzo1x.h
    #lzo1x_c.cpp
    #lzo1x_d.cpp
    #lzoconf.h
    #lzopipe.cpp
    #lzopipe.h
    #lzostraw.cpp
    #lzostraw.h
    #lzo_conf.h
    #md5.cpp
    #md5.h
    mempool.h
    mmsys.h
    mpsc_intrusive_queue.h
    mpu.cpp
    MPU.h
    multilist.cpp
    multilist.h
    mutex.cpp
    mutex.h
    #noinit.h
    notifier.h
    nstrdup.cpp
    nstrdup.h
    ntree.h
    pipe.cpp
    PIPE.h
    # Unlike the other platform/ modules this one is built everywhere; on Windows it forwards
    # to the same Win32 calls its call sites used to make directly.
    platform/platform_path.cpp
    platform/platform_path.h
    Point.h
    ramfile.cpp
    RAMFILE.h
    random.cpp
    RANDOM.h
    rawfile.cpp
    RAWFILE.h
    readline.cpp
    readline.h
    realcrc.cpp
    realcrc.h
    ref_ptr.h
    refcount.cpp
    refcount.h
    #regexpr.cpp
    #regexpr.h
    registry.cpp
    registry.h
    #search.h
    sharebuf.h
    Signaler.h
    simplevec.h
    SLIST.h
    slnode.cpp
    SLNODE.h
    stimer.cpp
    stimer.h
    STLUtils.h
    straw.cpp
    STRAW.h
    stringex.h
    strtok_r.cpp
    strtok_r.h
    #swap.h
    SystemAllocator.h
    systimer.cpp
    systimer.h
    TARGA.cpp
    TARGA.h
    textfile.cpp
    textfile.h
    tgatodxt.cpp
    tgatodxt.h
    thread.cpp
    thread.h
    trect.h
    trim.cpp
    trim.h
    uarray.h
    vector.cpp
    Vector.h
    visualc.h
    widestring.cpp
    widestring.h
    win.h
    WWCommon.h
    WWDefines.h
    wwfile.cpp
    WWFILE.h
    wwstring.cpp
    wwstring.h
    xpipe.cpp
    XPIPE.h
    xstraw.cpp
    XSTRAW.h
)

if(WIN32)
    list(APPEND WWLIB_SRC
        rcfile.cpp
        rcfile.h
        verchk.cpp
        verchk.h
        WWCOMUtil.cpp
        WWCOMUtil.h
    )
else()
    # Stand-ins for the Win32 threading, synchronisation, timing, registry and process calls.
    list(APPEND WWLIB_SRC
        platform/platform_dialog.cpp
        platform/platform_dialog.h
        platform/platform_mutex.cpp
        platform/platform_mutex.h
        platform/platform_process.cpp
        platform/platform_process.h
        platform/platform_settings.cpp
        platform/platform_settings.h
        platform/platform_thread.cpp
        platform/platform_thread.h
        platform/platform_time.cpp
        platform/platform_time.h
        platform/platform_window.h
    )
    # The window/event-loop/input backends (docs/porting/window-event-loop.md). The header
    # above is always listed because it is pure declarations; a backend is opt-in and off by
    # default, because it drags in SDL2 or Objective-C++ and nothing in the engine calls the
    # seam yet - the engine still cannot link natively at all (docs/porting/native-build.md).
    # spikes/renderer builds both backends standalone, which is where they are exercised.
    option(CORE_WWLIB_WINDOW_BACKEND
        "Compile a platform_window backend into core_wwlib (needs SDL2; Objective-C++ on Apple)"
        OFF)
    if(CORE_WWLIB_WINDOW_BACKEND)
        if(APPLE)
            enable_language(OBJCXX)
            # The backend is Objective-C++17 (ARC is off; it retains/releases nothing that
            # outlives the window). The spike builds the same file the same way.
            set(CMAKE_OBJCXX_STANDARD 17)
            set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
            list(APPEND WWLIB_SRC platform/platform_window_cocoa.mm)
        else()
            find_package(SDL2 REQUIRED)
            list(APPEND WWLIB_SRC platform/platform_window_sdl2.cpp)
        endif()
    endif()
endif()

add_library(core_wwlib STATIC)
set_target_properties(core_wwlib PROPERTIES OUTPUT_NAME wwlib)

target_sources(core_wwlib PRIVATE ${WWLIB_SRC})

target_precompile_headers(core_wwlib PRIVATE
    [["Utility/CppMacros.h"]] # Must be first, to be removed when abandoning VC6
    [["WWLib/always.h"]]
    [["WWLib/STLUtils.h"]]
    [["WWLib/win.h"]]
    [["WWLib/WWCommon.h"]]
    [["WWLib/wwstring.h"]]
    <windows.h>
)

target_link_libraries(core_wwlib PRIVATE
    core_wwcommon
    corei_always
)

if(NOT WIN32 AND CORE_WWLIB_WINDOW_BACKEND)
    if(APPLE)
        # CoreGraphics for the cursor calls (macOS has no ClipCursor) and Foundation for the
        # autorelease pool; AppKit drags both in, but linking what the source #imports keeps
        # this working if the backend is ever built on its own.
        target_link_libraries(core_wwlib PRIVATE
            "-framework AppKit" "-framework QuartzCore" "-framework Metal"
            "-framework CoreGraphics" "-framework Foundation")
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(core_wwlib PRIVATE SDL2::SDL2)
    else()
        target_include_directories(core_wwlib PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(core_wwlib PRIVATE ${SDL2_LIBRARIES})
    endif()
endif()
