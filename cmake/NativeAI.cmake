include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

add_library(charged_native_ai STATIC src/platform/ai.cpp)
add_dependencies(charged_native_ai verify_prepared)
target_include_directories(charged_native_ai PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_ai PUBLIC TARGET_PC=1)
target_compile_features(charged_native_ai PUBLIC cxx_std_20)
target_link_libraries(charged_native_ai PUBLIC charged_native_interrupts
    PRIVATE SDL3::SDL3)

if(BUILD_TESTING)
    add_executable(native_ai_tests tests/native_ai.cpp)
    target_link_libraries(native_ai_tests PRIVATE charged_native_ai SDL3::SDL3)
    add_test(NAME native_ai COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_ai.py"
        "$<TARGET_FILE:native_ai_tests>")
    set_tests_properties(native_ai PROPERTIES TIMEOUT 60)
endif()
