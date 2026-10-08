include_guard(GLOBAL)
include(cmake/OriginalNativeCompilerProfile.cmake)
include(cmake/OriginalModuleLinkage.cmake)
include(cmake/NativeHBMDebug.cmake)

# One explicit test module owns the whole HBM task/MEM/lock source. Additional
# capacity tests attach to this module, rather than loading a second registry.
# This helper adds no source main call, HBM initializer or worker to the game.
function(mscharged_add_original_hbm_task_test_exports target)
    get_target_property(_exports "${target}" MSCHARGED_HBM_TASK_TEST_EXPORTS)
    if(NOT _exports)
        set(_exports)
    endif()
    list(APPEND _exports ${ARGN})
    list(REMOVE_DUPLICATES _exports)
    set(_map "{\n  global:\n")
    foreach(_name IN LISTS _exports)
        if(NOT _name MATCHES "^charged_hbm_task_[A-Za-z0-9_]+$")
            message(FATAL_ERROR "HBM task test exports must name an explicit test interface: ${_name}")
        endif()
        string(APPEND _map "    ${_name};\n")
    endforeach()
    string(APPEND _map "  local: *;\n};\n")
    set_property(TARGET "${target}" PROPERTY MSCHARGED_HBM_TASK_TEST_EXPORTS "${_exports}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    file(CONFIGURE OUTPUT "${_path}" CONTENT "${_map}" @ONLY)
    mscharged_set_original_module_exports("${target}" "${_path}")
endfunction()

function(mscharged_add_original_hbm_task_lifecycle_tests)
    if(NOT BUILD_TESTING)
        return()
    endif()
    mscharged_original_native_profile_supported(_profile C CXX)
    if(NOT _profile)
        return()
    endif()
    if(TARGET mscharged_original_hbm_task_module)
        return()
    endif()
    if(NOT TARGET charged_original_os_messages)
        include(cmake/OriginalOSMessages.cmake)
    endif()

    add_library(mscharged_original_hbm_task_module MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_TaskThread.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_TaskManager.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_LinkList.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_unitHeap.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_heapCommon.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_list.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMutex.c"
        src/platform/mem_logical_heap.cpp
        src/platform/game_allocation_ownership.cpp
        tests/diagnostics/original_hbm_task_thread.cpp)
    mscharged_select_original_hbm_debug(mscharged_original_hbm_task_module)
    add_dependencies(mscharged_original_hbm_task_module verify_prepared)
    set_target_properties(mscharged_original_hbm_task_module PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_features(mscharged_original_hbm_task_module PRIVATE c_std_17 cxx_std_20)
    target_include_directories(mscharged_original_hbm_task_module PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(mscharged_original_hbm_task_module PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1 HBM_ASSERT=1)
    target_compile_options(mscharged_original_hbm_task_module PRIVATE
        -ffunction-sections -fdata-sections -fno-strict-aliasing -fsigned-char
        -Wno-unknown-pragmas
        "$<$<COMPILE_LANGUAGE:C>:-fexceptions;-Werror=pointer-to-int-cast;-Werror=implicit-function-declaration>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>")
    set_property(SOURCE tests/diagnostics/original_hbm_task_thread.cpp
        TARGET_DIRECTORY mscharged_original_hbm_task_module
        APPEND PROPERTY COMPILE_OPTIONS -fno-access-control)
    if(APPLE)
        target_link_options(mscharged_original_hbm_task_module PRIVATE
            "LINKER:-undefined,dynamic_lookup" "LINKER:-dead_strip")
    else()
        target_link_options(mscharged_original_hbm_task_module PRIVATE
            "LINKER:--gc-sections" "LINKER:-Bsymbolic-functions")
    endif()
    mscharged_add_original_hbm_task_test_exports(mscharged_original_hbm_task_module
        charged_hbm_task_thread_owner charged_hbm_task_thread_create
        charged_hbm_task_thread_destroy charged_hbm_task_thread_wake
        charged_hbm_task_manager_owner charged_hbm_task_free_blocks
        charged_hbm_task_block_size)

    add_executable(original_hbm_task_lifecycle_tests
        tests/original_hbm_task_lifecycle.cpp src/platform/host_metadata.cpp)
    add_dependencies(original_hbm_task_lifecycle_tests mscharged_original_hbm_task_module)
    target_compile_features(original_hbm_task_lifecycle_tests PRIVATE cxx_std_20)
    mscharged_link_original_hbm_debug_host(original_hbm_task_lifecycle_tests CPU_FIXTURE)
    target_compile_definitions(original_hbm_task_lifecycle_tests PRIVATE HBM_ASSERT=1)
    target_compile_options(original_hbm_task_lifecycle_tests PRIVATE -fno-access-control)
    target_link_libraries(original_hbm_task_lifecycle_tests PRIVATE
        charged_original_os_messages "${CMAKE_DL_LIBS}")
    if(APPLE)
        target_link_options(original_hbm_task_lifecycle_tests PRIVATE "LINKER:-export_dynamic")
    else()
        target_link_options(original_hbm_task_lifecycle_tests PRIVATE "LINKER:--export-dynamic")
    endif()
    foreach(_symbol IN ITEMS OSInitMessageQueue OSSendMessage OSReceiveMessage
            ChargedNativeMetadataRelease)
        mscharged_require_original_host_symbol(original_hbm_task_lifecycle_tests PRIVATE "${_symbol}")
    endforeach()
    add_test(NAME original_hbm_task_lifecycle COMMAND original_hbm_task_lifecycle_tests
        "$<TARGET_FILE:mscharged_original_hbm_task_module>")
    set_tests_properties(original_hbm_task_lifecycle PROPERTIES TIMEOUT 15 LABELS "Platform")
endfunction()
