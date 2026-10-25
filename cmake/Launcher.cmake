if(TARGET imgui)
    # A single ImGui implementation serves Aurora/WGPU and the SDL launcher.
    add_library(charged_launcher_ui INTERFACE)
    target_link_libraries(charged_launcher_ui INTERFACE imgui SDL3::SDL3)
    target_include_directories(charged_launcher_ui INTERFACE
        "${MSCHARGED_IMGUI_PREPARED}/backends" "${MSCHARGED_IMGUI_PREPARED}/misc/cpp")
else()
    mscharged_prepare_dependency(imgui MSCHARGED_IMGUI_PREPARED)

    add_library(charged_launcher_ui STATIC
        "${MSCHARGED_IMGUI_PREPARED}/imgui.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/imgui_draw.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/imgui_tables.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/imgui_widgets.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/misc/cpp/imgui_stdlib.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/backends/imgui_impl_sdl3.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/backends/imgui_impl_sdlrenderer3.cpp"
    )
    add_dependencies(charged_launcher_ui verify_prepared)
    target_include_directories(charged_launcher_ui PUBLIC
        "${MSCHARGED_IMGUI_PREPARED}" "${MSCHARGED_IMGUI_PREPARED}/backends"
        "${MSCHARGED_IMGUI_PREPARED}/misc/cpp")
    target_link_libraries(charged_launcher_ui PUBLIC SDL3::SDL3)

endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WiimoteScan.cmake")
# The run's log file and crash report (logs/recharged.log), its log window, and
# reopening the launcher when the game closes (it ends the log).
add_library(charged_session_log STATIC src/platform/session_log.cpp src/platform/log_window.cpp
    src/platform/launcher_reopen.cpp)
target_include_directories(charged_session_log PUBLIC src)
target_compile_features(charged_session_log PRIVATE cxx_std_20)
target_link_libraries(charged_session_log PUBLIC SDL3::SDL3)
if(NOT WIN32 AND NOT APPLE)
    # dladdr names the module of each crash stack frame.
    target_link_libraries(charged_session_log PRIVATE ${CMAKE_DL_LIBS})
endif()
if(NOT WIN32)
    add_executable(session_log_tests tests/session_log_tests.cpp)
    target_compile_features(session_log_tests PRIVATE cxx_std_20)
    target_link_libraries(session_log_tests PRIVATE charged_session_log)
    add_test(NAME session_log COMMAND session_log_tests)
    add_executable(launcher_reopen_tests tests/launcher_reopen.cpp)
    target_compile_features(launcher_reopen_tests PRIVATE cxx_std_20)
    target_link_libraries(launcher_reopen_tests PRIVATE charged_session_log)
    add_test(NAME launcher_reopen COMMAND launcher_reopen_tests)
endif()
# The graphics cards the launcher offers (Advanced -> Graphics card): DXGI on
# Windows, the Vulkan loader on Linux (its headers come with Dawn), none on macOS.
add_library(charged_graphics_cards STATIC src/platform/graphics_cards.cpp)
target_include_directories(charged_graphics_cards PUBLIC src)
target_compile_features(charged_graphics_cards PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(charged_graphics_cards PRIVATE dxgi)
elseif(NOT APPLE AND DEFINED MSCHARGED_DAWN_PREPARED)
    target_compile_definitions(charged_graphics_cards PRIVATE MSCHARGED_GRAPHICS_CARDS_VULKAN=1)
    target_include_directories(charged_graphics_cards PRIVATE
        "${MSCHARGED_DAWN_PREPARED}/third_party/vulkan-headers/src/include")
    target_link_libraries(charged_graphics_cards PRIVATE ${CMAKE_DL_LIBS})
endif()
if(BUILD_TESTING)
    add_executable(graphics_cards_tests tests/graphics_cards.cpp)
    target_link_libraries(graphics_cards_tests PRIVATE charged_graphics_cards)
    add_test(NAME graphics_cards COMMAND graphics_cards_tests)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Updates.cmake")
# Finding and checking disc images: the first-start search and the image library (#65).
add_library(charged_image_library STATIC src/launcher/disc_search.cpp src/launcher/image_library.cpp
    src/launcher/release_banner.cpp)
target_include_directories(charged_image_library PUBLIC src)
target_compile_features(charged_image_library PUBLIC cxx_std_17)
target_link_libraries(charged_image_library PUBLIC charged_host charged_json)
# Match presets: named match settings in files, written into the game save (the Matches page).
add_library(charged_match_presets STATIC src/launcher/match_presets.cpp)
target_include_directories(charged_match_presets PUBLIC src)
target_compile_features(charged_match_presets PUBLIC cxx_std_17)
target_link_libraries(charged_match_presets PUBLIC charged_host)
# New Miis for players without any (the Miis page).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MiiMaker.cmake")
add_executable(mscharged src/launcher/main.cpp src/launcher/ui_kit.cpp src/launcher/release_notes_view.cpp)
# The program players start: recharged (recharged.exe). The target keeps its old name.
# On Windows it is a Windows (GUI) program, so no console opens with it (#61).
set_target_properties(mscharged PROPERTIES OUTPUT_NAME recharged WIN32_EXECUTABLE TRUE)
target_link_libraries(mscharged PRIVATE charged_host charged_launcher_ui charged_app_icon mscharged_build_info
    charged_wiimote_scan charged_session_log charged_updates charged_image_library charged_graphics_cards
    charged_texture_packs charged_mii_maker charged_match_presets)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PipelineCatalog.cmake")
mscharged_stage_pipeline_catalog(mscharged)
if(APPLE)
    target_sources(mscharged PRIVATE src/platform/macos_app.cpp)
    target_link_libraries(mscharged PRIVATE "-framework CoreFoundation")
    # Recharged.app beside the program, so Finder starts it without Terminal
    # (#61). It holds only a copy of the program, Info.plist and the icon;
    # SDL_FILESYSTEM_BASE_DIR_TYPE=parent keeps everything else beside it.
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MSCHARGED_APP_MINIMUM_SYSTEM
            "\n    <key>LSMinimumSystemVersion</key>\n    <string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
    endif()
    configure_file(cmake/Info.plist.in generated/mscharged/Info.plist @ONLY)
    find_program(MSCHARGED_CODESIGN codesign REQUIRED)
    set(app_bundle "$<TARGET_FILE_DIR:mscharged>/Recharged.app")
    add_custom_command(TARGET mscharged POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${app_bundle}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${app_bundle}/Contents/MacOS" "${app_bundle}/Contents/Resources"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:mscharged>" "${app_bundle}/Contents/MacOS/recharged"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged/Info.plist"
                "${app_bundle}/Contents/Info.plist"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.icns"
                "${app_bundle}/Contents/Resources/recharged.icns"
        # Signed (ad hoc) as a whole, so the signature also covers Info.plist.
        COMMAND "${MSCHARGED_CODESIGN}" --force --sign - "${app_bundle}"
        VERBATIM)
    set_property(TARGET mscharged APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.icns" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in")
endif()
if(WIN32)
    enable_language(RC)
    configure_file(cmake/app_icon.rc.in generated/mscharged/app_icon.rc @ONLY)
    target_sources(mscharged PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged/app_icon.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged/app_icon.rc"
        PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.ico")
endif()
# Aurora's logo for the About page, from its pinned prepared source (MIT).
if(NOT MSCHARGED_AURORA_PREPARED)
    mscharged_prepare_dependency(aurora MSCHARGED_AURORA_PREPARED)
endif()
add_custom_command(TARGET mscharged POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mscharged>/assets/launcher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/logo.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.png"
            "${MSCHARGED_IMGUI_PREPARED}/misc/fonts/Roboto-Medium.ttf"
            "${MSCHARGED_AURORA_PREPARED}/assets/aurora.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/welcome.md"
            "$<TARGET_FILE_DIR:mscharged>/assets/launcher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Apache-2.0.txt"
            "$<TARGET_FILE_DIR:mscharged>/assets/launcher/LICENSE-APACHE"
    # Gameplay data the host passes to the game module (tweak "Robot goalies").
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mscharged>/assets/gameplay"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/gameplay/robot_goalie.map"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/gameplay/README.md"
            "$<TARGET_FILE_DIR:mscharged>/assets/gameplay"
    VERBATIM)
# Resource edits must refresh the copied files even without a C++ source edit.
set_property(TARGET mscharged APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/logo.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/welcome.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Apache-2.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/gameplay/robot_goalie.map"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/gameplay/README.md"
    "${MSCHARGED_IMGUI_PREPARED}/misc/fonts/Roboto-Medium.ttf"
    "${MSCHARGED_AURORA_PREPARED}/assets/aurora.png")

if(BUILD_TESTING)
    add_test(NAME launcher_smoke
        COMMAND mscharged --smoke-test --config "${CMAKE_CURRENT_BINARY_DIR}/launcher-test-missing.ini")
    set_tests_properties(launcher_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 30)
    add_executable(launcher_ui_metrics_tests tests/launcher_ui_metrics.cpp)
    target_include_directories(launcher_ui_metrics_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME launcher_ui_metrics COMMAND launcher_ui_metrics_tests)
    add_executable(launcher_match_presets_tests tests/launcher_match_presets.cpp)
    target_link_libraries(launcher_match_presets_tests PRIVATE charged_match_presets)
    add_test(NAME launcher_match_presets COMMAND launcher_match_presets_tests
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-match-presets-data")
    add_executable(launcher_player_lineup_tests tests/launcher_player_lineup.cpp)
    target_include_directories(launcher_player_lineup_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME launcher_player_lineup COMMAND launcher_player_lineup_tests)
    add_executable(launcher_disc_search_tests tests/launcher_disc_search.cpp)
    target_link_libraries(launcher_disc_search_tests PRIVATE charged_image_library)
    add_test(NAME launcher_disc_search COMMAND launcher_disc_search_tests)
    # The library's scan, cache and order, and banners and probes of synthetic discs.
    add_executable(launcher_image_library_tests tests/launcher_image_library.cpp)
    target_link_libraries(launcher_image_library_tests PRIVATE charged_image_library)
    add_test(NAME launcher_image_library
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_image_library.py"
                "$<TARGET_FILE:launcher_image_library_tests>")
    set_tests_properties(launcher_image_library PROPERTIES TIMEOUT 120)
endif()
