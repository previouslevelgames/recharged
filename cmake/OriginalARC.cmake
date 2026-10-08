include_guard(GLOBAL)

# Whole original archive APIs and bounded Wii serialized-byte/address transport.
# This compiler inventory does not admit Mii/RFL startup or a source readiness
# predicate. Runtime consumers must share their module's one allocation registry.
include("${CMAKE_CURRENT_LIST_DIR}/OriginalNativeCompilerProfile.cmake")
mscharged_original_native_profile_supported(_original_native_profile C CXX)
if(NOT _original_native_profile)
    return()
endif()
add_library(charged_original_arc_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/arc/arc.c"
    src/platform/arc_data_transport.cpp)
add_dependencies(charged_original_arc_sources verify_prepared)
set_target_properties(charged_original_arc_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_arc_sources PRIVATE c_std_17 cxx_std_20)
target_include_directories(charged_original_arc_sources PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_arc_sources PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_options(charged_original_arc_sources PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fsigned-char
    -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:C>:-fexceptions;-Werror=pointer-to-int-cast;-Werror=implicit-function-declaration>")

function(mscharged_add_original_arc target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original ARC must share its source module's allocation/completion registry")
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_path IN ITEMS
            "${MSCHARGED_PREPARED}/src/RVL_SDK/arc/arc.c"
            "${PROJECT_SOURCE_DIR}/src/platform/arc_data_transport.cpp")
        file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_path}")
        if(NOT _path IN_LIST _sources AND NOT _relative IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
    # The C consumer must unwind a real native bounds/domain exception. No C++
    # source algorithms or error callbacks are extracted into the host here.
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/arc/arc.c"
        TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
        -fexceptions -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
endfunction()
