include_guard(GLOBAL)
include(cmake/NativeAXFrameCommands.cmake)
include(cmake/NativeOSAudioBoot.cmake)

# Explicit native command/IRQ attachment after actual initialized retained chip
# state. Defaults stay unchanged; generated-bank conformance is not production
# kernel readiness, authentic coefficients, or admission of a game audio cue.
add_library(charged_native_ax_normal_command STATIC src/platform/ax_normal_device.cpp)
add_dependencies(charged_native_ax_normal_command verify_prepared)
target_include_directories(charged_native_ax_normal_command PUBLIC src)
target_compile_features(charged_native_ax_normal_command PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_normal_command PUBLIC
    charged_native_ax_bootstrap charged_native_ax_frame_commands)

if(BUILD_TESTING AND TARGET native_ax_active_source_fixture)
    # Complete OS TU without a repeated AX firmware/global owner in this image.
    add_library(native_ax_normal_os_source_fixture MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSAudioSystem.c")
    add_dependencies(native_ax_normal_os_source_fixture verify_prepared)
    target_include_directories(native_ax_normal_os_source_fixture PRIVATE src
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_normal_os_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_normal_os_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_normal_os_source_fixture PRIVATE
        -fexceptions -fwrapv -fno-strict-aliasing -Wno-unknown-pragmas)
    set_target_properties(native_ax_normal_os_source_fixture PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
    target_link_options(native_ax_normal_os_source_fixture PRIVATE -Wl,--no-gc-sections)
    add_executable(native_ax_normal_tests tests/native_ax_normal.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_normal_tests verify_prepared
        native_ax_active_source_fixture native_ax_normal_os_source_fixture)
    target_include_directories(native_ax_normal_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_normal_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_normal_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_normal_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_normal_tests PRIVATE charged_native_ax_normal_command
        charged_native_os_audio_boot charged_native_thread_queues charged_native_ai
        aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    # Both images contain whole source and no second SDK. Original DSP source
    # globals/handler live in the AX image, not a duplicate host DSP library.
    get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
    target_link_options(native_ax_normal_tests PRIVATE ${ax_source_exports}
        -Wl,--undefined=DCFlushRangeNoSync -Wl,--undefined=OSGetTick
        -Wl,--undefined=OSGetArenaHi -Wl,--undefined=__OSInIPL)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_normal COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_normal.py"
            "$<TARGET_FILE:native_ax_normal_tests>" "$<TARGET_FILE:native_ax_active_source_fixture>"
            "$<TARGET_FILE:native_ax_normal_os_source_fixture>")
        set_tests_properties(native_ax_normal PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
