include_guard(GLOBAL)
add_library(charged_game_print STATIC src/runtime/game_print.cpp)
target_link_libraries(charged_game_print PUBLIC charged_graphics_memory)
if(BUILD_TESTING)
    add_executable(game_print_tests tests/game_print.cpp)
    target_link_libraries(game_print_tests PRIVATE charged_game_print)
    add_test(NAME game_print
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_print.py"
                "$<TARGET_FILE:game_print_tests>")
    set_tests_properties(game_print PROPERTIES TIMEOUT 10)
endif()
