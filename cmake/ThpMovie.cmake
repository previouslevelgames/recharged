include_guard(GLOBAL)
# Explicit prepared Aurora decoder selection; do not change global SDK THP use.
# Aurora's source retains its MIT terms (extern/aurora/LICENSE).
add_library(charged_thp_decoder STATIC
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/thp/THPDec.cpp"
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/thp/THPAudio.cpp")
add_dependencies(charged_thp_decoder verify_prepared)
target_link_libraries(charged_thp_decoder PUBLIC aurora::core)
target_compile_features(charged_thp_decoder PUBLIC cxx_std_20)
add_library(charged_thp_movie STATIC src/resources/thp_movie.cpp src/runtime/thp_movie.cpp)
add_dependencies(charged_thp_movie verify_prepared)
target_include_directories(charged_thp_movie PUBLIC src PRIVATE "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_thp_movie PRIVATE MSCHARGED_NATIVE=1)
target_compile_features(charged_thp_movie PUBLIC cxx_std_20)
target_link_libraries(charged_thp_movie PUBLIC charged_thp_decoder charged_decomp_startup)
if(BUILD_TESTING)
    add_executable(thp_movie_tests tests/thp_movie.cpp)
    target_link_libraries(thp_movie_tests PRIVATE charged_thp_movie aurora::dvd)
    if(NOT MSCHARGED_BUILD_SCENE_PREVIEW)
        add_test(NAME thp_movie COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_thp_movie.py" "$<TARGET_FILE:thp_movie_tests>")
        set_tests_properties(thp_movie PROPERTIES TIMEOUT 100 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
    endif()
endif()
