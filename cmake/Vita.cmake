# PS Vita host (spike). Stage 1: CPU-only null renderer to measure the
# engine's logic + capture cost on the Cortex-A9. Users supply their own PAL
# ZDIR.BIN/ZZDATA.BIN in ux0:data/NFSHP2; no game data is packaged.
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "NFS HP2 Native")
set(VITA_TITLEID  "NFSHP2002")
set(VITA_VERSION  "00.01")

foreach(hp2_target IN ITEMS NFSHP2Core NFSHP2Engine)
    target_compile_options(${hp2_target} PRIVATE -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
endforeach()

# Stage 2: vitaGL renderer for the shared native Gather (ecstasy_gl.inc).
option(HP2_VITA_GL "Build the vitaGL renderer (OFF = stage-1 null renderer)" ON)
set(HP2_VITA_GL_LIBS)
if(HP2_VITA_GL)
    target_compile_definitions(NFSHP2Core PUBLIC HP2_ENABLE_VITAGL=1)
    # The shared renderer helpers live in an anonymous namespace (the Vulkan
    # Storage is MSVC/Switch-only); some Vulkan-only helpers are unused here.
    set_source_files_properties(src/ecstasy/ecstasye.cpp PROPERTIES COMPILE_OPTIONS
        "-Wno-subobject-linkage;-Wno-unused-function;-Wno-overflow")
    # Own vitaGL build (native/deps/vitaGL_hp2: NO_DEBUG=1 HAVE_VERTEX_LAYOUT_CACHE=1
    # + HP2 fast paths); OFF links the stock VitaSDK library.
    option(HP2_OWN_VITAGL "Link native/deps/vitaGL_hp2/libvitaGL.a" ON)
    set(HP2_VITAGL_LIB vitaGL)
    set(HP2_OWN_VITAGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/vitaGL_hp2")
    if(HP2_OWN_VITAGL AND EXISTS "${HP2_OWN_VITAGL_DIR}/libvitaGL.a")
        set(HP2_VITAGL_LIB "${HP2_OWN_VITAGL_DIR}/libvitaGL.a")
        target_include_directories(NFSHP2Core BEFORE PUBLIC "${HP2_OWN_VITAGL_DIR}/source")
        message(STATUS "HP2: own vitaGL ${HP2_VITAGL_LIB}")
    endif()
    set(HP2_VITA_GL_LIBS ${HP2_VITAGL_LIB} vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
        SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub
        SceIofilemgr_stub mathneon)
endif()

# kubridge crash handler: weak imports, so the eboot loads without kubridge.skprx.
enable_language(ASM)
add_executable(NFSHP2 src/vita/main.cpp src/vita/audio.cpp src/vita/selftest.cpp src/vita/fastclock.cpp src/vita/crash.cpp src/vita/kubridge/kubridge_stub.S)
target_compile_options(NFSHP2 PRIVATE -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
target_link_libraries(NFSHP2 PRIVATE -Wl,--wrap=_ZNSt6chrono3_V212steady_clock3nowEv
    -Wl,--start-group NFSHP2Engine NFSHP2Core ${HP2_VITA_GL_LIBS} -Wl,--end-group
    SceCtrl_stub ScePower_stub SceAppUtil_stub SceSysmodule_stub
    SceKernelThreadMgr_stub SceRtc_stub SceAudio_stub
    -Wl,--whole-archive pthread -Wl,--no-whole-archive m)

# Whole-program optimization: the engine is C-style with many tiny cross-TU
# helpers (bScalar*, vector ops, accessors) that only LTO can inline.
option(HP2_VITA_LTO "Link-time optimization for the Vita build" ON)
set(HP2_VITA_LTO_JOBS "4" CACHE STRING "Parallel LTO jobs for the Vita link")
if(HP2_VITA_LTO)
    set_property(TARGET NFSHP2Core NFSHP2Engine NFSHP2 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    target_link_options(NFSHP2 PRIVATE "-flto=${HP2_VITA_LTO_JOBS}" -flto-partition=balanced
        -ffp-contract=off -fno-fast-math -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
endif()

vita_create_self(eboot.bin NFSHP2 UNSAFE)
vita_create_vpk(NFSHP2.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME})
