include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

# One explicitly initialized virtual channel0/device1 endpoint. Declaring the
# target does not create a backing image or initialize the original SRAM cache.
add_library(charged_native_rtc STATIC src/platform/rtc_device.cpp)
add_dependencies(charged_native_rtc verify_prepared)
target_include_directories(charged_native_rtc PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_rtc PUBLIC TARGET_PC=1)
target_compile_features(charged_native_rtc PUBLIC cxx_std_17)
target_link_libraries(charged_native_rtc PUBLIC charged_native_interrupts aurora::os)

# Complete original software/cache/retry owner. Kept as an explicit inventory;
# callers must configure the hardware before issuing the original SRAM reads.
add_library(charged_original_rtc STATIC EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSRtc.c")
add_dependencies(charged_original_rtc verify_prepared)
target_include_directories(charged_original_rtc PRIVATE
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_rtc PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_rtc PRIVATE c_std_17)
target_link_libraries(charged_original_rtc PUBLIC charged_native_rtc)
set_target_properties(charged_original_rtc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_rtc PRIVATE -fexceptions -fwrapv
        -fno-strict-aliasing -Wno-unknown-pragmas)
endif()

if(BUILD_TESTING)
    include(cmake/NativeAlarms.cmake)
    include(cmake/NativeSystemSettings.cmake)
    # Fixture includes the whole original TU to expose only a paired private
    # LockSram/UnlockSram leaf, using bytes read by the original EXI requests.
    add_executable(native_rtc_tests tests/native_rtc.cpp tests/rtc_source.c)
    add_dependencies(native_rtc_tests verify_prepared charged_original_rtc)
    target_include_directories(native_rtc_tests PRIVATE "${MSCHARGED_PREPARED}"
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_rtc_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_rtc_tests PRIVATE cxx_std_20 c_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(native_rtc_tests PRIVATE -fexceptions -fwrapv -fno-strict-aliasing)
    endif()
    target_link_libraries(native_rtc_tests PRIVATE charged_native_rtc charged_native_alarms)
    add_test(NAME native_rtc COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_rtc.py"
        "$<TARGET_FILE:native_rtc_tests>")
    set_tests_properties(native_rtc PROPERTIES TIMEOUT 20 LABELS "Platform")

    add_executable(native_rtc_runtime_tests tests/native_rtc_runtime.cpp)
    target_compile_definitions(native_rtc_runtime_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_rtc_runtime_tests PRIVATE cxx_std_20)
    target_link_libraries(native_rtc_runtime_tests PRIVATE charged_original_rtc
        charged_native_alarms charged_native_system_settings charged_native_interrupt_controller)
    add_test(NAME native_rtc_runtime COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_rtc.py"
        "$<TARGET_FILE:native_rtc_runtime_tests>")
    set_tests_properties(native_rtc_runtime PROPERTIES TIMEOUT 20 LABELS "Platform")
endif()
