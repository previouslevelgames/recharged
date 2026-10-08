include_guard(GLOBAL)
include(cmake/OriginalNativeCompilerProfile.cmake)

mscharged_original_native_profile_supported(_charged_shutdown_record_profile C CXX)
if(NOT _charged_shutdown_record_profile)
    message(FATAL_ERROR "Original shutdown-record providers require the supported native C/C++ profile")
endif()

# Whole SDK owners only. This inventory does not select ResetTask, start play
# recording, initialize NAND, or admit source reset/shutdown requests.
# Link these into the same original module as its NAND/OSReset owners when the
# caller's real shutdown predecessor is ready; do not create a second owner.
add_library(charged_original_os_shutdown_record_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSPlayRecord.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSStateFlags.c"
    src/platform/os_shutdown_record_transport.cpp)
add_dependencies(charged_original_os_shutdown_record_sources verify_prepared)
function(mscharged_configure_original_shutdown_records target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(${target} PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(${target} PRIVATE c_std_17 cxx_std_17)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${target} PRIVATE
        -fexceptions -ffunction-sections -fdata-sections -fvisibility=hidden
        -fno-strict-aliasing -Wno-unknown-pragmas
        $<$<COMPILE_LANGUAGE:C>:-Werror=pointer-to-int-cast>
        $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
endfunction()
mscharged_configure_original_shutdown_records(charged_original_os_shutdown_record_sources)

# Attach to the existing original module and its already selected NAND/OSReset
# owners. This adds no initializer, host game loop or reset request.
function(mscharged_add_original_shutdown_records target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Original shutdown records require an existing source target")
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_source IN ITEMS
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSPlayRecord.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSStateFlags.c")
        if(_source IN_LIST _sources)
            message(FATAL_ERROR "Original shutdown record owner is already selected: ${_source}")
        endif()
    endforeach()
    target_sources("${target}" PRIVATE $<TARGET_OBJECTS:charged_original_os_shutdown_record_sources>)
endfunction()

# Requalifies current prepared source; this fixture owns only a disposable
# virtual system-title filesystem. The whole PlayRecord TU is included once
# by a read-only test probe, not copied into the repository or initialized by
# the production attachment above.
if(BUILD_TESTING)
    include(cmake/OriginalOSReset.cmake)
    include(cmake/OriginalNAND.cmake)
    include(cmake/NativeFilesystem.cmake)
    include(cmake/NativeAlarms.cmake)
    include(cmake/NativeSystemSettings.cmake)
    include(cmake/NativeVideo.cmake)
    set(_record_oracle "${CMAKE_CURRENT_BINARY_DIR}/os-record-oracles/os_record_oracles.h")
    add_custom_command(OUTPUT "${_record_oracle}"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_os_record_oracles.py"
            "${_record_oracle}"
        DEPENDS tests/generate_os_record_oracles.py
        VERBATIM)
    add_executable(original_os_shutdown_record_tests
        tests/original_os_shutdown_records.cpp tests/os_play_record_source.c
        "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSStateFlags.c"
        src/platform/os_shutdown_record_transport.cpp
        src/platform/os.cpp src/platform/os_version.cpp
        src/platform/host_metadata.cpp "${_record_oracle}")
    add_dependencies(original_os_shutdown_record_tests verify_prepared)
    mscharged_configure_original_shutdown_records(original_os_shutdown_record_tests)
    target_include_directories(original_os_shutdown_record_tests PRIVATE
        "${MSCHARGED_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/os-record-oracles")
    target_compile_definitions(original_os_shutdown_record_tests PRIVATE
        MSCHARGED_OS_RECORD_TEST_OBSERVER=1 AURORA_NATIVE_VIDEO=1)
    target_compile_features(original_os_shutdown_record_tests PRIVATE cxx_std_20)
    target_link_libraries(original_os_shutdown_record_tests PRIVATE
        charged_original_os_reset charged_original_nand_sources
        charged_original_fs_sources charged_native_filesystem
        charged_original_ipc_memory charged_native_ipc_boot_buffer
        charged_native_alarms charged_native_system_settings
        charged_native_video_device aurora::os aurora::core SDL3::SDL3)
    if(APPLE)
        target_link_options(original_os_shutdown_record_tests PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(original_os_shutdown_record_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_os_shutdown_records COMMAND original_os_shutdown_record_tests
        "${CMAKE_CURRENT_BINARY_DIR}/original-os-record-tests-data")
    add_test(NAME original_os_shutdown_records_game_identity COMMAND original_os_shutdown_record_tests
        "${CMAKE_CURRENT_BINARY_DIR}/original-os-record-tests-data" --game-identity)
    set_tests_properties(original_os_shutdown_records original_os_shutdown_records_game_identity
        PROPERTIES TIMEOUT 30 LABELS "Platform")
endif()
