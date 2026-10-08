include_guard(GLOBAL)
include(cmake/OriginalHBMWii16.cmake)
include(cmake/OriginalModuleLinkage.cmake)
include(cmake/NativeAlarms.cmake)
include(cmake/NativeVideo.cmake)

# Native address/unwind/halt services use the host CRT. Original assertion,
# console, bitmap and map-file software remains in whole source TUs below.
add_library(charged_native_hbm_debug STATIC src/platform/hbm_debug.cpp)
add_dependencies(charged_native_hbm_debug verify_prepared)
target_include_directories(charged_native_hbm_debug PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_features(charged_native_hbm_debug PUBLIC cxx_std_20)
target_compile_definitions(charged_native_hbm_debug PRIVATE TARGET_PC=1)
target_link_libraries(charged_native_hbm_debug PUBLIC
    charged_platform_thread aurora::os "${CMAKE_DL_LIBS}")

function(mscharged_select_original_hbm_debug target)
    get_target_property(_sources "${target}" SOURCES)
    foreach(_name IN ITEMS db_assert db_console db_directPrint db_mapFile db_DbgPrintBase)
        set(_source "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/db/${_name}.cpp")
        if(NOT _source IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _sources "${_source}")
        endif()
    endforeach()
endfunction()

function(mscharged_link_original_hbm_debug_host target)
    target_link_libraries("${target}" PRIVATE charged_native_hbm_debug
        charged_native_alarms charged_native_video_device)
    # Module-owned whole db software calls these actual host services. Retain
    # each export through dead stripping; source operators stay module-local.
    foreach(_symbol IN ITEMS ChargedNativeHBMPointerValid ChargedNativeHBMMapCursorWord
            ChargedNativeHBMCaptureStack ChargedNativeHBMStackSymbol ChargedNativeHBMHalt)
        mscharged_require_original_host_symbol("${target}" PRIVATE "${_symbol}")
    endforeach()
    if(ARGC GREATER 1 AND ARGV1 STREQUAL "CPU_FIXTURE")
        # Existing main hosts already own their reports and target-local native
        # VI. Standalone CPU fixtures require those genuine providers as well.
        get_target_property(_sources "${target}" SOURCES)
        if(NOT "src/platform/os.cpp" IN_LIST _sources)
            target_sources("${target}" PRIVATE src/platform/os.cpp)
        endif()
        if(NOT TARGET charged_native_hbm_debug_vi)
            add_library(charged_native_hbm_debug_vi OBJECT
                "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp")
            add_dependencies(charged_native_hbm_debug_vi verify_prepared)
            target_compile_features(charged_native_hbm_debug_vi PRIVATE cxx_std_20)
            target_compile_definitions(charged_native_hbm_debug_vi PRIVATE
                AURORA_NATIVE_VIDEO=1 AURORA_WII_CLOCK=1 TARGET_PC=1)
            target_compile_options(charged_native_hbm_debug_vi PRIVATE
                -ffunction-sections -fdata-sections)
            target_link_libraries(charged_native_hbm_debug_vi PRIVATE aurora::vi)
        endif()
        target_link_libraries("${target}" PRIVATE charged_native_hbm_debug_vi)
        mscharged_require_original_host_symbol("${target}" PRIVATE OSReport)
        mscharged_require_original_host_symbol("${target}" PRIVATE OSVReport)
    endif()
endfunction()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    add_executable(native_hbm_assert_tests tests/native_hbm_assert.cpp)
    mscharged_select_original_hbm_debug(native_hbm_assert_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_assert_tests CPU_FIXTURE)
    add_dependencies(native_hbm_assert_tests verify_prepared)
    target_compile_features(native_hbm_assert_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_assert_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_assert_tests PRIVATE
        MSCHARGED_NATIVE=1 HBM_ASSERT=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_options(native_hbm_assert_tests PRIVATE -fshort-wchar
        -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off
        -Wno-unknown-pragmas "$<$<CXX_COMPILER_ID:Clang>:-Wno-register>")
    if(APPLE)
        target_link_options(native_hbm_assert_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_assert_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_assert COMMAND native_hbm_assert_tests positive)
    set_tests_properties(native_hbm_assert PROPERTIES TIMEOUT 10 LABELS "Platform")
    foreach(_mode IN ITEMS panic alignment)
        add_test(NAME "native_hbm_assert_${_mode}" COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/check_hbm_assert_failure.py"
            "$<TARGET_FILE:native_hbm_assert_tests>" "${_mode}")
        set_tests_properties("native_hbm_assert_${_mode}" PROPERTIES TIMEOUT 10 LABELS "Platform")
    endforeach()
endif()
