include_guard(GLOBAL)
include(cmake/NativeOSAudioRegisters.cmake)
include(cmake/NativeAXNativeFilter.cmake)
include(cmake/NativeAXFrameCommands.cmake)

# Explicit bounded functional-native hardware contract, not authentic Wii ROM
# or full DSP-ISA execution. Source owners/requests/callbacks remain unchanged.
# No production activation; unsupported voice/remote Studio/surround/compressor work still faults.
add_library(charged_native_ax_functional STATIC src/platform/ax_functional_device.cpp)
add_dependencies(charged_native_ax_functional verify_prepared)
target_include_directories(charged_native_ax_functional PUBLIC src)
target_compile_features(charged_native_ax_functional PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_functional PUBLIC charged_native_ax_bootstrap
    charged_native_ax_native_filter charged_native_ax_frame_commands charged_native_ai
    charged_native_os_audio_registers)

if(BUILD_TESTING AND TARGET native_ax_active_source_fixture)
    add_executable(native_ax_functional_tests tests/native_ax_functional.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_functional_tests verify_prepared native_ax_active_source_fixture)
    target_include_directories(native_ax_functional_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_functional_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_functional_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_functional_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_functional_tests PRIVATE charged_native_ax_functional
        charged_native_thread_queues aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
    target_link_options(native_ax_functional_tests PRIVATE ${ax_source_exports})
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_functional COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_functional.py"
            "$<TARGET_FILE:native_ax_functional_tests>" "$<TARGET_FILE:native_ax_active_source_fixture>")
        set_tests_properties(native_ax_functional PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()

# The Release original-main graph includes this device directly, without the
# older NativeRuntime startup prototype. Bring in the genuine whole OS boot
# fixture only when the matching whole AX source test image is available.
if(BUILD_TESTING AND TARGET native_ax_active_source_fixture)
    include(cmake/NativeOSAudioBoot.cmake)
endif()
mscharged_add_native_os_audio_functional_stop_test()
