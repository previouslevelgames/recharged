# The game's overlay over its picture (F1, F2 or HOME): tabs for Online (the
# player, their set, friends online, players available) and Game options. The
# Online tab's data comes from the profile service's lists without any drawing
# (charged_overlay_model); the overlay draws with ImGui and the launcher's
# widgets (charged_overlay), in the game through Aurora and in the launcher's
# preview (--page online-panel).
include_guard(GLOBAL)
add_library(charged_overlay_model STATIC src/overlay/online_panel_model.cpp)
target_include_directories(charged_overlay_model PUBLIC src)
target_compile_features(charged_overlay_model PUBLIC cxx_std_17)
target_link_libraries(charged_overlay_model PUBLIC charged_competitive)
if(BUILD_TESTING)
    add_executable(online_panel_tests tests/online_panel.cpp)
    target_link_libraries(online_panel_tests PRIVATE charged_overlay_model)
    add_test(NAME online_panel COMMAND online_panel_tests)
endif()

# The launcher's palette and widgets (pure ImGui), shared by the launcher and the overlay.
add_library(charged_ui_kit STATIC src/launcher/ui_kit.cpp)
target_include_directories(charged_ui_kit PUBLIC src)
target_compile_features(charged_ui_kit PUBLIC cxx_std_17)
target_link_libraries(charged_ui_kit PUBLIC charged_launcher_ui)

add_library(charged_overlay STATIC src/overlay/overlay_frame.cpp src/overlay/online_panel.cpp
    src/overlay/game_options.cpp src/overlay/game_overlay.cpp)
target_link_libraries(charged_overlay PUBLIC charged_overlay_model charged_ui_kit)
if(BUILD_TESTING)
    add_executable(game_overlay_tests tests/game_overlay.cpp)
    target_link_libraries(game_overlay_tests PRIVATE charged_overlay)
    add_test(NAME game_overlay COMMAND game_overlay_tests)
endif()
