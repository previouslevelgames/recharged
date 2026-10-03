include(cmake/NativeRuntime.cmake)
include(cmake/Events.cmake)
include(cmake/GameConfig.cmake)
include(cmake/Tweaks.cmake)
include(cmake/FrameTiming.cmake)
include(cmake/AnimatedCamera.cmake)
include(cmake/DebugCamera.cmake)
include(cmake/CameraAssets.cmake)
include(cmake/CameraBatch.cmake)
include(cmake/FrontendCameraAssets.cmake)

add_library(charged_game_startup STATIC src/runtime/startup.cpp src/runtime/startup_tasks.cpp src/runtime/startup_cameras.cpp src/runtime/startup_camera_assets.cpp)
target_include_directories(charged_game_startup PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(charged_game_startup PRIVATE cxx_std_20)
target_link_libraries(charged_game_startup PRIVATE charged_decomp_startup charged_host
    charged_events charged_game_config charged_tweaks charged_frame_timing charged_cameras charged_camera_assets charged_frontend_camera_assets charged_animated_camera aurora::dvd aurora::os aurora::vi aurora::core mscharged_build_info)
target_link_libraries(mscharged PRIVATE charged_game_startup)
target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_GAME_STARTUP=1)

# Compile the full original entry unit separately. It is deliberately not linked
# into the startup prototype: its task globals and other services are unfinished.
add_library(charged_game_entry OBJECT EXCLUDE_FROM_ALL "${MSCHARGED_PREPARED}/src/Game/main.cpp")
add_dependencies(charged_game_entry verify_prepared)
target_include_directories(charged_game_entry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
# The entry includes physics headers; retain upstream's single-precision ODE ABI.
target_compile_definitions(charged_game_entry PRIVATE MSCHARGED_NATIVE=1 dSINGLE=1)
target_compile_features(charged_game_entry PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_game_entry PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_custom_target(charged_game_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:FILEPATH=$<TARGET_OBJECTS:charged_game_entry>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/game-entry-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_game_entry
    COMMENT "Record unresolved references from the compiled original game entry (not a link check)"
    VERBATIM)

if(BUILD_TESTING)
    add_executable(sanim_decode_tests tests/sanim_decode.cpp)
    target_compile_features(sanim_decode_tests PRIVATE cxx_std_17)
    target_link_libraries(sanim_decode_tests PRIVATE charged_sanim_decode)
    add_test(NAME sanim_decode COMMAND sanim_decode_tests)

    add_executable(native_allocator_tests tests/native_allocator.cpp)
    target_compile_features(native_allocator_tests PRIVATE cxx_std_17)
    target_link_libraries(native_allocator_tests PRIVATE charged_native_allocator)
    add_test(NAME native_allocator COMMAND native_allocator_tests)

    add_executable(runtime_memory_tests tests/runtime_memory.cpp)
    target_compile_features(runtime_memory_tests PRIVATE cxx_std_20)
    target_compile_definitions(runtime_memory_tests PRIVATE MSCHARGED_NATIVE=1)
    target_link_libraries(runtime_memory_tests PRIVATE charged_game_startup charged_decomp_startup
        aurora::os aurora::core)
    add_test(NAME runtime_memory COMMAND runtime_memory_tests)
    set_tests_properties(runtime_memory PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 45)
    add_executable(runtime_files_tests tests/runtime_files.cpp)
    target_compile_features(runtime_files_tests PRIVATE cxx_std_20)
    target_link_libraries(runtime_files_tests PRIVATE charged_game_startup charged_game_config charged_camera_assets charged_decomp_startup
        aurora::dvd aurora::core)
    add_test(NAME runtime_files
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_files.py"
            "$<TARGET_FILE:runtime_files_tests>")
    set_tests_properties(runtime_files PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 45)
    add_executable(startup_header_tests tests/startup_headers.cpp)
    target_include_directories(startup_header_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(startup_header_tests PRIVATE MSCHARGED_NATIVE=1)
    target_compile_features(startup_header_tests PRIVATE cxx_std_17)
    add_dependencies(startup_header_tests verify_prepared)
    add_test(NAME startup_headers COMMAND startup_header_tests)
    add_test(NAME game_startup
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_startup.py" "$<TARGET_FILE:mscharged>")
    set_tests_properties(game_startup PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 45)
endif()
