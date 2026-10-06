if(TARGET charged_native_hardware_owner)
    return()
endif()

include(cmake/NativeWpad.cmake)
include(cmake/NativeSTM.cmake)
add_library(charged_native_hardware_owner STATIC
    src/platform/hardware_owner.cpp src/platform/desktop_wpad.cpp)
target_compile_features(charged_native_hardware_owner PUBLIC cxx_std_20)
target_include_directories(charged_native_hardware_owner PUBLIC src
    PRIVATE "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_hardware_owner PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_link_libraries(charged_native_hardware_owner PUBLIC
    charged_native_wpad charged_native_stm aurora::os aurora::core)
add_dependencies(charged_native_hardware_owner verify_prepared)

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
endif()
