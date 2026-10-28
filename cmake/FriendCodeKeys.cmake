include_guard(GLOBAL)
# Number keys for the online friend code entry (patch 0756): the host reads
# them from SDL and the game module takes them one at a time.
add_library(charged_friend_code_keys STATIC src/platform/friend_code_keys.cpp)
target_compile_features(charged_friend_code_keys PUBLIC cxx_std_17)
target_include_directories(charged_friend_code_keys PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(charged_friend_code_keys PUBLIC SDL3::SDL3)

if(BUILD_TESTING)
    add_executable(friend_code_keys_tests tests/friend_code_keys.cpp)
    target_link_libraries(friend_code_keys_tests PRIVATE charged_friend_code_keys)
    add_test(NAME friend_code_keys COMMAND friend_code_keys_tests)
    set_tests_properties(friend_code_keys PROPERTIES TIMEOUT 10)
endif()
