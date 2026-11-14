# The data of Recharged's menus (src/overlay): the online pages' model from
# the profile service's lists (charged_overlay_model), players' pictures
# (charged_overlay_images), the challenge setup and the gameplay options'
# groups (charged_overlay); the menus themselves are the UI kit's (src/ui,
# charged_recharged_ui).
include_guard(GLOBAL)
add_library(charged_overlay_model STATIC src/overlay/online_panel_model.cpp src/overlay/flags.cpp)
target_include_directories(charged_overlay_model PUBLIC src)
target_compile_features(charged_overlay_model PUBLIC cxx_std_17)
target_link_libraries(charged_overlay_model PUBLIC charged_competitive charged_texts)
if(BUILD_TESTING)
    add_executable(online_panel_tests tests/online_panel.cpp)
    target_link_libraries(online_panel_tests PRIVATE charged_overlay_model)
    add_test(NAME online_panel COMMAND online_panel_tests)
endif()

# The launcher's palette and widgets (pure ImGui), and the fallback fonts the game's menus share.
add_library(charged_ui_kit STATIC src/launcher/ui_kit.cpp src/launcher/fallback_fonts.cpp)
target_include_directories(charged_ui_kit PUBLIC src)
target_compile_features(charged_ui_kit PUBLIC cxx_std_17)
target_link_libraries(charged_ui_kit PUBLIC charged_launcher_ui charged_name_glyphs)

# Players' pictures (their avatars): downloaded (charged_http) and decoded
# (SDL's PNG) on a thread of its own, made textures by the host when drawn.
include("${CMAKE_CURRENT_LIST_DIR}/Http.cmake")
add_library(charged_overlay_images STATIC src/overlay/overlay_images.cpp)
target_include_directories(charged_overlay_images PUBLIC src)
target_compile_features(charged_overlay_images PUBLIC cxx_std_17)
target_link_libraries(charged_overlay_images PUBLIC charged_launcher_ui charged_http Threads::Threads)
if(BUILD_TESTING)
    add_executable(overlay_images_tests tests/overlay_images.cpp)
    target_link_libraries(overlay_images_tests PRIVATE charged_overlay_images)
    add_test(NAME overlay_images COMMAND overlay_images_tests)
endif()

add_library(charged_overlay STATIC src/overlay/online_panel.cpp src/overlay/game_options.cpp)
target_link_libraries(charged_overlay PUBLIC charged_overlay_model charged_overlay_images)

# Recharged's menus in the look of the game's own (src/ui, the UI kit): the
# TV set, its plates and rows, and the pages (OPTIONS, GAMEPLAY, GRAPHICS,
# SOUND, the Recharged menu), drawn with ImGui in the game; the pages' models
# are the overlay's.
add_library(charged_recharged_ui STATIC src/ui/rui_kit.cpp src/ui/rui_pages.cpp)
target_link_libraries(charged_recharged_ui PUBLIC charged_overlay charged_texts)
