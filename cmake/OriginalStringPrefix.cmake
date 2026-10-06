include_guard(GLOBAL)
# Native four-byte runtime metadata beneath the unchanged original pool decisions.
# Raw prefix and typed original string operations use retained CPU fixture arenas.
# Actual SDK/module source gates remain private; CRT teardown is unqualified.
if(BUILD_TESTING AND UNIX AND NOT APPLE
    AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    add_executable(original_string_prefix_tests
        tests/original_string_prefix.cpp tests/original_string_prefix_host.cpp
        "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp")
    add_dependencies(original_string_prefix_tests verify_prepared)
    target_compile_features(original_string_prefix_tests PRIVATE cxx_std_20)
    target_include_directories(original_string_prefix_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_string_prefix_tests PRIVATE
        charged_original_core charged_game_print)
    target_compile_options(original_string_prefix_tests PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    target_link_options(original_string_prefix_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME original_string_prefix COMMAND original_string_prefix_tests)
    set_tests_properties(original_string_prefix PROPERTIES TIMEOUT 30
        PASS_REGULAR_EXPRESSION "actual original string prefix checks="
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
