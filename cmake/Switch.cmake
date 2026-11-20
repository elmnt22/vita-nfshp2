if(NOT HP2_ENABLE_VULKAN)
    message(FATAL_ERROR "The Nintendo Switch port requires the native Vulkan renderer")
endif()

# MSYS converts the library paths on the command line for devkitA64.
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_LIBRARIES 0)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)

find_path(HP2_SWITCH_PNG_INCLUDE png.h REQUIRED)
find_library(HP2_SWITCH_PNG_LIBRARY png REQUIRED)
target_sources(NFSHP2Core PRIVATE src/nx/controller_art.cpp)
target_include_directories(NFSHP2Core PRIVATE "${HP2_SWITCH_PNG_INCLUDE}")
target_link_libraries(NFSHP2Core PRIVATE "${HP2_SWITCH_PNG_LIBRARY}" z)

target_sources(NFSHP2Core PRIVATE src/nx/config.cpp src/nx/display_options.cpp src/nx/display.cpp)

add_executable(NFSHP2 src/nx/main.cpp src/nx/audio.cpp src/nx/pgo.cpp)
option(HP2_SWITCH_DIAGNOSTICS "Write Switch diagnostic logs and detailed timing summaries" OFF)
if(NOT HP2_SWITCH_DIAGNOSTICS)
    foreach(hp2_target IN ITEMS NFSHP2Core NFSHP2Engine NFSHP2)
        target_compile_definitions(${hp2_target} PRIVATE HP2_DISABLE_DIAGNOSTICS=1)
    endforeach()
endif()
include(cmake/SwitchOptimization.cmake)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
target_link_libraries(NFSHP2 PRIVATE "$<LINK_GROUP:RESCAN,NFSHP2Engine,NFSHP2Core>")
if(NINTENDO_SWITCH2)
    target_sources(NFSHP2 PRIVATE src/nx/threads.cpp)
    target_link_options(NFSHP2 PRIVATE "LINKER:--wrap=pthread_create" "LINKER:-u,__wrap_pthread_create")
    set_target_properties(NFSHP2 PROPERTIES OUTPUT_NAME "NFSHP2-Switch2")
    set(hp2_nacp_name "Need for Speed: Hot Pursuit 2 - Switch 2")
else()
    set_target_properties(NFSHP2 PROPERTIES OUTPUT_NAME "NFSHP2-Switch")
    set(hp2_nacp_name "Need for Speed: Hot Pursuit 2")
endif()
nx_generate_nacp(OUTPUT NFSHP2.nacp NAME "${hp2_nacp_name}"
    AUTHOR "HDErick" VERSION "1.0")
find_program(HP2_ICON_PYTHON NAMES python3 python REQUIRED)
set(HP2_PIPELINE_CACHE_SEED "${CMAKE_CURRENT_SOURCE_DIR}/VulkanPipelineCache.bin")
set(HP2_PIPELINE_CACHE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pipeline_cache_seed.inc")
if(NOT NINTENDO_SWITCH2)
    add_custom_command(OUTPUT "${HP2_PIPELINE_CACHE_HEADER}"
        COMMAND "${HP2_ICON_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/nx/embed_pipeline_cache.py"
            "${HP2_PIPELINE_CACHE_SEED}" "${HP2_PIPELINE_CACHE_HEADER}"
        DEPENDS "${HP2_PIPELINE_CACHE_SEED}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/nx/embed_pipeline_cache.py"
        VERBATIM)
    target_sources(NFSHP2Core PRIVATE "${HP2_PIPELINE_CACHE_HEADER}")
endif()
target_sources(NFSHP2Core PRIVATE src/nx/pipeline_cache.cpp)
set(HP2_BUTTONS_PNG "${CMAKE_CURRENT_SOURCE_DIR}/buttons.png")
set(HP2_BUTTONS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/buttons_png.inc")
add_custom_command(OUTPUT "${HP2_BUTTONS_HEADER}"
    COMMAND "${HP2_ICON_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/nx/embed_buttons.py"
        "${HP2_BUTTONS_PNG}" "${HP2_BUTTONS_HEADER}"
    DEPENDS "${HP2_BUTTONS_PNG}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/nx/embed_buttons.py"
    VERBATIM)
target_sources(NFSHP2Core PRIVATE "${HP2_BUTTONS_HEADER}")
target_include_directories(NFSHP2Core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set(HP2_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
set(HP2_ICON_CONVERTER "${CMAKE_CURRENT_SOURCE_DIR}/tools/nx/convert_icon.py")
set(HP2_NRO_ICON "${CMAKE_CURRENT_BINARY_DIR}/NFSHP2-icon.jpg")
add_custom_command(OUTPUT "${HP2_NRO_ICON}"
    COMMAND "${HP2_ICON_PYTHON}" "${HP2_ICON_CONVERTER}" "${HP2_ICON_SOURCE}" "${HP2_NRO_ICON}"
    DEPENDS "${HP2_ICON_SOURCE}" "${HP2_ICON_CONVERTER}"
    VERBATIM)
nx_create_nro(NFSHP2 ICON "${HP2_NRO_ICON}" NACP NFSHP2.nacp)
