if(TARGET charged_native_hardware_owner)
    return()
endif()

include(cmake/NativeWpad.cmake)
include(cmake/NativeSTM.cmake)
include(cmake/NativeAlarms.cmake)
include(cmake/NativeRTC.cmake)
include(cmake/NativeThreadQueues.cmake)
add_library(charged_native_hardware_owner STATIC
    src/platform/hardware_owner.cpp src/platform/desktop_wpad.cpp
    src/platform/desktop_dpd.cpp src/platform/desktop_dpd_projection.cpp)
target_compile_features(charged_native_hardware_owner PUBLIC cxx_std_20)
target_include_directories(charged_native_hardware_owner PUBLIC src
    PRIVATE "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_hardware_owner PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_link_libraries(charged_native_hardware_owner PUBLIC
    charged_native_wpad charged_native_stm charged_native_alarms charged_native_thread_queues
    aurora::os aurora::core)
add_dependencies(charged_native_hardware_owner verify_prepared)

# The successful-Present getter belongs to the real GX output provider only.
# Core-only input tests use raw observations/generated geometry explicitly.
if(MSCHARGED_BUILD_GX_CHECK)
    target_sources(charged_native_hardware_owner PRIVATE src/platform/desktop_presented_dpd.cpp)
    # Host screenshots (P) copy the presented frame through the same GX output.
    target_sources(charged_native_hardware_owner PRIVATE
        src/platform/screenshot_hotkey.cpp src/platform/screenshot_path.cpp)
    target_compile_definitions(charged_native_hardware_owner PRIVATE MSCHARGED_HOST_SCREENSHOTS=1)
endif()

# Native SDK ownership/desktop raw transport, without a game input manager.
# The explicit desktop profile composes with the existing movie AI owner.
if(BUILD_TESTING)
    include(cmake/OriginalCreditsMovieHardware.cmake)
    add_executable(desktop_wpad_tests tests/desktop_wpad_tests.cpp)
    target_compile_features(desktop_wpad_tests PRIVATE cxx_std_20)
    target_compile_definitions(desktop_wpad_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(desktop_wpad_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(desktop_wpad_tests PRIVATE
        charged_native_hardware_owner charged_credits_movie_hardware)
    add_test(NAME desktop_wpad COMMAND desktop_wpad_tests)
    # Real Wii Remotes, a DolphinBar or other gamepads attached to the host must
    # not join the generated fixtures' WPAD channels: only the fixtures' own
    # virtual devices count as gamepads.
    set(MSCHARGED_NO_REAL_REMOTES "SDL_HIDAPI_IGNORE_DEVICES=0x057e/0x0306,0x057e/0x0330;SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x1234/0x4321,0x057e/0x0306,0x057e/0x0330")
    set_tests_properties(desktop_wpad PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software;${MSCHARGED_NO_REAL_REMOTES}")

    add_executable(native_hardware_window_close_tests tests/native_hardware_window_close.cpp)
    target_compile_features(native_hardware_window_close_tests PRIVATE cxx_std_20)
    target_compile_definitions(native_hardware_window_close_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(native_hardware_window_close_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(native_hardware_window_close_tests PRIVATE
        charged_native_hardware_owner charged_credits_movie_hardware)
    add_test(NAME native_hardware_window_close COMMAND native_hardware_window_close_tests)
    set_tests_properties(native_hardware_window_close PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software")

    add_executable(screenshot_path_tests tests/screenshot_path_tests.cpp src/platform/screenshot_path.cpp)
    target_compile_features(screenshot_path_tests PRIVATE cxx_std_20)
    target_include_directories(screenshot_path_tests PRIVATE src)
    add_test(NAME screenshot_path COMMAND screenshot_path_tests)
    set_tests_properties(screenshot_path PROPERTIES TIMEOUT 15 ENVIRONMENT "TZ=UTC")

    add_executable(desktop_wpad_profile_tests tests/desktop_wpad_profile_tests.cpp)
    target_compile_features(desktop_wpad_profile_tests PRIVATE cxx_std_20)
    target_compile_definitions(desktop_wpad_profile_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(desktop_wpad_profile_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(desktop_wpad_profile_tests PRIVATE charged_native_hardware_owner)
    add_test(NAME desktop_wpad_profile COMMAND desktop_wpad_profile_tests)
    set_tests_properties(desktop_wpad_profile PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;${MSCHARGED_NO_REAL_REMOTES}")

    # Physical Wii Remote + Nunchuk through SDL's Wii HID driver identity,
    # emulated with an SDL virtual device of the same name/shape.
    add_executable(wpad_physical_nunchuk_tests tests/wpad_physical_nunchuk_tests.cpp)
    target_compile_features(wpad_physical_nunchuk_tests PRIVATE cxx_std_20)
    target_compile_definitions(wpad_physical_nunchuk_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(wpad_physical_nunchuk_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(wpad_physical_nunchuk_tests PRIVATE charged_native_hardware_owner)
    add_test(NAME wpad_physical_nunchuk COMMAND wpad_physical_nunchuk_tests)
    set_tests_properties(wpad_physical_nunchuk PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;${MSCHARGED_NO_REAL_REMOTES}")

    add_executable(desktop_dpd_projection_tests tests/desktop_dpd_projection_tests.cpp)
    target_compile_features(desktop_dpd_projection_tests PRIVATE cxx_std_20)
    target_link_libraries(desktop_dpd_projection_tests PRIVATE charged_native_hardware_owner)
    add_test(NAME desktop_dpd_projection COMMAND desktop_dpd_projection_tests)

    # Whole original KPAD + DPDData arithmetic, not a host pointer manager.
    add_executable(desktop_dpd_tests tests/desktop_dpd_tests.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/kpad/KPAD.c"
        "${MSCHARGED_PREPARED}/src/NL/plat/DPDData.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
        src/platform/os.cpp)
    add_dependencies(desktop_dpd_tests verify_prepared)
    target_compile_features(desktop_dpd_tests PRIVATE cxx_std_20 c_std_99)
    target_compile_definitions(desktop_dpd_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(desktop_dpd_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_options(desktop_dpd_tests PRIVATE -ffp-contract=off -fno-strict-aliasing)
    target_link_libraries(desktop_dpd_tests PRIVATE charged_native_hardware_owner aurora::mtx)
    add_test(NAME desktop_dpd COMMAND desktop_dpd_tests)
    set_tests_properties(desktop_dpd PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;${MSCHARGED_NO_REAL_REMOTES}")

    # Focus, leave/enter, repeat, shared-mapping and reattach lifecycle through
    # the same whole original KPAD consumer, one latest sample per update.
    add_executable(desktop_input_lifecycle_tests tests/desktop_input_lifecycle_tests.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/kpad/KPAD.c"
        "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
        src/platform/os.cpp)
    add_dependencies(desktop_input_lifecycle_tests verify_prepared)
    target_compile_features(desktop_input_lifecycle_tests PRIVATE cxx_std_20 c_std_99)
    target_compile_definitions(desktop_input_lifecycle_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(desktop_input_lifecycle_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_options(desktop_input_lifecycle_tests PRIVATE -ffp-contract=off -fno-strict-aliasing)
    target_link_libraries(desktop_input_lifecycle_tests PRIVATE charged_native_hardware_owner aurora::mtx)
    add_test(NAME desktop_input_lifecycle COMMAND desktop_input_lifecycle_tests)
    set_tests_properties(desktop_input_lifecycle PROPERTIES TIMEOUT 30
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;${MSCHARGED_NO_REAL_REMOTES}")
endif()
