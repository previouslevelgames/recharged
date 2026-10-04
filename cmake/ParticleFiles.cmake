include_guard(GLOBAL)
include(cmake/CompressedAssets.cmake)
add_library(charged_particle_files STATIC src/runtime/particle_files.cpp)
target_include_directories(charged_particle_files PUBLIC src)
target_compile_features(charged_particle_files PUBLIC cxx_std_20)
target_link_libraries(charged_particle_files PUBLIC charged_compressed_assets charged_decomp_startup)
if(BUILD_TESTING)
    add_executable(particle_files_tests tests/particle_files.cpp)
    target_link_libraries(particle_files_tests PRIVATE charged_particle_files aurora::dvd aurora::core)
    add_test(NAME particle_files
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_particle_files.py"
            "$<TARGET_FILE:particle_files_tests>")
    set_tests_properties(particle_files PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
endif()
