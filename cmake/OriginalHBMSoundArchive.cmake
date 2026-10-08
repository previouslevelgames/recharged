include_guard(GLOBAL)
include(cmake/OriginalHBMWii16.cmake)
include(cmake/NativeHBMDebug.cmake)

# Whole original readers and their real utility/stream providers. Selection
# supplies no source initializer, player, archive bytes or successful readiness.
function(mscharged_select_original_hbm_sound_archive target)
    set(_sources src/platform/native_hbm_sound_archive.cpp)
    foreach(_name IN ITEMS snd_MemorySoundArchive snd_SoundArchive snd_SoundArchiveFile snd_Util)
        list(APPEND _sources "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/${_name}.cpp")
    endforeach()
    foreach(_name IN ITEMS ut_FileStream ut_IOStream)
        list(APPEND _sources "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/${_name}.cpp")
    endforeach()
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN LISTS _sources)
        if(NOT _source IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_source}")
        endif()
    endforeach()
endfunction()

if(BUILD_TESTING)
    # Synthetic archive through actual original readers/allocator and the
    # original retail assertion profile. No UI or source player claim.
    add_executable(native_hbm_archive_tests tests/native_hbm_archive.cpp
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        src/platform/game_allocation_ownership.cpp src/platform/host_metadata.cpp)
    mscharged_select_original_hbm_sound_archive(native_hbm_archive_tests)
    mscharged_select_original_hbm_debug(native_hbm_archive_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_archive_tests CPU_FIXTURE)
    add_dependencies(native_hbm_archive_tests verify_prepared)
    target_compile_features(native_hbm_archive_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_archive_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_archive_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 HBM_ASSERT=1)
    target_compile_options(native_hbm_archive_tests PRIVATE
        -fno-rtti -fshort-wchar -fno-strict-aliasing -ffp-contract=off
        -ffunction-sections -fdata-sections -Wno-unknown-pragmas)
    target_link_libraries(native_hbm_archive_tests PRIVATE Threads::Threads)
    if(APPLE)
        target_link_options(native_hbm_archive_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_archive_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_archive COMMAND native_hbm_archive_tests)
    set_tests_properties(native_hbm_archive PROPERTIES TIMEOUT 15 LABELS "Platform")
endif()

include(cmake/OriginalHBMHeap.cmake)
