include_guard(GLOBAL)
include(cmake/NativeAXTransport.cmake)

# Literal Wii AX command parser and verified zero-input device slice only.
# No DSP bootstrap/mail readiness, active voice/aux/filter/compressor kernel.
add_library(charged_native_ax_command_service STATIC src/platform/ax_command_service.cpp)
add_dependencies(charged_native_ax_command_service verify_prepared)
target_include_directories(charged_native_ax_command_service PUBLIC src)
target_compile_features(charged_native_ax_command_service PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_command_service PUBLIC charged_native_dsp_memory)

if(BUILD_TESTING AND TARGET native_ax_source_fixture)
    add_executable(native_ax_commands_tests tests/native_ax_commands.cpp)
    add_dependencies(native_ax_commands_tests verify_prepared native_ax_source_fixture)
    target_include_directories(native_ax_commands_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_commands_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1)
    target_compile_features(native_ax_commands_tests PRIVATE cxx_std_17)
    target_link_libraries(native_ax_commands_tests PRIVATE
        charged_native_ax_command_service aurora::core SDL3::SDL3)
    # Source-only DSO imports the genuine single host SDK/cache/context APIs.
    target_link_options(native_ax_commands_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=DCFlushRange -Wl,--undefined=DCFlushRangeNoSync
        -Wl,--undefined=DCInvalidateRange
        -Wl,--undefined=OSDisableInterrupts -Wl,--undefined=OSRestoreInterrupts)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_commands
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
                $<TARGET_FILE:native_ax_commands_tests>
                $<TARGET_FILE:native_ax_source_fixture>)
        set_tests_properties(native_ax_commands PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()

include(cmake/NativeAXADPCM.cmake)
