include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/OriginalMEMExpHeap.cmake")

# Whole original RFL system/resource code, with serialized-byte and MEM lifetime
# transport. This compiler inventory does not admit Mii initialization, raw DB
# records, uncached NAND buffers or native rendering. Consumers share the actual
# source module's completion/allocator registry; never a separately owned bridge.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
add_library(charged_original_rfl_resource_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_System.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_NANDLoader.c"
    src/platform/rfl_resource_transport.cpp
    src/platform/rfl_temp_memory.cpp)
add_dependencies(charged_original_rfl_resource_sources verify_prepared)
set_target_properties(charged_original_rfl_resource_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_rfl_resource_sources PRIVATE
    c_std_17 cxx_std_20)
target_include_directories(charged_original_rfl_resource_sources PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_rfl_resource_sources PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_options(charged_original_rfl_resource_sources PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fsigned-char
    -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:C>:-fexceptions;-Werror=pointer-to-int-cast;-Werror=implicit-function-declaration>")

function(mscharged_add_original_rfl_resource target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original RFL resource transport must share its source module's allocation/completion registry")
    endif()
    mscharged_add_original_mem_exp_heap("${target}")
    get_target_property(_sources "${target}" SOURCES)
    foreach(_path IN ITEMS
            "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_System.c"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_NANDLoader.c"
            "${PROJECT_SOURCE_DIR}/src/platform/rfl_resource_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/platform/rfl_temp_memory.cpp")
        file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_path}")
        if(NOT _path IN_LIST _sources AND NOT _relative IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
    set_property(SOURCE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_System.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_NANDLoader.c"
        TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
        -fexceptions -Werror=pointer-to-int-cast
        -Werror=implicit-function-declaration)
endfunction()
