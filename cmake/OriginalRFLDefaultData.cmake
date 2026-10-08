include_guard(GLOBAL)

# Genuine original default-input boundary with local native representation.
# No RFL initialization, persisted character/CRC or manager readiness admission.
include("${CMAKE_CURRENT_LIST_DIR}/OriginalNativeCompilerProfile.cmake")
mscharged_original_native_profile_supported(_original_native_profile C)
if(NOT _original_native_profile)
    return()
endif()
add_library(charged_original_rfl_default_data OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_DefaultDatabase.c"
    "${PROJECT_SOURCE_DIR}/src/platform/rfl_character_transport.c")
add_dependencies(charged_original_rfl_default_data verify_prepared)
set_target_properties(charged_original_rfl_default_data PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_compile_features(charged_original_rfl_default_data PRIVATE c_std_17)
target_include_directories(charged_original_rfl_default_data PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_rfl_default_data PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_options(charged_original_rfl_default_data PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fsigned-char
    -fexceptions -Wno-unknown-pragmas -Werror=pointer-to-int-cast
    -Werror=implicit-function-declaration)

function(mscharged_add_original_rfl_default_data target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original RFL default inputs must share the module's native record types")
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_path IN ITEMS
            "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_DefaultDatabase.c"
            "${PROJECT_SOURCE_DIR}/src/platform/rfl_character_transport.c")
        file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_path}")
        if(NOT _path IN_LIST _sources AND NOT _relative IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
        set_property(SOURCE "${_path}" TARGET_DIRECTORY "${target}" APPEND
            PROPERTY COMPILE_OPTIONS -fexceptions -Werror=pointer-to-int-cast
            -Werror=implicit-function-declaration)
    endforeach()
endfunction()
