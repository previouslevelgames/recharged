include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

add_library(charged_native_ios STATIC src/platform/ios_device.cpp)
add_dependencies(charged_native_ios verify_prepared)
target_include_directories(charged_native_ios PUBLIC src
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_ios PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_native_ios PUBLIC cxx_std_17)
target_link_libraries(charged_native_ios PUBLIC charged_native_interrupts)

if(BUILD_TESTING)
    add_executable(native_ios_tests tests/native_ios.cpp)
    target_link_libraries(native_ios_tests PRIVATE charged_native_ios)
    add_test(NAME native_ios COMMAND native_ios_tests)
    set_tests_properties(native_ios PROPERTIES TIMEOUT 30)
endif()
