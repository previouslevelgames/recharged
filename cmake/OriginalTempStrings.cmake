include_guard(GLOBAL)
# Whole original static manager plus literal typed source methods. Retained
# CPU fixture arenas precede source constructors; this is not module/SDK
# startup, original main, or CRT teardown qualification.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    add_executable(original_temp_strings_tests
        tests/original_temp_strings.cpp tests/original_string_prefix_host.cpp
        "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp")
    add_dependencies(original_temp_strings_tests verify_prepared)
    target_compile_features(original_temp_strings_tests PRIVATE cxx_std_20)
    target_include_directories(original_temp_strings_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_temp_strings_tests PRIVATE
        charged_original_core charged_game_print)
    target_compile_options(original_temp_strings_tests PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -fcheck-new -ffunction-sections -fdata-sections)
    target_link_options(original_temp_strings_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME original_temp_strings COMMAND original_temp_strings_tests)
    set_tests_properties(original_temp_strings PROPERTIES TIMEOUT 30
        PASS_REGULAR_EXPRESSION "actual original temporary string checks="
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
