include_guard(GLOBAL)
if(BUILD_TESTING)
    add_executable(native_clock_tests tests/native_clock.cpp)
    target_compile_features(native_clock_tests PRIVATE cxx_std_20)
    target_link_libraries(native_clock_tests PRIVATE
        charged_original_core aurora::os aurora::core SDL3::SDL3)
    add_test(NAME native_clock COMMAND native_clock_tests)
    set_tests_properties(native_clock PROPERTIES TIMEOUT 30
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
