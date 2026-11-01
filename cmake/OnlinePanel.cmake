# The online panel the game opens over its picture (F1 or HOME): the player,
# their set, friends, available players and Mii friends. Its data comes from
# the profile service's lists without any drawing (charged_online_panel_model);
# it draws with ImGui and the launcher's widgets (charged_online_panel), in the
# game through Aurora and in the launcher's preview (--page online-panel).
include_guard(GLOBAL)
add_library(charged_online_panel_model STATIC src/online_panel/online_panel_model.cpp)
target_include_directories(charged_online_panel_model PUBLIC src)
target_compile_features(charged_online_panel_model PUBLIC cxx_std_17)
target_link_libraries(charged_online_panel_model PUBLIC charged_competitive)
if(BUILD_TESTING)
    add_executable(online_panel_tests tests/online_panel.cpp)
    target_link_libraries(online_panel_tests PRIVATE charged_online_panel_model)
    add_test(NAME online_panel COMMAND online_panel_tests)
endif()

# The launcher's palette and widgets (pure ImGui), shared by the launcher and the panel.
add_library(charged_ui_kit STATIC src/launcher/ui_kit.cpp)
target_include_directories(charged_ui_kit PUBLIC src)
target_compile_features(charged_ui_kit PUBLIC cxx_std_17)
target_link_libraries(charged_ui_kit PUBLIC charged_launcher_ui)

add_library(charged_online_panel STATIC src/online_panel/online_panel.cpp)
target_link_libraries(charged_online_panel PUBLIC charged_online_panel_model charged_ui_kit)
if(BUILD_TESTING)
    add_executable(online_panel_draw_tests tests/online_panel_draw.cpp)
    target_link_libraries(online_panel_draw_tests PRIVATE charged_online_panel)
    add_test(NAME online_panel_draw COMMAND online_panel_draw_tests)
endif()
