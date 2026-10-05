include_guard(GLOBAL)
include(cmake/WiiStringFormat.cmake)
# Compile the complete original NL wrappers; host code implements only the
# underlying formatting/report services, including libc alias compatibility.
add_library(charged_game_print STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlPrint.cpp"
    src/platform/string_format.cpp src/platform/report.cpp)
add_dependencies(charged_game_print verify_prepared)
target_compile_features(charged_game_print PRIVATE cxx_std_17)
target_link_libraries(charged_game_print PUBLIC charged_original_core
    PRIVATE charged_wii_string_format)
if(BUILD_TESTING)
    add_executable(game_print_tests tests/game_print.cpp)
    target_link_libraries(game_print_tests PRIVATE charged_game_print)
    add_test(NAME game_print
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_print.py"
                "$<TARGET_FILE:game_print_tests>")
    set_tests_properties(game_print PROPERTIES TIMEOUT 10)
    add_executable(original_print_tests tests/original_print.cpp)
    target_compile_features(original_print_tests PRIVATE cxx_std_20)
    target_link_libraries(original_print_tests PRIVATE charged_game_print)
    add_test(NAME original_print
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_print.py"
                "$<TARGET_FILE:original_print_tests>")
    set_tests_properties(original_print PROPERTIES TIMEOUT 20)
endif()
