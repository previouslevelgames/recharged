# The Discord status (issue #71): the local connection to the Discord app and
# the texts made from the game's state. The game host links it; the game
# module only reports its state (src/platform/presence_snapshot.cpp).
find_package(Threads REQUIRED)
if(NOT TARGET charged_json)
    add_library(charged_json STATIC src/platform/json.cpp)
    target_include_directories(charged_json PUBLIC src)
    target_compile_features(charged_json PUBLIC cxx_std_17)
endif()
add_library(charged_discord STATIC
    src/platform/discord_ipc.cpp
    src/platform/discord_presence.cpp
    src/platform/game_presence.cpp)
target_include_directories(charged_discord PUBLIC src)
target_compile_features(charged_discord PUBLIC cxx_std_17)
target_link_libraries(charged_discord PUBLIC charged_json Threads::Threads)

if(BUILD_TESTING)
    add_executable(discord_presence_tests tests/discord_presence.cpp)
    target_link_libraries(discord_presence_tests PRIVATE charged_discord)
    add_test(NAME discord_presence COMMAND discord_presence_tests)
    set_tests_properties(discord_presence PROPERTIES TIMEOUT 60)
endif()
