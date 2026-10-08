include_guard(GLOBAL)
include(cmake/OriginalHBMWii16.cmake)

# Original ASCII resource-name comparison. Keep its MSL locale and libc names
# inside the isolated module; the genuine C source needs no C++ FILE adaptation.
add_library(charged_original_hbm_string_extras STATIC EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/MSL/extras.c")
set_target_properties(charged_original_hbm_string_extras PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_compile_features(charged_original_hbm_string_extras PRIVATE c_std_11)
target_include_directories(charged_original_hbm_string_extras PRIVATE
    "${MSCHARGED_PREPARED}/libs/MSL_C/include"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_PREPARED}/libs/MetroTRK/include")
target_compile_definitions(charged_original_hbm_string_extras PRIVATE
    MSCHARGED_NATIVE=1 _current_locale=ChargedWii__current_locale)
target_compile_options(charged_original_hbm_string_extras PRIVATE
    -fshort-wchar -fsigned-char -fgnu89-inline -ffunction-sections -fdata-sections)
target_link_libraries(charged_original_hbm_string_extras PRIVATE charged_wii_msl)
add_dependencies(charged_original_hbm_string_extras verify_prepared)

# Provider selection only. The caller already owns original ARC + its serialized
# bridge and the sole allocation registry. No HomeButton/create/show admission.
function(mscharged_select_original_hbm_arc_resources target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HBM ARC resources require the isolated game module")
    endif()
    get_target_property(_existing "${target}" SOURCES)
    foreach(_relative IN ITEMS
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_arcResourceAccessor.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_resourceAccessor.cpp
        src/RVL_SDK/hbm/nw4hbm/ut/ut_LinkList.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _existing "${_source}")
        endif()
    endforeach()
    target_link_libraries("${target}" PRIVATE charged_original_hbm_string_extras)
endfunction()
