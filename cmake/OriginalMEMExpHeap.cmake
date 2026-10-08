include_guard(GLOBAL)

# Whole original MEM Exp/common/list algorithms. Native headers are separate
# metadata attached to the same source module's actual NL allocation owner.
# This compiler inventory grants no RFL/game readiness or contended scheduler.
include("${CMAKE_CURRENT_LIST_DIR}/OriginalNativeCompilerProfile.cmake")
mscharged_original_native_profile_supported(_original_native_profile C CXX)
if(NOT _original_native_profile)
    return()
endif()
add_library(charged_original_mem_exp_heap_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_expHeap.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_heapCommon.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_list.c"
    src/platform/mem_logical_heap.cpp)
add_dependencies(charged_original_mem_exp_heap_sources verify_prepared)
set_target_properties(charged_original_mem_exp_heap_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_mem_exp_heap_sources PRIVATE c_std_17 cxx_std_20)
target_include_directories(charged_original_mem_exp_heap_sources PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_mem_exp_heap_sources PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_options(charged_original_mem_exp_heap_sources PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fsigned-char
    -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:C>:-fexceptions;-Werror=pointer-to-int-cast;-Werror=implicit-function-declaration>")

function(mscharged_add_original_mem_exp_heap target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original MEM logical storage must share its source module's allocation registry")
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_path IN ITEMS
            "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_expHeap.c"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_heapCommon.c"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_list.c"
            "${PROJECT_SOURCE_DIR}/src/platform/mem_logical_heap.cpp")
        file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_path}")
        if(NOT _path IN_LIST _sources AND NOT _relative IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
    set_property(SOURCE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_expHeap.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_heapCommon.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_list.c"
        TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
        -fexceptions -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
endfunction()
