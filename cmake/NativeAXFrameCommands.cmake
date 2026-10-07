include_guard(GLOBAL)
include(cmake/NativeAXActiveVoice.cmake)

# Staged AX command/AUX/output conformance, not a production-ready DSP kernel.
# Original callbacks/ring/source lists remain authoritative. Complete Studio,
# surround, DPL2, compressor history/attack and authentic FIR remain bounded.
add_library(charged_native_ax_frame_commands STATIC src/platform/ax_frame_commands.cpp)
add_dependencies(charged_native_ax_frame_commands verify_prepared)
target_include_directories(charged_native_ax_frame_commands PUBLIC src)
target_compile_features(charged_native_ax_frame_commands PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_frame_commands PUBLIC
    charged_native_ax_active_voice charged_native_ax_command_service)

if(BUILD_TESTING AND TARGET native_ax_init_source_fixture)
    get_target_property(ax_original_sources native_ax_init_source_fixture SOURCES)
    add_library(native_ax_frame_source_fixture MODULE ${ax_original_sources}
        "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXDelay.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXHooks.c")
    add_dependencies(native_ax_frame_source_fixture verify_prepared)
    target_include_directories(native_ax_frame_source_fixture PRIVATE src
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_PREPARED}/libs/Runtime/include" "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_frame_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_frame_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_frame_source_fixture PRIVATE
        -fexceptions -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
    set_target_properties(native_ax_frame_source_fixture PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
    target_link_libraries(native_ax_frame_source_fixture PRIVATE m)
    target_link_options(native_ax_frame_source_fixture PRIVATE -Wl,-Bsymbolic-functions -Wl,--no-gc-sections)

    set(ax_command_oracle "${CMAKE_CURRENT_BINARY_DIR}/native-ax-command-oracle/command-oracle.bin")
    add_custom_command(OUTPUT "${ax_command_oracle}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_command_oracle.py"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c" "${CMAKE_CURRENT_BINARY_DIR}/native-ax-command-oracle"
        DEPENDS verify_prepared "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_command_oracle.py" VERBATIM)
    add_custom_target(native_ax_command_oracle DEPENDS "${ax_command_oracle}")
    add_executable(native_ax_frame_commands_tests tests/native_ax_frame_commands.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_frame_commands_tests verify_prepared native_ax_frame_source_fixture native_ax_command_oracle)
    target_include_directories(native_ax_frame_commands_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_frame_commands_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_frame_commands_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_frame_commands_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_frame_commands_tests PRIVATE charged_native_ax_frame_commands
        charged_native_ax_bootstrap charged_native_thread_queues charged_native_ai
        aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
    target_link_options(native_ax_frame_commands_tests PRIVATE ${ax_source_exports}
        -Wl,--undefined=OSAllocFromHeap -Wl,--undefined=OSFreeToHeap -Wl,--undefined=__OSCurrHeap)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_frame_commands COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_frame_commands.py"
            "$<TARGET_FILE:native_ax_frame_commands_tests>" "$<TARGET_FILE:native_ax_frame_source_fixture>" "${ax_command_oracle}")
        set_tests_properties(native_ax_frame_commands PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
