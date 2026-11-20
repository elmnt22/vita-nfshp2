cmake_minimum_required(VERSION 3.24)
project(NFSHP2 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # C++ implementations behind the retained C ABI propagate exceptions.
    # Remove CMake's default /EHc promise before target-specific flags apply.
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# Platform services and resource archive support.
add_library(NFSHP2Core STATIC
    bware/src/bFile.cpp bware/src/bFileE.cpp
    indep/src/lzcompress.cpp

    bware/src/bMemory.cpp
    src/platform.cpp)
target_sources(NFSHP2Core PRIVATE
    bware/src/Strings.cpp
    bware/src/bDebug.cpp
    indep/src/brepeat.cpp
    src/ecstasy/ecstasye.cpp
    src/ecstasy/eenvmape.cpp
)

target_include_directories(NFSHP2Core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/indep/inc")

# Game logic and native runtime services.
add_library(NFSHP2Engine STATIC
    bware/src/bList.cpp

    indep/src/racecoordinator.cpp
    indep/src/trackedcontroller.cpp

    indep/src/carsounds.cpp
    indep/src/carsoundengine.cpp
    indep/src/carsoundtires.cpp
    indep/src/choppersound.cpp
    indep/src/soundscape.cpp
    indep/src/weatherman.cpp

    indep/src/trackpath.cpp
    indep/src/physicscar.cpp


    indep/src/timer.cpp

    indep/src/terraintype.cpp

)
# Runtime modules and their typed dependencies.
target_sources(NFSHP2Engine PRIVATE
    indep/src/cameramover.cpp
    indep/src/zonecameras.cpp
    indep/src/rumble.cpp
    indep/src/trackrouteeditor.cpp
    indep/src/copdriver.cpp
    indep/src/copmgr.cpp


    indep/src/joystick.cpp
    indep/src/eastereggs.cpp
    indep/src/stomper.cpp
    indep/src/roadblock.cpp
    indep/src/spacefiller.cpp
    indep/src/ghostcar.cpp
    
    indep/src/realcontroller.cpp
)

target_sources(NFSHP2Engine PRIVATE
    bware/src/bMath.cpp
    bware/src/bMatrix.cpp
    bware/src/bQuaternion.cpp
    bware/src/bVector.cpp
    indep/src/aerodynamics.cpp
    indep/src/animateddriver.cpp
    indep/src/aicarcontroller.cpp
    indep/src/aiplan.cpp
    indep/src/aiutil.cpp
    indep/src/bodydamage.cpp
    indep/src/cardamage.cpp
    indep/src/car.cpp
    indep/src/rewardsystem.cpp
    indep/src/carinfo.cpp
    indep/src/carloader.cpp
    indep/src/trackgraph.cpp
    indep/src/scenery.cpp
    indep/src/worldobject.cpp
    indep/src/fernflattener.cpp
    indep/src/terraineffects.cpp
    indep/src/texture.cpp
    indep/src/resourceloader.cpp
    src/ecstasy/elighte.cpp
    src/sune.cpp
    src/skye.cpp
    indep/src/munitions.cpp
    indep/src/speech/speechfn.cpp
    indep/src/ecstasy.cpp
    indep/src/hudanim.cpp
    indep/src/language.cpp
    indep/src/gameflow.cpp
    indep/src/joylog.cpp
    indep/src/Config.cpp
    indep/src/main.cpp
    bsound/src/bRegion.cpp
    src/demodisc.cpp
    bsound/src/bDriver.cpp
    bsound/src/bSound.cpp
    bsound/src/bJukebox.cpp
    bsound/src/bSample.cpp
    indep/src/MoviePlayer/cyclicbuffer.cpp
    indep/src/MoviePlayer/huffman2.cpp
    indep/src/MoviePlayer/dcttable.cpp
    indep/src/MoviePlayer/codec.cpp
    indep/src/MoviePlayer/dctimage.cpp
    indep/src/MoviePlayer/ringbuffer.cpp
    indep/src/MoviePlayer/dctblock.cpp
    indep/src/MoviePlayer/mv3codec.cpp
    indep/src/MoviePlayer/mv3codecdecomp.cpp
    indep/src/MoviePlayer/movieplayer.cpp
    src/ecstasy/eenveffectse.cpp
    indep/src/frontendscreens/mainmenu.cpp
    cmn/SPCHlog.c
    cmn/SPCHrule.c
    cmn/SPCHsamp.c
    bsound/src/bSegment.cpp
    indep/src/queuedfile.cpp
    indep/src/frontendscreens/options.cpp
    indep/src/frontendscreens/nameentry.cpp
    indep/src/frontendscreens/racesetup.cpp
    indep/src/frontendscreens/joystickconfiguration.cpp
    indep/src/frontendscreens/frontendeffects.cpp
    indep/src/frontendscreens/frontend.cpp
    indep/src/barricade.cpp
    indep/src/speech/conversions.cpp
    indep/src/sound.cpp
    indep/src/sound3d.cpp
    indep/src/smokeable.cpp
    indep/src/eventtree.cpp
    indep/src/speechnoise.cpp
    indep/src/speech/mgr.cpp
    indep/src/speech/fsm.cpp
    indep/src/speech/state.cpp
    indep/src/speech/transition.cpp
    indep/src/speech/action.cpp
    indep/src/speech/nfsfsm.cpp
    indep/src/frontendscreens/campaigntrees.cpp
    indep/src/frontendscreens/rewardview.cpp
    indep/src/frontendscreens/feanim.cpp
    indep/src/frontendscreens/helpmenu.cpp
    indep/src/frontendscreens/selectcar.cpp
    indep/src/frontendscreens/replayscreen.cpp
    indep/src/frontendscreens/selecttrack.cpp
    indep/src/speech/sedwedge.cpp
    cmn/SPCHrslv.c
    cmn/SPCHbank.c
    cmn/SPCHrand.c
    cmn/SPCHinit.c
    cmn/spchevnt.c
    cmn/spchpick.c
    src/acidfxe.cpp
    indep/src/nis.cpp
    indep/src/player.cpp
    src/care.cpp
    indep/src/trafficteleporter.cpp
    indep/src/soundcollision.cpp
    indep/src/sound3dsample.cpp
    indep/src/characterinfo.cpp
    indep/src/eanimations.cpp
    indep/src/eanimationsolid.cpp
    indep/src/cameracoordinator.cpp
    indep/src/carcollisiondetect.cpp
    indep/src/carstate.cpp
    indep/src/chopper.cpp
    indep/src/collision.cpp
    indep/src/collisionbody.cpp
    indep/src/componentphysics.cpp
    indep/src/contact.cpp
    indep/src/integrable.cpp
    indep/src/cop.cpp
    indep/src/coputil.cpp
    indep/src/drivetarget.cpp
    indep/src/drivingchoice.cpp
    indep/src/drivinglocation.cpp
    indep/src/engine.cpp
    indep/src/eventmanager.cpp
    indep/src/fred.cpp
    indep/src/replay.cpp
    indep/src/demomodereplay.cpp
    indep/src/flailerdb.cpp
    indep/src/fmenus.cpp
    indep/src/helisheet.cpp
    indep/src/mellowmover.cpp
    indep/src/point.cpp
    indep/src/polyhedra.cpp
    indep/src/psychodriver.cpp
    indep/src/pursuit.cpp
    indep/src/quickspline.cpp
    indep/src/race.cpp
    indep/src/raceparameters.cpp
    indep/src/realdriver.cpp
    indep/src/rigidbody.cpp
    indep/src/skids.cpp
    indep/src/spikestrip.cpp
    indep/src/spline.cpp
    indep/src/suspension.cpp
    indep/src/table.cpp
    indep/src/track.cpp
    indep/src/trackroute.cpp
    indep/src/trafficdriver.cpp
    indep/src/volumetree.cpp
    indep/src/voronoiclip.cpp
    indep/src/wheel.cpp
    indep/src/world.cpp
    src/ecstasy/emathe.cpp
    src/joysticke.cpp
)





target_sources(NFSHP2Engine PRIVATE
    bsound/src/bEffect.cpp
    bsound/src/bVoice.cpp
    cmn/spabort.c
    cmn/spdprint.c
    cmn/sptick.c
    cmn/sputil.c
    indep/src/aidriver.cpp
    indep/src/beltmover.cpp
    indep/src/carcontroller.cpp
    indep/src/deliverymover.cpp
    indep/src/frontendscreens/endgamescreens.cpp
    indep/inc/EndGameScreens.hpp
    indep/src/frontendscreens/pausemenu.cpp
    indep/src/frontendscreens/startracescreens.cpp
    indep/src/speech/speechcontext.cpp
    indep/src/trafficcar.cpp
)


target_sources(NFSHP2Engine PRIVATE
    bsound/src/bDriverLowPS2.cpp
    bsound/src/bDriverPS2.cpp
    src/logitechwheel.cpp
)


target_sources(NFSHP2Engine PRIVATE
    indep/src/clans.cpp
    src/texturee.cpp
)


# Individually proven native save/database owners.
target_sources(NFSHP2Engine PRIVATE src/memorycard.cpp src/dbfiles.cpp indep/src/frontendscreens/memcardscreens.cpp)
target_sources(NFSHP2Engine PRIVATE indep/src/camera.cpp)

target_sources(NFSHP2Engine PRIVATE
    indep/src/trackmap.cpp
)

target_include_directories(NFSHP2Engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/indep/inc")
target_link_libraries(NFSHP2Engine PUBLIC NFSHP2Core)
if(WIN32)
    target_compile_definitions(NFSHP2Core PUBLIC UNICODE _UNICODE)
    if(MINGW)
        target_link_libraries(NFSHP2Engine PUBLIC xaudio2_8 ole32 uuid)
    else()
        target_link_libraries(NFSHP2Engine PUBLIC xaudio2 ole32)
    endif()
endif()
# Native PC float32 arithmetic: retain explicit expression ordering without
# strict software-conforming FP state-machine overhead or unsafe fast-math.
foreach(target IN ITEMS NFSHP2Core NFSHP2Engine)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
endforeach()

foreach(target IN ITEMS NFSHP2Core NFSHP2Engine)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        # C-linkage engine entry points implemented in C++ can propagate exceptions.
        target_compile_options(${target} PRIVATE /W4 /WX /EHsc- /utf-8)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Keep layout/observer members and virtual slots. Clang 23
            # diagnoses these more broadly than the required GCC/MSVC gates.
            # All other diagnostics remain errors in this auxiliary cross-build.
            target_compile_options(${target} PRIVATE -Wno-unused-const-variable
                -Wno-unnecessary-virtual-specifier -Wno-unused-private-field
                -Wno-unused-but-set-global)
        endif()
    endif()
endforeach()

set(HP2_VULKAN_DEFAULT ${WIN32})
if(NINTENDO_SWITCH)
    set(HP2_VULKAN_DEFAULT ON)
endif()
option(HP2_ENABLE_VULKAN "Build the physical Vulkan graphics backend" ${HP2_VULKAN_DEFAULT})
if(HP2_ENABLE_VULKAN)
    include(cmake/Vulkan.cmake)
    target_compile_definitions(NFSHP2Core PUBLIC HP2_ENABLE_VULKAN=1)
    target_link_libraries(NFSHP2Core PUBLIC Vulkan::Vulkan)
endif()
if(WIN32 AND HP2_ENABLE_VULKAN)
    target_link_libraries(NFSHP2Core PUBLIC shell32 user32 gdi32 comctl32)
    enable_language(RC)
    add_executable(NFSHP2 WIN32 src/platform.cpp src/resources/NFSHP2.rc)
    target_include_directories(NFSHP2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/resources")
    target_compile_definitions(NFSHP2 PRIVATE HP2_APPLICATION_ENTRY=1)
    if(MSVC)
        target_link_libraries(NFSHP2 PRIVATE NFSHP2Engine NFSHP2Core)
    else()
        target_link_libraries(NFSHP2 PRIVATE "$<LINK_GROUP:RESCAN,NFSHP2Engine,NFSHP2Core>")
    endif()
    if(MSVC)
        target_compile_options(NFSHP2 PRIVATE /W4 /WX /EHsc- /utf-8)
        set_property(TARGET NFSHP2 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(NFSHP2 PRIVATE -municode)
    endif()
    add_custom_command(TARGET NFSHP2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:NFSHP2>/SDL2.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2/LICENSE.txt" "$<TARGET_FILE_DIR:NFSHP2>/SDL2-LICENSE.txt"
        COMMENT "Copying the native SDL2 controller runtime")
endif()

# Standalone resource utility: no renderer, audio runtime or Python dependency.
if(NINTENDO_SWITCH)
    include(cmake/Switch.cmake)
else()
    add_subdirectory(tools/archive)
endif()

# Verification is available only in a development checkout.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/Development.cmake")
    option(HP2_BUILD_TESTS "Build development verification tools" OFF)
    if(HP2_BUILD_TESTS)
        include(tests/Development.cmake)
    endif()
endif()
