include_guard(GLOBAL)
include(cmake/CameraBatch.cmake)
add_library(charged_frontend_camera_assets STATIC src/runtime/frontend_camera_assets.cpp)
add_dependencies(charged_frontend_camera_assets verify_prepared)
target_link_libraries(charged_frontend_camera_assets PUBLIC charged_camera_batch)
target_compile_features(charged_frontend_camera_assets PUBLIC cxx_std_20)
if(BUILD_TESTING)
    add_executable(frontend_camera_assets_tests tests/frontend_camera_assets.cpp)
    target_link_libraries(frontend_camera_assets_tests PRIVATE charged_frontend_camera_assets charged_animated_camera aurora::dvd aurora::core)
    add_test(NAME frontend_camera_assets
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_camera_assets.py"
            "$<TARGET_FILE:frontend_camera_assets_tests>")
    set_tests_properties(frontend_camera_assets PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
endif()
