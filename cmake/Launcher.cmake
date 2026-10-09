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
add_executable(mscharged src/launcher/main.cpp src/launcher/ui_kit.cpp)
target_link_libraries(mscharged PRIVATE charged_host charged_launcher_ui charged_app_icon mscharged_build_info
    charged_wiimote_scan)
if(WIN32)
    enable_language(RC)
    configure_file(cmake/app_icon.rc.in generated/mscharged/app_icon.rc @ONLY)
    target_sources(mscharged PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged/app_icon.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/mscharged/app_icon.rc"
        PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.ico")
endif()
add_custom_command(TARGET mscharged POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mscharged>/assets/launcher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.png"
            "${MSCHARGED_IMGUI_PREPARED}/misc/fonts/Roboto-Medium.ttf"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/README.md"
            "$<TARGET_FILE_DIR:mscharged>/assets/launcher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Apache-2.0.txt"
            "$<TARGET_FILE_DIR:mscharged>/assets/launcher/LICENSE-APACHE"
    VERBATIM)
# Resource edits must refresh the copied files even without a C++ source edit.
set_property(TARGET mscharged APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Apache-2.0.txt"
    "${MSCHARGED_IMGUI_PREPARED}/misc/fonts/Roboto-Medium.ttf")

if(BUILD_TESTING)
    add_test(NAME launcher_smoke
        COMMAND mscharged --smoke-test --config "${CMAKE_CURRENT_BINARY_DIR}/launcher-test-missing.ini")
    set_tests_properties(launcher_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 30)
    add_executable(launcher_ui_metrics_tests tests/launcher_ui_metrics.cpp)
    target_include_directories(launcher_ui_metrics_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME launcher_ui_metrics COMMAND launcher_ui_metrics_tests)
endif()
