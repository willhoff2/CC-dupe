set(WW3D2_SRC
    aabtree.cpp
    aabtree.h
    #aabtreebuilder.cpp
    #aabtreebuilder.h
    agg_def.cpp
    agg_def.h
    animatedsoundmgr.cpp
    animatedsoundmgr.h
    #animobj.cpp
    #animobj.h
    #assetmgr.cpp
    #assetmgr.h
    assetstatus.cpp
    assetstatus.h
    bitmaphandler.cpp
    bitmaphandler.h
    bmp2d.cpp
    bmp2d.h
    #boxrobj.cpp
    #boxrobj.h
    bwrender.cpp
    bwrender.h
    #camera.cpp
    #camera.h
    #classid.h # unused
    collect.cpp
    collect.h
    colorspace.h
    coltest.cpp
    coltest.h
    coltype.h
    composite.cpp
    composite.h
    d3d8renderbackend.cpp
    d3d8renderbackend.h
    d3dx8fvf.cpp
    d3dx8shader.cpp
    d3dx8texcreate.cpp
    d3dx8texcreate.h
    d3dx8texture.cpp
    d3dx8texture.h
    #dazzle.cpp
    #dazzle.h
    #ddsfile.cpp
    #ddsfile.h
    #decalmsh.cpp
    #decalmsh.h
    decalsys.cpp
    decalsys.h
    distlod.cpp
    distlod.h
    dllist.h
    dx8caps.cpp
    dx8caps.h
    dx8fvf.cpp
    dx8fvf.h
    dx8indexbuffer.cpp
    dx8indexbuffer.h
    dx8list.h
    dx8polygonrenderer.cpp
    dx8polygonrenderer.h
    dx8renderer.cpp
    dx8renderer.h
    dx8rendererdebugger.cpp
    dx8rendererdebugger.h
    dx8texman.cpp
    dx8texman.h
    dx8vertexbuffer.cpp
    dx8vertexbuffer.h
    dx8webbrowser.cpp
    dx8webbrowser.h
    dx8wrapper.cpp
    dx8wrapper.h
    dynamesh.cpp
    dynamesh.h
    font3d.cpp
    font3d.h
    formconv.cpp
    formconv.h
    FramGrab.cpp
    framgrab.h
    hanim.cpp
    hanim.h
    #hanimmgr.cpp
    #hanimmgr.h
    hcanim.cpp
    hcanim.h
    #hlod.cpp
    #hlod.h
    hmdldef.cpp
    hmdldef.h
    #hmorphanim.cpp
    #hmorphanim.h
    #hrawanim.cpp
    #hrawanim.h
    htree.cpp
    htree.h
    #htreemgr.cpp
    #htreemgr.h
    intersec.cpp
    intersec.h
    intersec.inl
    inttest.h
    layer.cpp
    layer.h
    #light.cpp
    #light.h
    #lightenvironment.cpp
    #lightenvironment.h
    line3d.cpp
    line3d.h
    #linegrp.cpp
    #linegrp.h
    #mapper.cpp
    #mapper.h
    matinfo.cpp
    matinfo.h
    matpass.cpp
    matpass.h
    #matrixmapper.cpp
    #matrixmapper.h
    #mesh.cpp
    #mesh.h
    #meshbuild.cpp
    #meshbuild.h
    meshdam.cpp
    meshdam.h
    #meshgeometry.cpp
    #meshgeometry.h
    #meshmatdesc.cpp
    #meshmatdesc.h
    #meshmdl.cpp
    #meshmdl.h
    #meshmdlio.cpp
    metalmap.cpp
    metalmap.h
    missingtexture.cpp
    missingtexture.h
    #motchan.cpp
    #motchan.h
    #nullrobj.cpp
    #nullrobj.h
    #part_buf.cpp
    #part_buf.h
    #part_emt.cpp
    #part_emt.h
    #part_ldr.cpp
    #part_ldr.h
    pivot.cpp
    pivot.h
    pointgr.cpp
    pointgr.h
    polyinfo.cpp
    polyinfo.h
    predlod.cpp
    predlod.h
    prim_anim.cpp
    prim_anim.h
    projector.cpp
    projector.h
    proto.cpp
    proto.h
    proxy.h
    rddesc.h
    renderbackend.h
    #render2d.cpp
    #render2d.h
    render2dsentence.cpp
    render2dsentence.h
    renderobjectrecycler.cpp
    renderobjectrecycler.h
    rendobj.cpp
    rendobj.h
    #rinfo.cpp
    #rinfo.h
    ringobj.cpp
    ringobj.h
    robjlist.h
    #scene.cpp
    #scene.h
    segline.cpp
    segline.h
    seglinerenderer.cpp
    seglinerenderer.h
    #shader.cpp
    #shader.h
    shattersystem.cpp
    shattersystem.h
    shdlib.h
    snappts.cpp
    snapPts.h
    sortingrenderer.cpp
    sortingrenderer.h
    soundlibrarybridge.h
    soundrobj.cpp
    soundrobj.h
    sphereobj.cpp
    sphereobj.h
    static_sort_list.cpp
    static_sort_list.h
    statistics.cpp
    statistics.h
    streak.cpp
    streak.h
    streakRender.cpp
    streakRender.h
    stripoptimizer.cpp
    stripoptimizer.h
    surfaceclass.cpp
    surfaceclass.h
    texproject.cpp
    texproject.h
    #textdraw.cpp # unused
    textdraw.h
    texture.cpp
    texture.h
    texturefilter.cpp
    texturefilter.h
    textureloader.cpp
    textureloader.h
    texturethumbnail.cpp
    texturethumbnail.h
    #vertmaterial.cpp
    #vertmaterial.h
    visrasterizer.cpp
    visrasterizer.h
    # TheSuperHackers @port The non-Windows RenderBackendClass. Entirely inside #ifndef _WIN32,
    # so the Windows build compiles an empty translation unit and its behaviour is unchanged.
    vulkanrenderbackend.cpp
    vulkanrenderbackend.h
    w3d_dep.cpp
    w3d_dep.h
    #w3d_file.h
    w3d_obsolete.h
    w3d_util.cpp
    w3d_util.h
    w3derr.h
    w3dexclusionlist.cpp
    w3dexclusionlist.h
    #ww3d.cpp
    #ww3d.h
    ww3dformat.cpp
    ww3dformat.h
    ww3dids.h
    ww3dtrig.h
)

add_library(corei_ww3d2 INTERFACE)

target_sources(corei_ww3d2 INTERFACE ${WW3D2_SRC})

target_include_directories(corei_ww3d2 INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# TheSuperHackers @port Off Windows, vulkanrenderbackend.cpp implements the RenderBackendClass seam
# over the renderer in spikes/renderer, so it needs that interface header and Vulkan's. On Windows
# the file is empty and neither is looked for. Kept as an interface include rather than a link to
# zh-render-backend: the library that carries the backend's objects is chosen by whoever builds the
# game target (scripts/native-build.py builds it as a support archive), and adding a dependency here
# would make every consumer of corei_ww3d2 need Vulkan at link time.
if (NOT WIN32)
    target_include_directories(corei_ww3d2 INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../spikes/renderer/src"
    )
    # Not REQUIRED: configuring a target that does not compile this file (the audio and probe jobs
    # configure the tree without building WW3D2) must not fail for want of a renderer. Compiling it
    # without Vulkan's headers fails loudly on the include, which is the honest failure.
    find_package(Vulkan QUIET)
    if (Vulkan_FOUND)
        target_include_directories(corei_ww3d2 INTERFACE "${Vulkan_INCLUDE_DIRS}")
    endif()
endif()

if (MSVC AND NOT IS_VS6_BUILD)
    target_link_libraries(corei_ww3d2 INTERFACE
        comsuppw
    )
endif()

target_link_libraries(corei_ww3d2 INTERFACE
    core_browserengine
    core_wwlib
    core_wwmath
)
