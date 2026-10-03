include_guard(GLOBAL)
include(cmake/Tasks.cmake)
include(cmake/GamePrint.cmake)
add_library(charged_cameras STATIC
    "${MSCHARGED_PREPARED}/src/Game/Camera/CameraCore.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/CameraMath.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/BaseCam.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/rumblefilter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/noisefilter.cpp"
    src/runtime/cameras.cpp)
add_dependencies(charged_cameras verify_prepared)
target_link_libraries(charged_cameras PUBLIC charged_tasks charged_game_print)
target_compile_features(charged_cameras PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_cameras PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
if(BUILD_TESTING)
    add_executable(camera_core_tests tests/camera_core.cpp tests/task_clock.cpp)
    target_include_directories(camera_core_tests PRIVATE "${MSCHARGED_PREPARED}/src")
    target_link_libraries(camera_core_tests PRIVATE charged_cameras)
    add_test(NAME camera_core COMMAND camera_core_tests)
    set_tests_properties(camera_core PROPERTIES TIMEOUT 30)
endif()
