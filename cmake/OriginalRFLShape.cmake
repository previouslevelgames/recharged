include_guard(GLOBAL)

# Whole original RFL model and bounded serialized shape/array transport.
# This compiler inventory does not admit source model construction or drawing.
# Runtime consumers must share their module's actual ownership registry.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
add_library(charged_original_rfl_shape_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_Model.c"
    src/platform/rfl_shape_transport.cpp)
add_dependencies(charged_original_rfl_shape_sources verify_prepared)
set_target_properties(charged_original_rfl_shape_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_rfl_shape_sources PRIVATE c_std_17 cxx_std_20)
target_include_directories(charged_original_rfl_shape_sources PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_rfl_shape_sources PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_options(charged_original_rfl_shape_sources PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fsigned-char
    -ffp-contract=off
    -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:C>:-fexceptions;-Werror=pointer-to-int-cast;-Werror=implicit-function-declaration>")

function(mscharged_add_original_rfl_shape target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original RFL shape transport must share its source module's allocation/completion registry")
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_path IN ITEMS
            "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_Model.c"
            "${PROJECT_SOURCE_DIR}/src/platform/rfl_shape_transport.cpp")
        file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_path}")
        if(NOT _path IN_LIST _sources AND NOT _relative IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
    # The C consumer must unwind a real native bounds/domain exception. No C++
    # source algorithms or error callbacks are extracted into the host here.
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_Model.c"
        TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
        -fexceptions -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
endfunction()
