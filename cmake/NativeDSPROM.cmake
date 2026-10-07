include_guard(GLOBAL)
include(cmake/NativeDSPControl.cmake)

# Original128-byte static initializer only. Complete OSAudioSystem.c is
# compiler-selected from the prepared TU; uncalled service sections are collected.
add_library(native_dsp_boot_data_fixture MODULE tests/dsp_boot_data.c)
add_dependencies(native_dsp_boot_data_fixture verify_prepared)
target_include_directories(native_dsp_boot_data_fixture PRIVATE tests src
    "${MSCHARGED_PREPARED}"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(native_dsp_boot_data_fixture PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_features(native_dsp_boot_data_fixture PRIVATE c_std_17)
# This data-only module has no SDK runtime imports. Linking the full static
# host here would require a second SDK instance and unrelated non-PIC libraries.
# Only genuine compiled source initializer bytes remain after section collection.
set_target_properties(native_dsp_boot_data_fixture PROPERTIES C_VISIBILITY_PRESET hidden)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(native_dsp_boot_data_fixture PRIVATE -ffunction-sections
        -fdata-sections -Wno-unknown-pragmas)
    if(APPLE)
        target_link_options(native_dsp_boot_data_fixture PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(native_dsp_boot_data_fixture PRIVATE -Wl,--gc-sections)
    endif()
endif()

add_executable(native_dsp_rom_tests tests/native_dsp_rom.cpp
    src/platform/os_version.cpp src/platform/os.cpp)
add_dependencies(native_dsp_rom_tests verify_prepared native_dsp_boot_data_fixture)
target_compile_features(native_dsp_rom_tests PRIVATE cxx_std_17)
target_link_libraries(native_dsp_rom_tests PRIVATE charged_original_dsp
    charged_native_dsp_instruction_core SDL3::SDL3)
# No source DSP section collection in this executable. ROM cells/context are
# explicitly generated diagnostics; no game/source boot readiness is claimed.
if(NOT MSCHARGED_BUILD_GX_CHECK)
    add_test(NAME native_dsp_rom
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_dsp_backing.py"
            $<TARGET_FILE:native_dsp_rom_tests>
            $<TARGET_FILE:native_dsp_boot_data_fixture>)
    set_tests_properties(native_dsp_rom PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
