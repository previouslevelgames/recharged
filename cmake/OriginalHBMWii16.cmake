include_guard(GLOBAL)

# Original retail library profile and Wii16 ABI; no game admission.
# Apply before any HBM inventory/module: every original HBM source that shares
# its classes, inlines or wide literals must use the same isolated Wii16 ABI.
include(cmake/WiiStringFormat.cmake)
include(cmake/OriginalHBMRetailProfile.cmake)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    message(FATAL_ERROR "Original HBM Wii16 currently requires GNU/Clang")
endif()
file(GLOB_RECURSE _charged_hbm_wii16_sources CONFIGURE_DEPENDS
    "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/*.cpp")
foreach(_source IN LISTS _charged_hbm_wii16_sources)
    set_property(SOURCE "${_source}" APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
endforeach()
function(mscharged_select_original_hbm_text_transport target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "HBM text transport belongs to the sole original game module")
    endif()
    get_target_property(_existing "${target}" SOURCES)
    if(NOT "src/platform/hbm_text_transport.cpp" IN_LIST _existing)
        target_sources("${target}" PRIVATE src/platform/hbm_text_transport.cpp)
    endif()
    target_link_libraries("${target}" PRIVATE charged_wii_msl)
endfunction()
