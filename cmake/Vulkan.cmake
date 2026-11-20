# The Windows checkout includes the Khronos C headers and a loader import
# library. Driver/runtime installation belongs to the graphics driver.
if(NINTENDO_SWITCH2)
    set(HP2_SWITCH2_MESA_SDK "" CACHE PATH "External Switch 2 Horizon Mesa SDK (portlibs/switch2 directory)")
    if(NOT EXISTS "${HP2_SWITCH2_MESA_SDK}/lib/libmesa_vulkan.a")
        message(FATAL_ERROR "Set HP2_SWITCH2_MESA_SDK to an external Switch 2 Mesa SDK; drivers are not vendored")
    endif()
    find_package(Vulkan REQUIRED CONFIG PATHS "${HP2_SWITCH2_MESA_SDK}/lib/cmake/Vulkan" NO_DEFAULT_PATH)
    # libpng comes from Switch 1 portlibs, which also contains Vulkan headers.
    # Keep the external Switch 2 SDK headers ahead of that shared include path.
    set_property(TARGET Vulkan::Vulkan PROPERTY IMPORTED_NO_SYSTEM TRUE)
    target_include_directories(NFSHP2Core BEFORE PUBLIC "${HP2_SWITCH2_MESA_SDK}/include")
    # The SDK's libvulkan.a is an ld script; resolve its archives ahead of OunceVK.
    target_link_directories(NFSHP2Core PUBLIC "${HP2_SWITCH2_MESA_SDK}/lib" "${DEVKITPRO}/portlibs/switch/lib")
elseif(NINTENDO_SWITCH)
    find_path(HP2_SWITCH_VULKAN_INCLUDE vulkan/vulkan_vi.h REQUIRED)
    find_library(HP2_SWITCH_VULKAN_LIBRARY vulkan REQUIRED)
    add_library(Vulkan::Vulkan STATIC IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${HP2_SWITCH_VULKAN_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HP2_SWITCH_VULKAN_INCLUDE}"
        INTERFACE_LINK_LIBRARIES "elf;expat;z;zstd;nx;m")
elseif(WIN32 AND MSVC)
    set(_hp2_vulkan "${CMAKE_CURRENT_LIST_DIR}/../third_party/vulkan")
    foreach(_hp2_file IN ITEMS include/vulkan/vulkan.h lib/x64/vulkan-1.lib)
        if(NOT EXISTS "${_hp2_vulkan}/${_hp2_file}")
            message(FATAL_ERROR "Missing pinned Vulkan dependency: third_party/vulkan/${_hp2_file}")
        endif()
    endforeach()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The native Windows port requires an x64 configuration")
    endif()
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${_hp2_vulkan}/lib/x64/vulkan-1.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_hp2_vulkan}/include")
    unset(_hp2_vulkan)
    unset(_hp2_file)
else()
    find_package(Vulkan 1.1 REQUIRED)
endif()
