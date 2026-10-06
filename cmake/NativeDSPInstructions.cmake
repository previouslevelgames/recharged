include_guard(GLOBAL)
include(cmake/NativeDSPBacking.cmake)

# Partial hardware ISA only. Actual code words come from original source; this
# provider does not supply a ROM, reset context, boot handshake or AX readiness.
add_library(charged_native_dsp_instruction_core STATIC src/platform/dsp_instruction_core.cpp)
add_dependencies(charged_native_dsp_instruction_core verify_prepared)
target_include_directories(charged_native_dsp_instruction_core PUBLIC src)
target_compile_features(charged_native_dsp_instruction_core PUBLIC cxx_std_17)
target_link_libraries(charged_native_dsp_instruction_core PUBLIC charged_native_dsp_memory)

if(TARGET native_dsp_source_data_fixture)
    add_executable(native_dsp_instructions_tests tests/native_dsp_instructions.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c")
    add_dependencies(native_dsp_instructions_tests verify_prepared native_dsp_source_data_fixture)
    target_include_directories(native_dsp_instructions_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_dsp_instructions_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_dsp_instructions_tests PRIVATE c_std_17 cxx_std_17)
    target_compile_options(native_dsp_instructions_tests PRIVATE
        -ffunction-sections -fdata-sections -fexceptions -Wno-unknown-pragmas)
    target_link_libraries(native_dsp_instructions_tests PRIVATE
        charged_native_dsp_instruction_core aurora::os SDL3::SDL3)
    target_link_options(native_dsp_instructions_tests PRIVATE -Wl,--gc-sections)
    # The real GX profile compiles this prerequisite; SDL dummy only qualifies
    # the core SDK. A supplied register context is explicitly diagnostic.
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_dsp_instructions
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_dsp_backing.py"
                $<TARGET_FILE:native_dsp_instructions_tests>
                $<TARGET_FILE:native_dsp_source_data_fixture>)
        set_tests_properties(native_dsp_instructions PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()
