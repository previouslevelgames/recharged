include_guard(GLOBAL)
if(NOT BUILD_TESTING OR NOT TARGET aurora_gx)
    return()
endif()
include(cmake/NativeInterrupts.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(native_wii_lighting_tests tests/native_wii_lighting.cpp)
add_dependencies(native_wii_lighting_tests verify_prepared)
target_compile_features(native_wii_lighting_tests PRIVATE cxx_std_20)
target_include_directories(native_wii_lighting_tests PRIVATE
    src "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(native_wii_lighting_tests PRIVATE TARGET_PC=1)
target_link_libraries(native_wii_lighting_tests PRIVATE
    charged_native_interrupt_controller
    aurora::gx aurora::os aurora::core)
add_test(NAME native_wii_lighting COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/run_native_wii_lighting.py"
    --executable "$<TARGET_FILE:native_wii_lighting_tests>")
set_tests_properties(native_wii_lighting PROPERTIES TIMEOUT 30
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
