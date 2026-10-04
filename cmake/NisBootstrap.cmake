include_guard(GLOBAL)
add_library(charged_nis_bootstrap_reader STATIC src/resources/nis_bootstrap.cpp)
target_include_directories(charged_nis_bootstrap_reader PUBLIC src)
target_compile_features(charged_nis_bootstrap_reader PUBLIC cxx_std_20)
add_library(charged_nis_bootstrap STATIC src/runtime/nis_bootstrap.cpp)
target_link_libraries(charged_nis_bootstrap PUBLIC charged_nis_bootstrap_reader charged_decomp_startup)
if(BUILD_TESTING)
    add_executable(nis_text_tests tests/nis_text.cpp)
    target_link_libraries(nis_text_tests PRIVATE charged_nis_bootstrap_reader)
    add_test(NAME nis_text COMMAND nis_text_tests)
    add_executable(nis_bootstrap_tests tests/nis_bootstrap.cpp)
    target_link_libraries(nis_bootstrap_tests PRIVATE charged_nis_bootstrap aurora::dvd aurora::core)
    add_test(NAME nis_bootstrap
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nis_bootstrap.py"
            "$<TARGET_FILE:nis_bootstrap_tests>")
    set_tests_properties(nis_bootstrap PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
endif()
