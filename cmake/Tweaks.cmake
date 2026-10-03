include_guard(GLOBAL)
add_library(charged_tweaks STATIC
    "${MSCHARGED_PREPARED}/src/Game/TweakRegistry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNode.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakEntry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValue.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValueBase.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValueNative.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakConfig.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNameRecycler.cpp"
    "${MSCHARGED_PREPARED}/src/NL/PointerEntryTable.cpp"
    src/runtime/tweaks.cpp src/runtime/game_print.cpp)
add_dependencies(charged_tweaks verify_prepared)
target_link_libraries(charged_tweaks PUBLIC charged_game_config)
target_compile_features(charged_tweaks PUBLIC cxx_std_17)
if(BUILD_TESTING)
    add_executable(tweaks_tests tests/tweaks.cpp)
    target_link_libraries(tweaks_tests PRIVATE charged_tweaks)
    add_test(NAME tweaks COMMAND tweaks_tests)
    set_tests_properties(tweaks PROPERTIES TIMEOUT 30)
endif()
