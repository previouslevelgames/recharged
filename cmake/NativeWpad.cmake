include_guard(GLOBAL)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NativeInterrupts.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WiimoteScan.cmake")

# Native hardware SDK interfaces underneath the original game/KPAD code.
# Owner servicing is explicit; no alternate pad manager or device readiness.
add_library(charged_native_wpad STATIC EXCLUDE_FROM_ALL
    src/platform/wpad_sdl.cpp src/platform/wiimote_hid.cpp src/platform/os_version.cpp)
add_dependencies(charged_native_wpad verify_prepared)
target_include_directories(charged_native_wpad PUBLIC src PRIVATE
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_features(charged_native_wpad PRIVATE cxx_std_20)
target_compile_definitions(charged_native_wpad PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_link_libraries(charged_native_wpad PUBLIC
    charged_native_interrupts charged_wiimote_scan SDL3::SDL3 aurora::os)

if(BUILD_TESTING)
    add_executable(native_wpad_speaker_volume_tests tests/native_wpad_speaker_volume.cpp)
    target_compile_features(native_wpad_speaker_volume_tests PRIVATE cxx_std_20)
    target_compile_definitions(native_wpad_speaker_volume_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_include_directories(native_wpad_speaker_volume_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(native_wpad_speaker_volume_tests PRIVATE charged_native_wpad)
    add_test(NAME native_wpad_speaker_volume COMMAND native_wpad_speaker_volume_tests)
    set_tests_properties(native_wpad_speaker_volume PROPERTIES TIMEOUT 10
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    add_executable(wiimote_hid_tests tests/wiimote_hid_tests.cpp)
    target_compile_features(wiimote_hid_tests PRIVATE cxx_std_20)
    target_compile_definitions(wiimote_hid_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_include_directories(wiimote_hid_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(wiimote_hid_tests PRIVATE charged_native_wpad)
    add_test(NAME wiimote_hid COMMAND wiimote_hid_tests)
    set_tests_properties(wiimote_hid PROPERTIES TIMEOUT 10)
endif()
