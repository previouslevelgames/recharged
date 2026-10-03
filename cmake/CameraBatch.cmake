include_guard(GLOBAL)
include(cmake/CameraAssets.cmake)
add_library(charged_camera_batch STATIC src/runtime/camera_batch.cpp)
target_link_libraries(charged_camera_batch PUBLIC charged_camera_assets)
target_compile_features(charged_camera_batch PUBLIC cxx_std_20)
if(BUILD_TESTING)
    add_executable(camera_batch_tests tests/camera_batch.cpp)
    target_link_libraries(camera_batch_tests PRIVATE charged_camera_batch aurora::dvd aurora::core)
    add_test(NAME camera_batch
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_camera_batch.py"
            "$<TARGET_FILE:camera_batch_tests>")
    set_tests_properties(camera_batch PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
endif()
