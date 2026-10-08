include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

add_library(charged_native_ai STATIC src/platform/ai.cpp)
add_dependencies(charged_native_ai verify_prepared)
target_include_directories(charged_native_ai PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_ai PUBLIC TARGET_PC=1)
target_compile_features(charged_native_ai PUBLIC cxx_std_20)
target_link_libraries(charged_native_ai PUBLIC charged_native_interrupts
    PRIVATE SDL3::SDL3)

if(BUILD_TESTING)
    add_executable(native_ai_tests tests/native_ai.cpp)
    target_link_libraries(native_ai_tests PRIVATE charged_native_ai SDL3::SDL3)
    add_test(NAME native_ai COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_ai.py"
        "$<TARGET_FILE:native_ai_tests>")
    set_tests_properties(native_ai PROPERTIES TIMEOUT 60)
    add_executable(native_ai_observation_tests tests/native_ai_observations.cpp)
    target_link_libraries(native_ai_observation_tests PRIVATE charged_native_ai SDL3::SDL3)
    add_test(NAME native_ai_observations COMMAND native_ai_observation_tests)
    set_tests_properties(native_ai_observations PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" LABELS "Platform")
    add_executable(native_ai_output_tests tests/native_ai_output.cpp)
    target_link_libraries(native_ai_output_tests PRIVATE charged_native_ai SDL3::SDL3)
    add_test(NAME native_ai_output COMMAND native_ai_output_tests)
    set_tests_properties(native_ai_output PROPERTIES TIMEOUT 15
        ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" LABELS "Platform")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(native_ai_timing_tests tests/native_ai_timing.cpp)
        target_link_libraries(native_ai_timing_tests PRIVATE charged_native_ai SDL3::SDL3)
        target_link_options(native_ai_timing_tests PRIVATE -Wl,--wrap=SDL_PutAudioStreamDataNoCopy)
        add_test(NAME native_ai_timing COMMAND native_ai_timing_tests)
        set_tests_properties(native_ai_timing PROPERTIES TIMEOUT 15
            ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" LABELS "Platform")
        # Late owner delivery: no replay of unprogrammed registers, bounded
        # catch-up. The dummy device pulls exactly every 2 ms.
        add_executable(native_ai_delivery_tests tests/native_ai_delivery.cpp)
        target_link_libraries(native_ai_delivery_tests PRIVATE charged_native_ai SDL3::SDL3)
        target_link_options(native_ai_delivery_tests PRIVATE -Wl,--wrap=SDL_PutAudioStreamDataNoCopy)
        add_test(NAME native_ai_delivery COMMAND native_ai_delivery_tests)
        set_tests_properties(native_ai_delivery PROPERTIES TIMEOUT 15 LABELS "Platform"
            ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_AUDIO_FREQUENCY=48000;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=96")
        # Prepared SDL keeps a read's channel map valid when the head track ends.
        add_executable(sdl_audio_queue_channel_map_tests tests/sdl_audio_queue_channel_map.cpp)
        target_link_libraries(sdl_audio_queue_channel_map_tests PRIVATE SDL3::SDL3)
        add_test(NAME sdl_audio_queue_channel_map COMMAND sdl_audio_queue_channel_map_tests)
        set_tests_properties(sdl_audio_queue_channel_map PROPERTIES TIMEOUT 15 LABELS "Platform"
            ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
    endif()
endif()
