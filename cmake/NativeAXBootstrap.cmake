include_guard(GLOBAL)
include(cmake/NativeDSPControl.cmake)
include(cmake/NativeAXCommandService.cmake)

# Real reset/loader + executed initialization prefix; default rejects frames.
# Explicit stopped-voice command mode remains bounded native conformance only.
# No production attachment, active voices or full DSP firmware execution.
add_library(charged_native_ax_bootstrap STATIC src/platform/ax_bootstrap_device.cpp)
add_dependencies(charged_native_ax_bootstrap verify_prepared)
target_include_directories(charged_native_ax_bootstrap PUBLIC src)
target_compile_features(charged_native_ax_bootstrap PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_bootstrap PUBLIC
    charged_native_dsp_instruction_core charged_native_ax_command_service)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT MSVC
        AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # All four original C TUs and every source method retained, no SDK in image.
    add_library(native_ax_bootstrap_source_fixture MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_task.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_debug.c")
    add_dependencies(native_ax_bootstrap_source_fixture verify_prepared)
    target_include_directories(native_ax_bootstrap_source_fixture PRIVATE src
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_bootstrap_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_bootstrap_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_bootstrap_source_fixture PRIVATE
        -fexceptions -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
    set_target_properties(native_ax_bootstrap_source_fixture PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
    target_link_options(native_ax_bootstrap_source_fixture PRIVATE -Wl,-Bsymbolic-functions)

    add_executable(native_ax_bootstrap_tests tests/native_ax_bootstrap.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_bootstrap_tests verify_prepared native_ax_bootstrap_source_fixture)
    target_include_directories(native_ax_bootstrap_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_bootstrap_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_bootstrap_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_bootstrap_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_bootstrap_tests PRIVATE charged_native_ax_bootstrap aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_options(native_ax_bootstrap_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=OSDisableInterrupts -Wl,--undefined=OSRestoreInterrupts
        -Wl,--undefined=OSRegisterVersion -Wl,--undefined=OSReport
        -Wl,--undefined=OSClearContext -Wl,--undefined=OSSetCurrentContext
        -Wl,--undefined=__OSSetInterruptHandler -Wl,--undefined=__OSUnmaskInterrupts
        -Wl,--undefined=ChargedDSPTaskMemoryWord -Wl,--undefined=ChargedDSPControlRead
        -Wl,--undefined=ChargedDSPControlWrite -Wl,--undefined=ChargedDSPMailToHigh
        -Wl,--undefined=ChargedDSPMailFromHigh -Wl,--undefined=ChargedDSPMailFromLow
        -Wl,--undefined=ChargedDSPMailToWriteHigh -Wl,--undefined=ChargedDSPMailToWriteLow
        -Wl,--undefined=ChargedDSPRequireMailWord)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_bootstrap COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
            "$<TARGET_FILE:native_ax_bootstrap_tests>"
            "$<TARGET_FILE:native_ax_bootstrap_source_fixture>")
        set_tests_properties(native_ax_bootstrap PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
