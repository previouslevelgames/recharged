if(TARGET charged_native_hardware_owner)
    return()
endif()

include(cmake/NativeWpad.cmake)
include(cmake/NativeSTM.cmake)
include(cmake/NativeAlarms.cmake)
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
    set_tests_properties(desktop_wpad PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software")

    add_executable(desktop_wpad_profile_tests tests/desktop_wpad_profile_tests.cpp)
    target_compile_features(desktop_wpad_profile_tests PRIVATE cxx_std_20)
    target_compile_definitions(desktop_wpad_profile_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_include_directories(desktop_wpad_profile_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(desktop_wpad_profile_tests PRIVATE charged_native_hardware_owner)
    add_test(NAME desktop_wpad_profile COMMAND desktop_wpad_profile_tests)
    set_tests_properties(desktop_wpad_profile PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software")

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
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software")
endif()
