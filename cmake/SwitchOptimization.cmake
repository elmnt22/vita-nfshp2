option(HP2_SWITCH_LTO "Enable link-time optimization for the Nintendo Switch build" OFF)
set(HP2_SWITCH_PGO "off" CACHE STRING "Switch GCC PGO mode: off, generate, or use")
set_property(CACHE HP2_SWITCH_PGO PROPERTY STRINGS off generate use)
set(HP2_SWITCH_LTO_JOBS "6" CACHE STRING "Parallel GCC link-time optimization jobs")

if(NOT HP2_SWITCH_PGO MATCHES "^(off|generate|use)$")
    message(FATAL_ERROR "HP2_SWITCH_PGO must be off, generate, or use")
endif()

set(hp2_switch_targets NFSHP2Core NFSHP2Engine NFSHP2)
set(hp2_switch_build_mode baseline)
if(HP2_SWITCH_LTO)
    if(NOT HP2_SWITCH_LTO_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "HP2_SWITCH_LTO_JOBS must be a positive integer")
    endif()
    set_property(TARGET ${hp2_switch_targets} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    target_link_options(NFSHP2 PRIVATE "-flto=${HP2_SWITCH_LTO_JOBS}" -flto-partition=balanced
        -ffp-contract=off -fno-fast-math)
    set(hp2_switch_build_mode lto)
endif()

if(NOT HP2_SWITCH_PGO STREQUAL "off")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "Switch PGO requires devkitA64 GCC")
    endif()

    # GCC's prefix must match its native object path. devkitA64 is a Windows
    # compiler in devkitPro MSYS2 and uses backslashes in the absolute prefix.
    set(hp2_profile_prefix "${CMAKE_BINARY_DIR}")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "MSYS")
        execute_process(COMMAND cygpath -w "${CMAKE_BINARY_DIR}"
            OUTPUT_VARIABLE hp2_profile_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
    elseif(CMAKE_HOST_WIN32)
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" hp2_profile_prefix)
    endif()

    # Profile capture and Gather without instrumenting the rest of the game.
    set(hp2_profile_core_sources src/ecstasy/ecstasye.cpp)
    set(hp2_profile_engine_sources indep/src/main.cpp indep/src/scenery.cpp src/care.cpp)

    # A relative output path makes the streams independent of drive and
    # checkout location. Merge them from the profile-use build directory.
    if(HP2_SWITCH_PGO STREQUAL "generate")
        set(hp2_profile_flags -fprofile-generate=profile-data -fno-profile-values
            -fprofile-update=single -fprofile-info-section=hp2_gcov_info)
        # GNU ld retains this section through its referenced start and stop symbols.
        target_link_options(NFSHP2 PRIVATE "LINKER:-z,nostart-stop-gc" "LINKER:-z,start-stop-visibility=hidden")
        set_property(SOURCE src/nx/pgo.cpp APPEND PROPERTY COMPILE_DEFINITIONS HP2_PGO_GENERATE=1)
    else()
        file(GLOB hp2_profile_data "${CMAKE_BINARY_DIR}/profile-data/*.gcda")
        if(NOT hp2_profile_data)
            message(FATAL_ERROR "Merge a Switch training stream into this build directory before configuring PGO use")
        endif()
        foreach(hp2_target IN ITEMS Core Engine)
            if(hp2_target STREQUAL "Core")
                set(hp2_target_sources ${hp2_profile_core_sources})
            else()
                set(hp2_target_sources ${hp2_profile_engine_sources})
            endif()
            foreach(hp2_source IN LISTS hp2_target_sources)
                string(REPLACE "/" "#" hp2_profile_file "${hp2_source}")
                string(REGEX REPLACE "[.]cpp$" ".gcda" hp2_profile_file "${hp2_profile_file}")
                set(hp2_required_profile "CMakeFiles#NFSHP2${hp2_target}.dir#${hp2_profile_file}")
                if(NOT EXISTS "${CMAKE_BINARY_DIR}/profile-data/${hp2_required_profile}")
                    message(FATAL_ERROR "Missing Switch training profile: ${hp2_required_profile}")
                endif()
                # Recompile the trained object when merged gameplay counts change.
                set_property(SOURCE "${hp2_source}" APPEND PROPERTY OBJECT_DEPENDS
                    "${CMAKE_BINARY_DIR}/profile-data/${hp2_required_profile}")
            endforeach()
        endforeach()
        # Smooth worker counters. GCC reports and discards stale function
        # profiles after capture/LOD edits, while retaining compatible counts.
        set(hp2_profile_flags -fprofile-use=profile-data -fno-profile-values
            -fprofile-correction -fprofile-partial-training -Wno-error=missing-profile -Wno-error=coverage-mismatch)
    endif()
    list(APPEND hp2_profile_flags "-fprofile-prefix-path=${hp2_profile_prefix}")
    set_property(SOURCE ${hp2_profile_core_sources} ${hp2_profile_engine_sources}
        APPEND PROPERTY COMPILE_OPTIONS ${hp2_profile_flags})
    set(hp2_switch_build_mode "pgo-${HP2_SWITCH_PGO}-renderer")
    if(HP2_SWITCH_PGO STREQUAL "generate")
        target_link_options(NFSHP2 PRIVATE ${hp2_profile_flags})
    elseif(HP2_SWITCH_LTO)
        # devkitA64 GCC 16.1 crashes at whole-program analysis when the
        # profile-use objects participate. Compile those objects with PGO
        # and link the other objects with LTO.
        set_property(SOURCE ${hp2_profile_core_sources} ${hp2_profile_engine_sources}
            APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
        set(hp2_switch_build_mode "pgo-use-renderer-hybrid")
    endif()
endif()
set_property(SOURCE src/nx/pgo.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    HP2_SWITCH_BUILD_MODE="${hp2_switch_build_mode}")
