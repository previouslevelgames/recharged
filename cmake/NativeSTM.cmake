include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)
include(cmake/NativeIOS.cmake)

# Original Wii state manager owns callbacks and reset pulse behavior. Native IOS
# endpoints own physical/UI event transport; full VI/shutdown providers remain
# explicit dependencies. Host endpoint setup + original __OSInitSTM must precede
# original ResetTask construction. This does not implement a replacement reset task.
add_library(charged_native_stm STATIC EXCLUDE_FROM_ALL
    src/platform/stm_device.cpp
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSStateTM.c")
add_dependencies(charged_native_stm verify_prepared)
target_include_directories(charged_native_stm PUBLIC src PRIVATE
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_stm PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_native_stm PRIVATE c_std_99 cxx_std_20)
target_compile_options(charged_native_stm PRIVATE -ffunction-sections -fdata-sections
    -Wno-unknown-pragmas)
target_link_libraries(charged_native_stm PUBLIC charged_native_interrupts charged_native_ios aurora::os)

if(BUILD_TESTING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND UNIX AND NOT APPLE)
    add_executable(native_stm_device_tests tests/native_stm_device.cpp)
    target_include_directories(native_stm_device_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_stm_device_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_stm_device_tests PRIVATE cxx_std_20)
    target_link_libraries(native_stm_device_tests PRIVATE charged_native_stm)
    # Preserve original cold hardware functions; no fake VI/shutdown provider.
    target_link_options(native_stm_device_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME native_stm_device COMMAND native_stm_device_tests)
    set_tests_properties(native_stm_device PROPERTIES TIMEOUT 30)
endif()
