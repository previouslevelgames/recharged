include_guard(GLOBAL)
include(cmake/NativeAXBootstrap.cmake)
include(cmake/NativeThreadQueues.cmake)
include(cmake/NativeAI.cmake)

# Actual whole AXOutInitDSP/private task/init callback qualification only.
# No production attachment, full AXInit, AI predecessor or command-frame ready.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT MSVC
        AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Retain every method in all 13 original C TUs. The sole SDK lives in host.
    add_library(native_ax_init_source_fixture MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AX.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAlloc.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAux.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXCL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXComp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXSPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXVPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXProf.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXOut.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_task.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_debug.c")
    add_dependencies(native_ax_init_source_fixture verify_prepared)
    target_include_directories(native_ax_init_source_fixture PRIVATE src
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_PREPARED}/libs/Runtime/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_init_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_init_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_init_source_fixture PRIVATE
        -fexceptions -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
    set_target_properties(native_ax_init_source_fixture PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
    target_link_libraries(native_ax_init_source_fixture PRIVATE m)
    target_link_options(native_ax_init_source_fixture PRIVATE
        -Wl,-Bsymbolic-functions -Wl,--no-gc-sections)

    add_executable(native_ax_init_tests tests/native_ax_init.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_init_tests verify_prepared native_ax_init_source_fixture)
    target_include_directories(native_ax_init_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_init_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_init_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_init_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_init_tests PRIVATE charged_native_ax_bootstrap
        charged_native_thread_queues charged_native_ai aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    # All genuine source imports are provided by real retained host definitions.
    target_link_options(native_ax_init_tests PRIVATE -Wl,--export-dynamic
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
        add_test(NAME native_ax_init COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
            "$<TARGET_FILE:native_ax_init_tests>"
            "$<TARGET_FILE:native_ax_init_source_fixture>")
        set_tests_properties(native_ax_init PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
