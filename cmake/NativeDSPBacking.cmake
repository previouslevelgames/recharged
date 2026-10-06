include_guard(GLOBAL)
include(cmake/NativeDSPMemory.cmake)

# Real source data and shared physical-address lifetime only. This does not run
# AXOut initialization, a DSP engine, or THPSimple's AX predecessor callback.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND UNIX AND NOT APPLE AND NOT MSVC)
    add_library(native_dsp_source_data_fixture MODULE tests/dsp_source_module.c
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c")
    add_dependencies(native_dsp_source_data_fixture verify_prepared)
    target_include_directories(native_dsp_source_data_fixture PRIVATE src
        "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_dsp_source_data_fixture PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1
        MSCHARGED_ORIGINAL_AXOUT_FILE="${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXOut.c")
    target_compile_features(native_dsp_source_data_fixture PRIVATE c_std_17)
    target_compile_options(native_dsp_source_data_fixture PRIVATE
        -ffunction-sections -fdata-sections -fvisibility=hidden -fexceptions
        -Wno-unknown-pragmas)
    target_link_options(native_dsp_source_data_fixture PRIVATE
        -Wl,--gc-sections -Wl,--no-undefined
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/dsp_source_module.exports")

    add_executable(native_dsp_backing_tests tests/native_dsp_backing.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_task.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_debug.c")
    add_dependencies(native_dsp_backing_tests verify_prepared native_dsp_source_data_fixture)
    target_include_directories(native_dsp_backing_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_dsp_backing_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_dsp_backing_tests PRIVATE c_std_17 cxx_std_17)
    target_compile_options(native_dsp_backing_tests PRIVATE
        -ffunction-sections -fdata-sections -fexceptions -Wno-unknown-pragmas)
    target_link_libraries(native_dsp_backing_tests PRIVATE charged_native_dsp_memory aurora::os
        SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_options(native_dsp_backing_tests PRIVATE -Wl,--gc-sections)
    # The real GX profile needs a real surface; SDL dummy is a core-only gate.
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_dsp_backing
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_dsp_backing.py"
                $<TARGET_FILE:native_dsp_backing_tests>
                $<TARGET_FILE:native_dsp_source_data_fixture>)
        set_tests_properties(native_dsp_backing PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()
