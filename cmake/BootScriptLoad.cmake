include_guard(GLOBAL)
include(cmake/Bytecode.cmake)
add_library(charged_boot_script_load STATIC src/runtime/boot_script_load.cpp)
target_link_libraries(charged_boot_script_load PUBLIC charged_bytecode_reader charged_decomp_startup)
if(BUILD_TESTING)
    add_executable(boot_script_load_tests tests/boot_script_load.cpp)
    target_link_libraries(boot_script_load_tests PRIVATE charged_boot_script_load aurora::dvd aurora::core)
    add_test(NAME boot_script_load
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_boot_script_load.py"
            "$<TARGET_FILE:boot_script_load_tests>")
    set_tests_properties(boot_script_load PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
endif()
