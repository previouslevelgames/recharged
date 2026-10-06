include_guard(GLOBAL)
include(cmake/NativeDSPInstructions.cmake)

# Real DSP-only CSR/IFX hardware transport. ROM/bootstrap and AI/ARAM DMA cause
# coupling remain unsupported; this target invents no source initialization.
add_library(charged_native_dsp_control STATIC src/platform/dsp_control.cpp)
add_dependencies(charged_native_dsp_control verify_prepared)
target_include_directories(charged_native_dsp_control PUBLIC src)
target_compile_features(charged_native_dsp_control PUBLIC cxx_std_17)
target_link_libraries(charged_native_dsp_control PUBLIC charged_native_dsp_mailbox)
target_link_libraries(charged_native_dsp_instruction_core PUBLIC charged_native_dsp_control)

# Complete original SDK source, preserving task/control/callback decisions.
# Production still needs one actual host OSVersion/reporting provider foundation.
add_library(charged_original_dsp STATIC
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_task.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_debug.c")
add_dependencies(charged_original_dsp verify_prepared)
target_include_directories(charged_original_dsp PUBLIC src
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_dsp PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_dsp PRIVATE c_std_17)
target_link_libraries(charged_original_dsp PUBLIC charged_native_dsp_control
    charged_native_dsp_memory aurora::os)
set_target_properties(charged_original_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_dsp PRIVATE -fexceptions -Wno-unknown-pragmas)
endif()

if(TARGET native_dsp_source_data_fixture)
    # Existing literal reporting services only; the fixture does not link any
    # game/core allocator, extracted startup or alternate initialization manager.
    add_executable(native_dsp_control_tests tests/native_dsp_control.cpp
        src/platform/os_version.cpp src/platform/os.cpp)
    add_dependencies(native_dsp_control_tests verify_prepared native_dsp_source_data_fixture)
    target_compile_features(native_dsp_control_tests PRIVATE cxx_std_17)
    target_link_libraries(native_dsp_control_tests PRIVATE charged_original_dsp
        charged_native_dsp_instruction_core SDL3::SDL3)
    # Deliberately no GC: all three whole DSP source units must genuinely link.
    # The source-data leaf still has the explicitly bounded 212 data export scope.
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_dsp_control
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_dsp_backing.py"
                $<TARGET_FILE:native_dsp_control_tests>
                $<TARGET_FILE:native_dsp_source_data_fixture>)
        set_tests_properties(native_dsp_control PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()
