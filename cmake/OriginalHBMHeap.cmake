include_guard(GLOBAL)
include(cmake/NativeHBMDebug.cmake)
include(cmake/NativeThreadQueues.cmake)

# Bounded original allocator methods. No HBMCreate/player/SaveState admission.
# The source's native sizeof/alignof fields live in caller-owned test storage.
if(BUILD_TESTING)
    add_executable(native_hbm_heap_tests tests/native_hbm_heap.cpp
        src/platform/game_allocation_ownership.cpp
        src/platform/mem_logical_heap.cpp src/platform/host_metadata.cpp)
    foreach(_name IN ITEMS snd_FrameHeap snd_SoundHeap snd_DisposeCallbackManager)
        target_sources(native_hbm_heap_tests PRIVATE
            "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/${_name}.cpp")
    endforeach()
    target_sources(native_hbm_heap_tests PRIVATE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_LinkList.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_frameHeap.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_heapCommon.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_list.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMutex.c")
    mscharged_select_original_hbm_debug(native_hbm_heap_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_heap_tests CPU_FIXTURE)
    add_dependencies(native_hbm_heap_tests verify_prepared)
    target_compile_features(native_hbm_heap_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_heap_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_heap_tests PRIVATE MSCHARGED_NATIVE=1
        MSCHARGED_GAME_MODULE=1 HBM_ASSERT=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_options(native_hbm_heap_tests PRIVATE -fshort-wchar
        -fno-strict-aliasing -ffp-contract=off -ffunction-sections -fdata-sections
        -Wno-unknown-pragmas)
    # Private fields are observation-only in this fixture; original TUs retain
    # their actual access control, source declarations and public calls.
    set_source_files_properties(tests/native_hbm_heap.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-access-control")
    target_link_libraries(native_hbm_heap_tests PRIVATE
        Threads::Threads charged_native_thread_queues)
    if(APPLE)
        target_link_options(native_hbm_heap_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_heap_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_heap COMMAND native_hbm_heap_tests)
    set_tests_properties(native_hbm_heap PROPERTIES TIMEOUT 15 LABELS "Platform")
endif()
