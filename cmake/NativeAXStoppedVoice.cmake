include_guard(GLOBAL)
include(cmake/NativeAXInitialization.cmake)

# Explicit stopped-voice device/whole original AXInit qualification only.
# Reuse the exact 13-TU source leaf and sole SDK from initialization0330.
# No production attachment, active codec/SRC/AUX or full DSP execution claim.
if(BUILD_TESTING AND TARGET native_ax_init_source_fixture)
    add_executable(native_ax_stopped_tests tests/native_ax_stopped.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_stopped_tests verify_prepared native_ax_init_source_fixture)
    target_include_directories(native_ax_stopped_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_stopped_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_stopped_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_stopped_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_stopped_tests PRIVATE charged_native_ax_bootstrap
        charged_native_thread_queues charged_native_ai aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_options(native_ax_stopped_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=OSDisableInterrupts -Wl,--undefined=OSRestoreInterrupts
        -Wl,--undefined=OSRegisterVersion -Wl,--undefined=OSReport
        -Wl,--undefined=OSGetTime -Wl,--undefined=OSBaseAddress
        -Wl,--undefined=OSClearContext -Wl,--undefined=OSSetCurrentContext
        -Wl,--undefined=__OSSetInterruptHandler -Wl,--undefined=__OSUnmaskInterrupts
        -Wl,--undefined=OSInitThreadQueue -Wl,--undefined=OSWakeupThread
        -Wl,--undefined=ChargedDSPTaskMemoryWord -Wl,--undefined=ChargedDSPControlRead
        -Wl,--undefined=ChargedDSPControlWrite -Wl,--undefined=ChargedDSPMailToHigh
        -Wl,--undefined=ChargedDSPMailFromHigh -Wl,--undefined=ChargedDSPMailFromLow
        -Wl,--undefined=ChargedDSPMailToWriteHigh -Wl,--undefined=ChargedDSPMailToWriteLow
        -Wl,--undefined=ChargedDSPRequireMailWord
        -Wl,--undefined=DCFlushRange -Wl,--undefined=DCFlushRangeNoSync
        -Wl,--undefined=DCInvalidateRange -Wl,--undefined=AIInitDMA
        -Wl,--undefined=AIGetDMABytesLeft -Wl,--undefined=AIRegisterDMACallback
        -Wl,--undefined=AIStartDMA)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_stopped COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
            "$<TARGET_FILE:native_ax_stopped_tests>"
            "$<TARGET_FILE:native_ax_init_source_fixture>")
        set_tests_properties(native_ax_stopped PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
