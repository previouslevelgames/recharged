include_guard(GLOBAL)
# Explicit stopped-voice/disposable-system-title SDK closure. No production
# ResetTask or main admission follows from declaring this CPU test.
if(NOT BUILD_TESTING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()
include(cmake/OriginalOSReset.cmake)
include(cmake/OriginalNAND.cmake)
include(cmake/NativeFilesystem.cmake)
include(cmake/NativeRTC.cmake)
include(cmake/NativeSTM.cmake)
include(cmake/NativeAlarms.cmake)
include(cmake/NativeSystemSettings.cmake)
include(cmake/NativeAXFunctional.cmake)
include(cmake/NativeOSShutdownRequests.cmake)
include(cmake/OriginalModuleLinkage.cmake)

# Deliberately fail rather than silently omit the whole-source closure when
# its original AX source image or genuine native power endpoint is unavailable.
if(NOT TARGET native_ax_active_source_fixture OR NOT TARGET charged_original_os_audio)
    message(FATAL_ERROR "Whole OSShutdownSystem CPU test requires the existing original AX and OS/audio providers")
endif()
add_executable(original_os_shutdown_tests
    tests/original_os_shutdown.cpp tests/os_play_record_source.c
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSStateFlags.c"
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp"
    src/platform/os_shutdown_record_transport.cpp
    tests/diagnostics/credits_movie_hardware.cpp
    src/platform/os.cpp src/platform/os_version.cpp src/platform/host_metadata.cpp)
add_dependencies(original_os_shutdown_tests verify_prepared native_ax_active_source_fixture)
target_include_directories(original_os_shutdown_tests PRIVATE src tests/diagnostics
    "${MSCHARGED_PREPARED}" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(original_os_shutdown_tests PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1 AURORA_NATIVE_VIDEO=1)
target_compile_features(original_os_shutdown_tests PRIVATE c_std_17 cxx_std_20)
target_compile_options(original_os_shutdown_tests PRIVATE
    -fexceptions -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off
    -Wno-unknown-pragmas $<$<COMPILE_LANGUAGE:C>:-Werror=pointer-to-int-cast>
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
target_link_libraries(original_os_shutdown_tests PRIVATE
    charged_original_os_reset charged_original_os_audio
    charged_original_nand_sources charged_original_fs_sources
    charged_native_filesystem charged_original_ipc_memory charged_native_ipc_boot_buffer
    charged_original_rtc charged_native_stm charged_native_alarms
    charged_native_system_settings charged_native_video_device charged_native_ax_functional
    charged_native_os_shutdown_requests
    aurora::dvd aurora::os aurora::core SDL3::SDL3 ${CMAKE_DL_LIBS})
get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
# Export only the real original AX image imports. A global export would keep
# unrelated default-visible original OSReset/NAND sections alive in this leaf.
foreach(_ax_option IN LISTS ax_source_exports)
    if(_ax_option MATCHES "^-Wl,--undefined=(.+)$")
        mscharged_require_original_host_symbol(original_os_shutdown_tests PRIVATE "${CMAKE_MATCH_1}")
    elseif(NOT _ax_option STREQUAL "-Wl,--export-dynamic")
        target_link_options(original_os_shutdown_tests PRIVATE "${_ax_option}")
    endif()
endforeach()
target_link_options(original_os_shutdown_tests PRIVATE -Wl,--gc-sections)
foreach(symbol OSGetTick __OSGetIOSRev __OSInitSram __OSSyncSram
    ChargedNativeOSInIPL ChargedOSAudioDSPRead ChargedOSAudioDSPWrite
    ChargedOSAudioDSPReadPair ChargedOSAudioDSPWritePair ChargedOSAudioIPCRead
    ChargedOSAudioIPCWrite ChargedOSAudioWorkMemory)
    mscharged_require_original_host_symbol(original_os_shutdown_tests PRIVATE "${symbol}")
endforeach()
add_test(NAME original_os_shutdown COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_original_os_shutdown.py"
    "$<TARGET_FILE:original_os_shutdown_tests>" "$<TARGET_FILE:native_ax_active_source_fixture>")
set_tests_properties(original_os_shutdown PROPERTIES TIMEOUT 30 LABELS "Platform"
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
