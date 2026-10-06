include_guard(GLOBAL)
add_library(charged_game_config STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlConfig.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/simpleparser.cpp"
    src/runtime/game_config.cpp)
add_dependencies(charged_game_config verify_prepared)
target_include_directories(charged_game_config PUBLIC src
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_game_config PUBLIC charged_graphics_memory)
target_compile_features(charged_game_config PUBLIC cxx_std_17)
# Selected configuration/tweak diagnostics retain their historical insertion
# policies. Genuine whole-source inventories and the original game module must
# use the literal original string/vector methods, without this definition.
target_compile_definitions(charged_game_config PUBLIC
    MSCHARGED_DIAGNOSTIC_VECTORS=1)
if(BUILD_TESTING)
    add_executable(game_config_tests tests/game_config.cpp)
    target_link_libraries(game_config_tests PRIVATE charged_game_config)
    add_test(NAME game_config COMMAND game_config_tests)
    set_tests_properties(game_config PROPERTIES TIMEOUT 30)
endif()
